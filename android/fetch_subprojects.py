import configparser
import os
import subprocess
import sys

TAISEI_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SUBPROJECTS = os.path.join(TAISEI_ROOT, "subprojects")


def sh(cmd, cwd):
    print("+ %s" % " ".join(cmd), flush=True)
    subprocess.run(cmd, cwd=cwd, check=True)


def force_rmtree(path):
    import shutil
    import stat

    def onerror(func, target, exc_info):
        try:
            os.chmod(target, stat.S_IWRITE)
            func(target)
        except OSError:
            pass

    shutil.rmtree(path, onerror=onerror)


def is_populated(path):
    if not os.path.isdir(path) or not os.listdir(path):
        return False
    return (
        os.path.isfile(os.path.join(path, "meson.build"))
        or os.path.isfile(os.path.join(path, "CMakeLists.txt"))
    )


SKIP = {"libpng", "mimalloc", "shaderc", "SPIRV-Headers", "SPIRV-Tools"}


def main():
    only = sys.argv[1:]
    jobs = []
    for name in sorted(os.listdir(SUBPROJECTS)):
        if not name.endswith(".wrap"):
            continue
        if name[:-5] in SKIP:
            print("not needed for this configuration: %s" % name[:-5], flush=True)
            continue
        jobs.append(name)

    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(max_workers=6) as pool:
        results = list(pool.map(fetch_one, jobs))
    if not all(results):
        raise SystemExit("some subprojects failed to fetch")
    print("all subprojects fetched", flush=True)


def fetch_one(name):
    try:
        return fetch_wrap(name)
    except Exception as e:
        print("FAILED %s: %s" % (name, e), flush=True)
        return False


def fetch_wrap(name):
    wrap = configparser.ConfigParser()
    wrap.read(os.path.join(SUBPROJECTS, name), encoding="utf-8")
    if "wrap-git" not in wrap:
        return True
    section = wrap["wrap-git"]
    directory = section.get("directory", name[:-5])
    url = section["url"]
    revision = section["revision"]
    dest = os.path.join(SUBPROJECTS, directory)
    if is_populated(dest):
        print("skip %s" % directory, flush=True)
        return True
    print("=== %s (%s @ %s)" % (directory, url, revision), flush=True)
    if os.path.isdir(dest):
        force_rmtree(dest)
    for attempt in range(3):
        try:
            if attempt == 0:
                sh([
                    "git", "-c", "http.postBuffer=524288000", "clone",
                    "--depth", "1", "--single-branch", "--branch", revision,
                    url, directory,
                ], SUBPROJECTS)
            elif is_populated(dest):
                sh(["git", "fetch", "--depth", "1", "origin", revision], dest)
                sh(["git", "checkout", "FETCH_HEAD"], dest)
            else:
                sh([
                    "git", "-c", "http.postBuffer=524288000", "clone",
                    "--no-checkout", url, directory,
                ], SUBPROJECTS)
                sh(["git", "fetch", "--depth", "1", "origin", revision], dest)
                sh(["git", "checkout", "FETCH_HEAD"], dest)
            print("done %s" % directory, flush=True)
            return True
        except subprocess.CalledProcessError as e:
            print("attempt %d failed for %s: %s" % (attempt, directory, e), flush=True)
            if os.path.isdir(dest):
                force_rmtree(dest)
    return False


if __name__ == "__main__":
    main()
