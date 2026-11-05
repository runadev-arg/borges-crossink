"""
PlatformIO pre script for [env:simulator-windows]: force-includes the simulator's
Windows shim into every translation unit. It lives here rather than in build_flags
because PlatformIO resolves a relative -include against the platform package.
"""

Import("env")  # noqa: F821 - SCons injects this at build time

import os

preinclude = os.path.normpath(
    os.path.join(env.subst("$PROJECT_DIR"), "..", "crossink-simulator", "src", "SimWindowsPreinclude.h")
)

# One argument: clang accepts -include<file>, and SCons cannot reorder or split it.
env.Append(CCFLAGS=["-include" + preinclude.replace("\\", "/")])
