"""Builds the TLS probe natively: the wolfSSL that PlatformIO fetched, with the device's
user_settings.h, patches and build flags, 32-bit so its word size and pointer size match
the ESP32-C3 and the heap numbers mean something.

    python -I test/borges_tls_probe/build_probe.py [--variant full|no-alt-chains|no-sha384]

Everything the device build configures is read from the repo, never restated here:
flags from platformio.ini [base], settings from scripts/patch_wolfssl.py.
"""
import argparse
import configparser
import concurrent.futures
import os
import re
import runpy
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
WOLF = REPO / ".pio/libdeps/default/Arduino-wolfSSL/src"
SDK = REPO / "freeink-sdk/libs/network/SecureNet"
PROBE = REPO / "test/borges_tls_probe"
BUILD_ROOT = Path(os.environ.get("BORGES_PROBE_BUILD", r"D:/CrossInkBorges/tmp/tls-probe"))
CC = "i686-w64-mingw32-clang"
CXX = "i686-w64-mingw32-clang++"

# What the device gets from the framework, not from [base]. ARDUINO and ESP32 are left
# out on purpose: in settings.h they only select lwIP sockets and the Arduino time hooks,
# which are the two things the probe replaces. The non-socket defines that block sets are
# repeated here. wolfSSL's time goes through the probe so scenarios can move the clock.
HOST_DEFINES = [
    "SIZEOF_LONG_LONG=8",
    "NO_WRITEV",
    "NO_WOLFSSL_DIR",
    "WOLFSSL_NO_CURRDIR",
    "TFM_TIMING_RESISTANT",
    "ECC_TIMING_RESISTANT",
    "WC_RSA_BLINDING",
    "WC_NO_CACHE_RESISTANT",
    "TFM_NO_ASM",
    "WOLFSSL_NO_ASM",
    "FREEINK_WOLFSSL_RETAIN_INPUT_BUFFER",
    "TIME_OVERRIDES",
    "HAVE_TIME_T_TYPE",
    "HAVE_TM_TYPE",
    "XTIME=probe_xtime",
    "XGMTIME=probe_xgmtime",
    "BORGES_NET_PROBE",
]


def device_flags():
    config = configparser.ConfigParser(interpolation=None, inline_comment_prefixes=(";",))
    config.read(REPO / "platformio.ini", encoding="utf-8")
    raw = config["base"]["build_flags"]
    keep = []
    for line in raw.splitlines():
        match = re.match(r"\s*-D((?:WOLFSSL|HAVE|FREEINK_NET)\w*(?:=\S+)?)", line)
        if match:
            keep.append(match.group(1))
    if "WOLFSSL_USER_SETTINGS" not in keep:
        raise SystemExit("platformio.ini [base] no longer defines WOLFSSL_USER_SETTINGS")
    return keep


def patched_settings(out_dir: Path):
    """The device's user_settings.h, through the device's patch script."""
    class Env:
        def subst(self, value):
            return str(out_dir)

        def Append(self, **kwargs):
            pass

    module = runpy.run_path(str(REPO / "scripts/patch_wolfssl.py"), init_globals={"Import": lambda _: None, "env": Env()})
    target = out_dir / "user_settings.h"
    shutil.copyfile(WOLF / "user_settings.h", target)
    module["patch_user_settings"](target)
    return target


def compile_one(args):
    cc, source, obj, flags = args
    if obj.exists() and obj.stat().st_mtime > source.stat().st_mtime:
        return None
    obj.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run([cc, *flags, "-c", str(source), "-o", str(obj)], capture_output=True, text=True)
    if result.returncode != 0:
        return f"{source}:\n{result.stderr}"
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--variant", default="full", choices=["full", "no-alt-chains", "no-sha384"])
    options = parser.parse_args()

    if "CrossInk: keep a grown input buffer" not in (WOLF / "src/internal.c").read_text(errors="ignore"):
        raise SystemExit("internal.c is not patched: run the firmware build once first")

    build = BUILD_ROOT / options.variant
    settings_dir = build / "settings"
    settings_dir.mkdir(parents=True, exist_ok=True)
    settings = patched_settings(settings_dir)

    flags = device_flags()
    if options.variant == "no-alt-chains":
        flags = [f for f in flags if f != "WOLFSSL_ALT_CERT_CHAINS"]
    if options.variant == "no-sha384":
        text = settings.read_text()
        text = re.sub(r"#ifndef WOLFSSL_SHA384\n#define WOLFSSL_SHA384\n#endif\n#ifndef WOLFSSL_SHA512\n#define WOLFSSL_SHA512\n#endif\n", "", text)
        settings.write_text(text, newline="\n")
        flags = [f for f in flags if f != "WOLFSSL_SP_384"] + ["NO_ECC384"]
    print("device flags:", " ".join(flags))

    defines = [f"-D{d}" for d in flags + HOST_DEFINES]
    if options.variant == "no-sha384":
        defines += ["-DWOLFSSL_NOSHA512_224", "-DWOLFSSL_NOSHA512_256", "-DWOLFSSL_NOSHA384", "-DWOLFSSL_NOSHA512"]
    includes = [f"-I{settings_dir}", f"-I{WOLF}", f"-I{PROBE}/shim", f"-I{SDK}/include", f"-I{REPO}/lib/BorgesCore", f"-I{REPO}/src/borges/net"]
    # The device compiles with -Os; heap and stack do not depend on it, but speed is close enough.
    common = ["-Os", "-w", "-include", "stdio.h", "-include", "time.h", *defines, *includes]

    jobs = []
    for directory in (WOLF / "src", WOLF / "wolfcrypt/src"):
        for source in sorted(directory.glob("*.c")):
            # #included by ssl.c or misc.c style files: the sources skip themselves.
            jobs.append((CC, source, build / "wolf" / (source.stem + ".o"), common))

    cxx = ["-std=gnu++20", *common]
    native = [
        SDK / "src/SecureClient.cpp",
        REPO / "src/borges/net/BorgesTrustAnchors.cpp",
        REPO / "src/borges/net/BorgesHttp.cpp",
        REPO / "src/borges/net/BorgesDns.cpp",
        REPO / "src/borges/net/BorgesClock.cpp",
        REPO / "src/borges/net/BorgesDevice.cpp",
        REPO / "lib/BorgesCore/net/ClockChain.cpp",
        REPO / "lib/BorgesCore/net/DnsChain.cpp",
        REPO / "lib/BorgesCore/net/DnsCodec.cpp",
        REPO / "lib/BorgesCore/net/HttpDate.cpp",
        REPO / "lib/BorgesCore/net/NetFailure.cpp",
        REPO / "lib/BorgesCore/net/NetRecords.cpp",
        REPO / "lib/BorgesCore/AtomicFile.cpp",
        REPO / "lib/BorgesCore/Codec.cpp",
        PROBE / "probe_main.cpp",
    ]
    for source in native:
        jobs.append((CXX, source, build / "app" / (source.stem + ".o"), cxx))

    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
        errors = [e for e in pool.map(compile_one, jobs) if e]
    if errors:
        print("\n".join(errors[:5]))
        raise SystemExit(f"{len(errors)} files failed to compile")

    objects = [str(job[2]) for job in jobs]
    exe = build / "tls_probe.exe"
    rsp = build / "objects.rsp"
    rsp.write_text("\n".join(o.replace("\\", "/") for o in objects))
    link = [CXX, f"@{rsp}", "-o", str(exe), "-static", "-lws2_32", "-ladvapi32", "-lcrypt32"]
    subprocess.run(link, check=True)
    print("built", exe)


if __name__ == "__main__":
    sys.exit(main())
