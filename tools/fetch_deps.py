#!/usr/bin/env python3
"""Ocarina-Of-Time-X: fetch the third-party code libultraship and Ship of Harkinian
build with, into deps/ (not committed; setup.py runs this). Each is pinned to
an exact commit: the versions LUS's CMake names, or for its vcpkg packages,
the releases current when the port was made.

    python tools/fetch_deps.py

Not fetched, on purpose: SDL (replaced by
the Xbox backends in xbox/), libzip and bzip2 (xbox/zip is a read-only
.o2r reader over zlib), StormLib (no MPQ), tinycc (no scripting), prism (only
the PC renderers' shader templates use it), libgfxd (debugger only).
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEPS = os.path.join(ROOT, "deps")

# name: (repository, commit): exact commits, so a tag moved upstream changes nothing here
PINS = {
    "spdlog": ("https://github.com/gabime/spdlog.git", "6fa36017cfd5731d617e1a934f0e5ea9c4445b13"),  # v1.15.3
    "json": ("https://github.com/nlohmann/json.git", "9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03"),  # v3.11.3
    "tinyxml2": ("https://github.com/leethomason/tinyxml2.git", "321ea883b7190d4e85cae5512a12e5eaa8f8731f"),  # 10.0.0
    "zlib": ("https://github.com/madler/zlib.git", "51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf"),  # v1.3.1
    "stb": ("https://github.com/nothings/stb.git", "0bc88af4de5fb022db643c2d8e549a0927749354"),
    "thread-pool": ("https://github.com/bshoshany/thread-pool.git", "097aa718f25d44315cadb80b407144ad455ee4f9"),  # v4.1.0
    "monocypher": ("https://github.com/LoupVaillant/Monocypher.git", "0d85f98c9d9b0227e42cf795cb527dff372b40a4"),
    "dr_libs": ("https://github.com/mackron/dr_libs.git", "da35f9d6c7374a95353fd1df1d394d44ab66cf01"),
    # Dear ImGui, the version LUS's CMake names (its patch only sets two imconfig.h options: xbox/include/
    # sohx_imconfig.h has them). SoH's game code calls it all over, so its core is built for SoH's files, though
    # nothing ever draws it: libultraship stays LUS_NO_IMGUI, with xbox/include/imgui.h's types alone.
    "imgui": ("https://github.com/ocornut/imgui.git", "4806a1924ff6181180bf5e4b8b79ab4394118875"),  # v1.91.9b-docking
}


def git(args, cwd=None):
    r = subprocess.run(["git"] + args, cwd=cwd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stdout.write(r.stdout + r.stderr)
        raise SystemExit("fetch_deps: git %s failed" % " ".join(args))
    return r.stdout.strip()


def fetch(name, url, ref):
    dst = os.path.join(DEPS, name)
    if os.path.isdir(os.path.join(dst, ".git")):
        head = git(["rev-parse", "HEAD"], dst)
        tag = subprocess.run(["git", "rev-parse", ref + "^{commit}"], cwd=dst, capture_output=True, text=True)
        if head == ref or (tag.returncode == 0 and tag.stdout.strip() == head):
            print("fetch_deps: %s at %s" % (name, ref))
            return
    os.makedirs(dst, exist_ok=True)
    if not os.path.isdir(os.path.join(dst, ".git")):
        git(["init", "-q"], dst)
        git(["remote", "add", "origin", url], dst)
    git(["fetch", "-q", "--depth", "1", "origin", ref], dst)
    git(["checkout", "-q", "FETCH_HEAD"], dst)
    print("fetch_deps: %s fetched at %s" % (name, ref))


def main():
    os.makedirs(DEPS, exist_ok=True)
    for name, (url, ref) in PINS.items():
        fetch(name, url, ref)


if __name__ == "__main__":
    main()
