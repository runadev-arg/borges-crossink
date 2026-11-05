"""
PlatformIO post script for [env:simulator-windows].

- Links the C++ runtime statically, so program.exe only needs SDL2.dll, and links
  SDL2 through its import library. A plain -lSDL2 would pick libSDL2.a under
  -static, and the simulator's documented runtime dependency is the DLL.
- Copies SDL2.dll next to the program, where Windows looks for it first.
"""

Import("env")  # noqa: F821 - SCons injects this at build time

import os
import shutil

project_dir = env.subst("$PROJECT_DIR")
sdl_root = os.environ.get("CROSSINK_SDL2_DIR") or os.path.join(
    project_dir, "..", "crossink-tools", "SDL2-2.32.10", "x86_64-w64-mingw32"
)
sdl_dll = os.path.normpath(os.path.join(sdl_root, "bin", "SDL2.dll"))
sdl_import_lib = os.path.normpath(os.path.join(sdl_root, "lib", "libSDL2.dll.a"))

env.Append(
    LINKFLAGS=["-static", "-mconsole"],
    LIBS=[env.File(sdl_import_lib), "ws2_32"],
)


def copy_sdl_dll(target, source, env):
    destination = os.path.join(env.subst("$BUILD_DIR"), "SDL2.dll")
    shutil.copy2(sdl_dll, destination)
    print(f"Copied {sdl_dll} -> {destination}")


env.AddPostAction("$BUILD_DIR/${PROGNAME}${PROGSUFFIX}", copy_sdl_dll)
