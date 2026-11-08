import argparse
import hashlib
import os
import platform
import shutil
import subprocess
import sys
import zipfile

ANDROID_DIR = os.path.dirname(os.path.abspath(__file__))
TAISEI_ROOT = os.path.dirname(ANDROID_DIR)
WORKSPACE = os.path.dirname(TAISEI_ROOT)
MIN_API = 24
NDK_VERSION = "26.1.10909125"
PLATFORM = "android-34"
BUILD_TOOLS = "34.0.0"

ABIS = {
    "arm64-v8a": {
        "triple": "aarch64-linux-android",
        "sysroot_triple": "aarch64-linux-android",
        "cpu_family": "aarch64",
        "cpu": "aarch64",
    },
    "armeabi-v7a": {
        "triple": "armv7a-linux-androideabi",
        "sysroot_triple": "arm-linux-androideabi",
        "cpu_family": "arm",
        "cpu": "armv7-a",
    },
    "x86_64": {
        "triple": "x86_64-linux-android",
        "sysroot_triple": "x86_64-linux-android",
        "cpu_family": "x86_64",
        "cpu": "x86_64",
    },
}


def log(msg):
    print("[android] %s" % msg, flush=True)


def run(cmd, cwd=None, env=None):
    cmd = [str(c) for c in cmd]
    if os.name == "nt" and cmd[0].lower().endswith((".bat", ".cmd")):
        cmd = ["cmd", "/c"] + cmd
    log("run: %s" % " ".join(cmd))
    subprocess.run(cmd, cwd=cwd, env=env, check=True)


def meson(args, cwd=None):
    env = os.environ.copy()
    patched = os.path.join(WORKSPACE, "toolchain", "meson-patched")
    if os.path.isdir(patched):
        env["PYTHONPATH"] = patched + os.pathsep + env.get("PYTHONPATH", "")
    env["PYTHONUNBUFFERED"] = "1"
    run([sys.executable, "-m", "mesonbuild.mesonmain"] + args, cwd=cwd, env=env)


def find_sdk(args):
    sdk = args.sdk or os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME")
    if not sdk:
        candidate = os.path.join(WORKSPACE, "android-sdk")
        if os.path.isdir(candidate):
            sdk = candidate
    if not sdk:
        raise SystemExit("Android SDK not found; pass --sdk or set ANDROID_SDK_ROOT")
    return os.path.abspath(sdk)


def find_ndk(args, sdk):
    ndk = args.ndk or os.environ.get("ANDROID_NDK_HOME") or os.environ.get("ANDROID_NDK_ROOT")
    if not ndk:
        candidate = os.path.join(sdk, "ndk", NDK_VERSION)
        if os.path.isdir(candidate):
            ndk = candidate
    if not ndk:
        raise SystemExit("Android NDK not found; pass --ndk or set ANDROID_NDK_HOME")
    return os.path.abspath(ndk)


def ndk_prebuilt(ndk):
    system = platform.system().lower()
    if system == "windows":
        name = "windows-x86_64"
    elif system == "darwin":
        name = "darwin-x86_64"
    else:
        name = "linux-x86_64"
    return os.path.join(ndk, "toolchains", "llvm", "prebuilt", name)


