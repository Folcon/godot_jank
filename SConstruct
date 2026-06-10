#!/usr/bin/env python
import os, glob, subprocess

# godot-cpp from the submodule (gives CPPPATH, per-platform flags + suffix logic).
env = SConscript("godot-cpp/SConstruct")
env.Append(CPPPATH=["src/"])

platform = env["platform"]

# --- locate jank (override with JANK_HOME) ----------------------------------
# jank is a build- AND run-time dependency; it is NOT vendored. macOS/Linux can use
# brew; everything else must set JANK_HOME. Lib dir is versioned: <home>/lib/jank/<ver>.
jank_home = os.environ.get("JANK_HOME")
if not jank_home and platform in ("macos", "linux"):
    jank_home = subprocess.run(
        ["brew", "--prefix", "jank"], capture_output=True, text=True).stdout.strip()
assert jank_home and os.path.isdir(jank_home), \
    "jank not found - set JANK_HOME (or `brew install jank` on macOS/Linux)."
jank_libdirs = sorted(glob.glob(os.path.join(jank_home, "lib", "jank", "*")))
assert jank_libdirs, "no <JANK_HOME>/lib/jank/<version> found."
JANK = jank_libdirs[-1]
env.Append(CPPPATH=[JANK + "/include"])

# jank's headers need libgc visible (immer's gc_heap); same on every platform.
env.Append(CPPDEFINES=["IMMER_HAS_LIBGC=1"])

if platform in ("macos", "linux"):
    # clang/gcc: jank reports errors by throwing, and its headers use C++20 concepts.
    env.Append(CXXFLAGS=["-fexceptions", "-std=c++20"])
    env.Append(LIBPATH=[JANK + "/lib"])
    env.Append(LINKFLAGS=[
        JANK + "/lib/libjank-standalone.a",
        "-lclang-cpp", "-lLLVM", "-lunwind", "-lz", "-lcrypto",
        "-Wl,-rpath," + JANK + "/lib",   # find jank's shared libs at runtime
    ])
    if platform == "macos":
        ssl = subprocess.run(
            ["brew", "--prefix", "openssl@3"], capture_output=True, text=True).stdout.strip()
        if ssl:
            env.Append(LIBPATH=[ssl + "/lib"])
      # Also search next to the extension binary, so an exported game finds jank's
      #   dylibs when copied beside the .dylib (the absolute rpath above is dev-only)
      env.Append(LINKFLAGS=["-Wl,-rpath,@loader_path"])
  else:  # linux: -lcrypto / -lunwind resolve against system libs
      env.Append(LINKFLAGS=["-Wl,-rpath,$$ORIGIN", "-Wl,-z,origin"])

elif platform == "windows":
    # /!\ UNVALIDATED. jank's Windows support is immature and embedding its JIT in
    #     Godot here is unproven. This branch is a starting point, NOT a working build.
    #     Expect to revisit the toolchain (clang-cl vs MSVC), the import libs, and the
    #     fact that Windows has no rpath (jank's .dll's must sit next to the binary).
    env.Append(CXXFLAGS=["/std:c++20", "/EHsc"])
    env.Append(LIBPATH=[JANK + "/lib"])
    env.Append(LINKFLAGS=[os.path.join(JANK, "lib", "jank-standalone.lib")])

else:
    raise SystemExit("godot_jank: unsupported platform '%s' (jank is native desktop "
                     "only; wasm/web needs the separate eval-free-AOT path)." % platform)

# NOTE: jank is native per-arch and its PCH is per-arch - the build `arch=` MUST match
# your installed jank (e.g. don't `arch=x86_64` against an arm64 jank).

libname = "libgodot_jank{}{}".format(env["suffix"], env["SHLIBSUFFIX"])
lib = env.SharedLibrary("addons/godot_jank/bin/" + libname, source=Glob("src/*.cpp"))
Default(lib)
