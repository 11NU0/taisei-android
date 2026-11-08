import os
import shutil
import subprocess
import sys

TAISEI_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SDL_SOURCE = os.path.join(TAISEI_ROOT, "subprojects", "sdl3")

ABIS = {
    "arm64-v8a": "aarch64",
    "armeabi-v7a": "arm",
    "x86_64": "x86_64",
}


def run(cmd):
    print("+ %s" % " ".join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True)


def build(ndk, abi, api=24, jobs=8):
    prefix = os.path.join(TAISEI_ROOT, "build-android", "sdl3-" + abi)
    config = os.path.join(prefix, "lib", "cmake", "SDL3", "SDL3Config.cmake")
    if os.path.isfile(config):
        print("SDL3 already installed: %s" % prefix, flush=True)
        return prefix

    build_dir = os.path.join(TAISEI_ROOT, "build-android", "sdl3-build-" + abi)
    os.makedirs(build_dir, exist_ok=True)
    toolchain = os.path.join(ndk, "build", "cmake", "android.toolchain.cmake")
    cmake = shutil.which("cmake") or "cmake"
    args = [
        cmake, "-S", SDL_SOURCE, "-B", build_dir, "-G", "Ninja",
        "-DCMAKE_TOOLCHAIN_FILE=" + toolchain,
        "-DANDROID_ABI=" + abi,
        "-DANDROID_PLATFORM=android-%d" % api,
        "-DANDROID_STL=c++_shared",
        "-DANDROID_USE_LEGACY_TOOLCHAIN_FILE=OFF",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_INSTALL_PREFIX=" + prefix,
        "-DSDL_DISABLE_INSTALL=OFF",
        "-DSDL_DISABLE_UNINSTALL=ON",
        "-DSDL_RENDER=OFF",
        "-DSDL_POWER=OFF",
        "-DSDL_HAPTIC=OFF",
        "-DSDL_SENSOR=OFF",
        "-DSDL_EXAMPLES=OFF",
        "-DSDL_TESTS=OFF",
        "-DSDL_SHARED=OFF",
        "-DSDL_STATIC=ON",
    ]
    run(args)
    run([cmake, "--build", build_dir, "--parallel", str(jobs)])
    run([cmake, "--install", build_dir])
    if not os.path.isfile(config):
        raise SystemExit("SDL3 install did not produce %s" % config)
    return prefix


def main():
    ndk = sys.argv[1]
    abi = sys.argv[2] if len(sys.argv) > 2 else "arm64-v8a"
    jobs = int(sys.argv[3]) if len(sys.argv) > 3 else 8
    print(build(ndk, abi, jobs=jobs), flush=True)


if __name__ == "__main__":
    main()