def write_cross_file(ndk, abi, dest, sdl_prefix=None):
    info = ABIS[abi]
    prebuilt = ndk_prebuilt(ndk)
    bin_dir = os.path.join(prebuilt, "bin")
    ext = ".cmd" if os.name == "nt" else ""
    exe = ".exe" if os.name == "nt" else ""

    def tool(name):
        return os.path.join(bin_dir, name)

    c = tool("%s%d-clang%s" % (info["triple"], MIN_API, ext))
    cpp = tool("%s%d-clang++%s" % (info["triple"], MIN_API, ext))
    for path in (c, cpp):
        if not os.path.isfile(path):
            raise SystemExit("NDK compiler not found: %s" % path)

    cmake = shutil.which("cmake") or "cmake"
    lines = [
        "[binaries]",
        "c = '%s'" % c,
        "cpp = '%s'" % cpp,
        "ar = '%s'" % tool("llvm-ar" + exe),
        "ranlib = '%s'" % tool("llvm-ranlib" + exe),
        "strip = '%s'" % tool("llvm-strip" + exe),
        "cmake = '%s'" % cmake,
        "python = '%s'" % sys.executable,
        "",
        "[properties]",
        "cmake_toolchain_file = '%s'" % os.path.join(ndk, "build", "cmake", "android.toolchain.cmake"),
        "needs_exe_wrapper = true",
    ]
    lines += [
        "[cmake]",
        "ANDROID_ABI = '%s'" % abi,
        "ANDROID_PLATFORM = 'android-%d'" % MIN_API,
        "ANDROID_STL = 'c++_shared'",
        "ANDROID_NDK = '%s'" % ndk,
        "ANDROID_USE_LEGACY_TOOLCHAIN_FILE = 'OFF'",
        "CMAKE_FIND_ROOT_PATH_MODE_PACKAGE = 'BOTH'",
        "CMAKE_WARN_DEPRECATED = 'OFF'",
        "CMAKE_SUPPRESS_DEVELOPER_WARNINGS = 'ON'",
        "",
        "[built-in options]",
        "c_args = ['-fPIC', '--sysroot=%s']" % os.path.join(prebuilt, "sysroot"),
        "cpp_args = ['-fPIC', '--sysroot=%s']" % os.path.join(prebuilt, "sysroot"),
        "c_link_args = ['--sysroot=%s']" % os.path.join(prebuilt, "sysroot"),
        "cpp_link_args = ['--sysroot=%s']" % os.path.join(prebuilt, "sysroot"),
        "cmake_prefix_path = ['%s']" % sdl_prefix if sdl_prefix else "cmake_prefix_path = []",
        "",
        "[host_machine]",
        "system = 'android'",
        "cpu_family = '%s'" % info["cpu_family"],
        "cpu = '%s'" % info["cpu"],
        "endian = 'little'",
        "",
    ]
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with open(dest, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    return dest


def meson_str_list(items):
    return "[%s]" % ", ".join("'%s'" % i for i in items)


def zig_command():
    local = os.path.join(WORKSPACE, "toolchain", "zig", "zig.exe" if os.name == "nt" else "zig")
    if os.path.isfile(local):
        return [local]
    return [sys.executable, "-m", "ziglang"]


def write_native_file(dest):
    zig = zig_command()
    target = ["-target", "x86_64-windows-gnu"] if os.name == "nt" else []
    lines = [
        "[binaries]",
        "c = %s" % meson_str_list(zig + ["cc"] + target),
        "cpp = %s" % meson_str_list(zig + ["c++"] + target),
        "ar = %s" % meson_str_list(zig + ["ar"]),
        "ranlib = %s" % meson_str_list(zig + ["ranlib"]),
        "",
        "[host_machine]",
        "system = 'windows'" if os.name == "nt" else "system = 'linux'",
        "cpu_family = 'x86_64'",
        "cpu = 'x86_64'",
        "endian = 'little'",
        "",
    ]
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with open(dest, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    return dest


def build_native(ndk, abi, buildtype, jobs, skip_compile, reconfigure=False):
    import build_sdl3

    build_dir = os.path.join(TAISEI_ROOT, "build-android", abi)
    install_dir = os.path.join(build_dir, "install")
    sdl_prefix = build_sdl3.build(ndk, abi, jobs=jobs)
    cross = write_cross_file(ndk, abi, os.path.join(build_dir, "cross.ini"), sdl_prefix=sdl_prefix)
    native = write_native_file(os.path.join(TAISEI_ROOT, "build-android", "native.ini"))
    options = []
    with open(os.path.join(ANDROID_DIR, "options.ini"), encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                options.append(line)

    configured = os.path.isfile(os.path.join(build_dir, "meson-private", "coredata.dat"))
    install_root = os.path.join(install_dir, "taisei")
    setup = [
        "setup", build_dir, TAISEI_ROOT,
        "--cross-file", cross,
        "--native-file", native,
        "--prefix", "/taisei",
        "--buildtype", buildtype,
        "--wrap-mode=default",
    ]
    if configured and reconfigure:
        setup.append("--reconfigure")
    if not configured or reconfigure:
        meson(setup + options)

    if not skip_compile:
        meson(["compile", "-C", build_dir, "-j", str(jobs)])
        meson(["install", "-C", build_dir, "--no-rebuild", "--destdir", install_dir])
    else:
        return None, install_root

    candidates = [
        os.path.join(build_dir, "src", "libtaisei.so"),
        os.path.join(install_root, "libtaisei.so"),
    ]
    for path in candidates:
        if os.path.isfile(path):
            return path, install_root
    raise SystemExit("libtaisei.so not found in %s" % build_dir)


def stage_assets(install_dir, assets_dir):
    data_src = os.path.join(install_dir, "data")
    if not os.path.isdir(data_src):
        raise SystemExit("Installed data directory not found: %s" % data_src)

    shutil.copytree(data_src, assets_dir, dirs_exist_ok=True)

    meta_dir = os.path.join(assets_dir, "taisei")
    os.makedirs(meta_dir, exist_ok=True)

    entries = []
    digest = hashlib.sha1()
    for root, dirs, files in os.walk(assets_dir):
        dirs.sort()
        for name in sorted(files):
            full = os.path.join(root, name)
            rel = os.path.relpath(full, assets_dir).replace(os.sep, "/")
            if rel.startswith("taisei/"):
                continue
            entries.append(rel)
            digest.update(rel.encode("utf-8"))
            digest.update(str(os.path.getsize(full)).encode("ascii"))

    with open(os.path.join(meta_dir, "files.list"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(entries) + "\n")
    with open(os.path.join(meta_dir, "version.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(digest.hexdigest())
    log("staged %d asset files, version %s" % (len(entries), digest.hexdigest()[:12]))


def java_tool(name):
    java_home = os.environ.get("JAVA_HOME")
    ext = ".exe" if os.name == "nt" else ""
    if java_home:
        candidate = os.path.join(java_home, "bin", name + ext)
        if os.path.isfile(candidate):
            return candidate
    found = shutil.which(name)
    if not found:
        raise SystemExit("%s not found; set JAVA_HOME" % name)
    return found


def copy_sdl_java(dest):
    src = os.path.join(
        TAISEI_ROOT, "subprojects", "sdl3", "android-project", "app", "src", "main", "java", "org", "libsdl", "app"
    )
    if not os.path.isdir(src):
        raise SystemExit("SDL Android Java sources not found at %s" % src)
    target = os.path.join(dest, "org", "libsdl", "app")
    os.makedirs(target, exist_ok=True)
    for name in os.listdir(src):
        if name.endswith(".java"):
            shutil.copy2(os.path.join(src, name), os.path.join(target, name))
    app_src = os.path.join(ANDROID_DIR, "app", "src", "org", "taisei_project", "TaiseiActivity.java")
    target2 = os.path.join(dest, "org", "taisei_project")
    os.makedirs(target2, exist_ok=True)
    shutil.copy2(app_src, os.path.join(target2, "TaiseiActivity.java"))
    return dest


def build_apk(sdk, ndk, abi_libs, assets_dir, out_apk, build_dir):
    build_tools = os.path.join(sdk, "build-tools", BUILD_TOOLS)
    android_jar = os.path.join(sdk, "platforms", PLATFORM, "android.jar")
    aapt2 = os.path.join(build_tools, "aapt2.exe" if os.name == "nt" else "aapt2")
    d8 = os.path.join(build_tools, "d8.bat" if os.name == "nt" else "d8")
    apksigner = os.path.join(build_tools, "apksigner.bat" if os.name == "nt" else "apksigner")
    zipalign = os.path.join(build_tools, "zipalign.exe" if os.name == "nt" else "zipalign")

    staging = os.path.join(build_dir, "apk-staging")
    if os.path.isdir(staging):
        shutil.rmtree(staging)
    os.makedirs(staging)

    app_dir = os.path.join(ANDROID_DIR, "app")

    def relative(path):
        return os.path.relpath(path, app_dir).replace(os.sep, "/")

    compiled_res = os.path.join(staging, "res.flat.zip")
    run([aapt2, "compile", "--dir", "res", "-o", relative(compiled_res)], cwd=app_dir)

    base_apk = os.path.join(staging, "base.apk")
    gen_dir = os.path.join(staging, "gen")
    run([
        aapt2, "link", "-o", relative(base_apk),
        "-I", relative(android_jar),
        "--manifest", "AndroidManifest.xml",
        "--java", relative(gen_dir),
        "--min-sdk-version", str(MIN_API),
        "--target-sdk-version", PLATFORM.split("-")[1],
        relative(compiled_res),
    ], cwd=app_dir)

    java_src = copy_sdl_java(os.path.join(staging, "java"))
    sources = []
    for root, dirs, files in os.walk(java_src):
        for name in files:
            if name.endswith(".java"):
                sources.append(os.path.join(root, name))
    for root, dirs, files in os.walk(gen_dir):
        for name in files:
            if name.endswith(".java"):
                sources.append(os.path.join(root, name))

    classes_dir = os.path.join(staging, "classes")
    os.makedirs(classes_dir, exist_ok=True)
    def staged(path):
        return os.path.relpath(path, staging).replace(os.sep, "/")

    argfile = os.path.join(staging, "javac-sources.txt")
    with open(argfile, "w", encoding="utf-8") as f:
        f.write("\n".join('"%s"' % staged(s) for s in sources))
    run([java_tool("javac"), "-source", "8", "-target", "8", "-encoding", "UTF-8",
         "-cp", staged(android_jar), "-d", "classes", "@javac-sources.txt"], cwd=staging)

    classes_jar = os.path.join(staging, "classes.jar")
    run([java_tool("jar"), "cf", "classes.jar", "-C", "classes", "."], cwd=staging)

    dex_dir = os.path.join(staging, "dex")
    os.makedirs(dex_dir, exist_ok=True)
    run([d8, "--release", "--min-api", str(MIN_API), "--lib", staged(android_jar),
         "--output", "dex", "classes.jar"], cwd=staging)

    unsigned = os.path.join(staging, "unsigned.apk")
    shutil.copy2(base_apk, unsigned)
    with zipfile.ZipFile(unsigned, "a", compression=zipfile.ZIP_STORED) as apk:
        apk.write(os.path.join(dex_dir, "classes.dex"), "classes.dex")
        for root, dirs, files in os.walk(assets_dir):
            for name in files:
                full = os.path.join(root, name)
                rel = os.path.relpath(full, assets_dir).replace(os.sep, "/")
                apk.write(full, "assets/" + rel)
        for abi, libs in abi_libs.items():
            for lib in libs:
                apk.write(lib, "lib/%s/%s" % (abi, os.path.basename(lib)))

    aligned = os.path.join(staging, "aligned.apk")
    run([zipalign, "-f", "-p", "4", unsigned, aligned])

    keystore = os.path.join(build_dir, "debug.keystore")
    if not os.path.isfile(keystore):
        run([
            java_tool("keytool"), "-genkeypair", "-v",
            "-keystore", keystore, "-alias", "androiddebugkey",
            "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000",
            "-storepass", "android", "-keypass", "android",
            "-dname", "CN=Android Debug,O=Android,C=US",
        ])

    if os.path.isfile(out_apk):
        os.remove(out_apk)
    run([apksigner, "sign", "--ks", keystore,
         "--ks-pass", "pass:android", "--key-pass", "pass:android",
         "--out", out_apk, aligned])
    run([apksigner, "verify", out_apk])
    log("APK ready: %s (%d bytes)" % (out_apk, os.path.getsize(out_apk)))


def main():
    parser = argparse.ArgumentParser(description="Build Taisei for Android")
    parser.add_argument("--abis", default="arm64-v8a")
    parser.add_argument("--sdk")
    parser.add_argument("--ndk")
    parser.add_argument("--buildtype", default="release")
    parser.add_argument("-j", "--jobs", type=int, default=min(8, os.cpu_count() or 4))
    parser.add_argument("--skip-native", action="store_true")
    parser.add_argument("--configure-only", action="store_true")
    parser.add_argument("--reconfigure", action="store_true")
    parser.add_argument("--skip-apk", action="store_true")
    parser.add_argument("--install", action="store_true")
    parser.add_argument("--out")
    args = parser.parse_args()

    abis = [a.strip() for a in args.abis.split(",") if a.strip()]
    for abi in abis:
        if abi not in ABIS:
            raise SystemExit("Unsupported ABI: %s" % abi)

    sdk = find_sdk(args)
    ndk = find_ndk(args, sdk)
    log("SDK: %s" % sdk)
    log("NDK: %s" % ndk)

    build_root = os.path.join(TAISEI_ROOT, "build-android")
    os.makedirs(build_root, exist_ok=True)
    out_apk = args.out or os.path.join(build_root, "taisei-android.apk")

    abi_libs = {}
    install_dir = None
    prebuilt = ndk_prebuilt(ndk)

    for abi in abis:
        if args.skip_native:
            install_dir = os.path.join(TAISEI_ROOT, "build-android", abi, "install", "taisei")
            lib = os.path.join(TAISEI_ROOT, "build-android", abi, "src", "libtaisei.so")
        else:
            lib, install_dir = build_native(ndk, abi, args.buildtype, args.jobs, args.configure_only, args.reconfigure)

        if not args.configure_only:
            libs = [lib]
            cxx = os.path.join(prebuilt, "sysroot", "usr", "lib", ABIS[abi]["sysroot_triple"], "libc++_shared.so")
            if not os.path.isfile(cxx):
                raise SystemExit("NDK C++ runtime not found: %s" % cxx)
            libs.append(cxx)
            abi_libs[abi] = libs

    if args.configure_only or args.skip_apk:
        log("native stage complete")
        return

    assets_dir = os.path.join(build_root, "assets")
    if os.path.isdir(assets_dir):
        shutil.rmtree(assets_dir)
    os.makedirs(assets_dir)
    stage_assets(install_dir, assets_dir)
    build_apk(sdk, ndk, abi_libs, assets_dir, out_apk, build_root)

    if args.install:
        adb = os.path.join(sdk, "platform-tools", "adb.exe" if os.name == "nt" else "adb")
        run([adb, "install", "-r", out_apk])


if __name__ == "__main__":
    main()
