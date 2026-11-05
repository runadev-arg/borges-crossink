"""Merges the borges-qemu image into a 16 MB flash and boots it in Espressif's QEMU.

    python -I test/borges_qemu/run.py --name full --plan "stack=16384" [--secs 300] [--fresh]

The plan goes into the last 64 KB of app1, where the image reads it. The flash image keeps
LittleFS between runs unless --fresh. Output: <work>/<name>.log (UART0).

Memory and stack numbers are the device code's. Times come from QEMU's instruction count,
8 ns each, and are an order of magnitude at best. A run can still freeze with every task
idle (about one in five); it is worth a retry and says nothing about the code under test.
"""
import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
QEMU = os.environ.get("BORGES_QEMU", r"D:\CrossInkBorges\crossink-tools\qemu\qemu\bin\qemu-system-riscv32.exe")
ESPTOOL = os.environ.get("BORGES_ESPTOOL", r"D:\CrossInkBorges\pio-core\packages\tool-esptoolpy\esptool.py")
BOOT_APP0 = os.environ.get(
    "BORGES_BOOT_APP0", r"D:\CrossInkBorges\pio-core\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin"
)
WORK = Path(os.environ.get("BORGES_QEMU_WORK", r"D:\CrossInkBorges\tmp\qemu\step2"))
BUILD_ROOT = Path(os.environ.get("BORGES_QEMU_BUILD", r"D:/CrossInkBorges/tmp/qemu/ws/build"))
PLAN_OFFSET = 0x650000 + 0x640000 - 0x10000
# Chip revision 0.3: the start of the default of idf.py qemu for esp32c3.
EFUSE = bytes.fromhex("00" * 36 + "00000c00") + bytes(1024 - 40)


def merge(env: str, out: Path):
    build = BUILD_ROOT / env
    subprocess.run(
        [sys.executable, "-I", ESPTOOL, "--chip", "esp32c3", "merge-bin", "--flash-mode", "dio", "--flash-size", "16MB",
         "--flash-freq", "80m", "--pad-to-size", "16MB", "-o", str(out), "0x0", str(build / "bootloader.bin"), "0x8000",
         str(build / "partitions.bin"), "0xe000", BOOT_APP0, "0x10000", str(build / "firmware.bin")],
        check=True, capture_output=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--env", default="borges-qemu")
    parser.add_argument("--name", required=True)
    parser.add_argument("--plan", default="")
    parser.add_argument("--secs", type=int, default=300)
    parser.add_argument("--fresh", action="store_true")
    parser.add_argument("--no-nic", action="store_true")
    parser.add_argument("--watch", default="@@ DONE")
    args = parser.parse_args()

    WORK.mkdir(parents=True, exist_ok=True)
    flash = WORK / f"flash-{args.env}.bin"
    elf_stamp = (BUILD_ROOT / args.env / "firmware.bin").stat().st_mtime
    if args.fresh or not flash.exists() or flash.stat().st_mtime < elf_stamp:
        merge(args.env, flash)
    plan = args.plan.replace(";", "\n").encode() + b"\n\xff"
    with open(flash, "r+b") as f:
        f.seek(PLAN_OFFSET)
        f.write(plan + b"\xff" * (512 - len(plan)))
    efuse = WORK / "efuse.bin"
    efuse.write_bytes(EFUSE)

    log = WORK / f"{args.name}.log"
    log.unlink(missing_ok=True)
    cmd = [QEMU, "-M", "esp32c3", "-nographic", "-monitor", "none",
           "-drive", f"file={flash},if=mtd,format=raw",
           "-drive", f"file={efuse},if=none,format=raw,id=efuse",
           "-global", "driver=nvram.esp32c3.efuse,property=drive,value=efuse",
           # As idf.py qemu does. QEMU's timers run on host time while its CPU does not.
           "-global", "driver=timer.esp32c3.timg,property=wdt_disable,value=true",
           # Guest time from instruction count, 8 ns each (125 MIPS, the same order as a 160 MHz
           # C3). Without it a busy host makes the emulated systimer miss its alarm: the
           # scheduler tick stops for good and every task sits idle.
           "-icount", "shift=3,sleep=on",
           "-serial", f"file:{log}",
           "-nic", "none" if args.no_nic else "user,model=open_eth"]
    print(" ".join(cmd), flush=True)
    process = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    started = time.time()
    outcome = "timeout"
    try:
        while time.time() - started < args.secs:
            time.sleep(1)
            if process.poll() is not None:
                outcome = f"qemu exited {process.returncode}"
                break
            if log.exists():
                text = log.read_bytes()
                if args.watch.encode() in text:
                    time.sleep(1)
                    outcome = "done"
                    break
                if text.count(b"Rebooting...") >= 1 or text.count(b"Guru Meditation") >= 1:
                    time.sleep(2)
                    outcome = "panic"
                    break
    finally:
        process.kill()
    print(f"outcome={outcome} after {time.time() - started:.0f}s log={log}")
    if log.exists():
        for line in log.read_text(errors="replace").splitlines():
            if line.startswith("@@") or "SecureClient" in line or "Guru" in line or "panic" in line.lower() or "abort" in line:
                print(line)


if __name__ == "__main__":
    main()
