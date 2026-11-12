#!/usr/bin/env python3
"""One command from your Ocarina of Time ROM to an Xbox disc image.

    python setup.py                 # everything -> out/Release/XISO/Ocarina-Of-Time-X.iso
    python setup.py --check         # say what is there and what is missing
    python setup.py --tester        # with the tester tools: log files, overlay, black box -> out/Tester/
    python setup.py --skip-build    # everything but compiling the game (the archives, the tools)

Needs: Python 3.8+ (standard library only), Git, Visual Studio 2022 or later
with "Desktop development with C++" (its CMake builds Ship of Harkinian's
archive tools), the RXDK (it builds the game), an internet connection the
first time, and your Ocarina of Time ROM in this folder as baserom.z64. A
.v64 / .n64 (byte-swapped) dump is accepted and put in order.

Nothing that comes from the ROM is in this repository, and nothing on this
PC but the above is used. In order, into folders .gitignore keeps out:

  1. the ROM, checked (SHA-1) against the versions Ship of Harkinian 9.3.1
     extracts, and put in big-endian order: build/rom/baserom.z64
  2. fetched, each at one exact commit, so a change upstream changes nothing
     here: Ship of Harkinian 9.3.1's asset definitions and its own extra
     files (build/upstream/Shipwright: soh/assets/yml and soh/assets/custom),
     Torch, its asset extractor (build/upstream/torch), and the libraries the
     game is built with (deps/, tools/fetch_deps.py)
  3. Ship of Harkinian's two archive tools, soh-torch and soh-o2r-packer
     (tools/extract), built with Visual Studio: build/extract/Release/
  4. data/oot.o2r (oot-mq.o2r for a Master Quest ROM), extracted from the ROM,
     and data/soh.o2r, Ship of Harkinian's own extras packed
  5. the game, built with the RXDK (tools/build.py), with the dashboard's
     images made from the ROM's Ocarina icon: out/<config>/XISO/
     Ocarina-Of-Time-X.iso and the same files as a folder, out/<config>/Build/
     Ocarina-Of-Time-X/, to copy to an Xbox's hard disk
"""
import argparse
import glob
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
NAME = "Ocarina-Of-Time-X"
BUILD = os.path.join(ROOT, "build")
DATA = os.path.join(ROOT, "data")
ROM_NAMES = ("baserom.z64", "baserom.v64", "baserom.n64", "oot.z64", "oot.v64", "oot.n64")
ROM = os.path.join(BUILD, "rom", "baserom.z64")
TESTED_SHA1 = "41b3bdc48d98c48529219919015a1af22f5057c2"  # USA (Rev 2), 1.2: the one the port is tested with

# Ship of Harkinian: the release this repository's Shipwright/ tree is (with the Xbox port's changes), fetched only for
# what isn't in the tree, its asset definitions and its own extra files (sparse: nothing else is downloaded)
SHIP_URL = "https://github.com/HarbourMasters/Shipwright.git"
SHIP_TAG = "9.3.1"
SHIP_REV = "f0599f96f8d442f26d0fa1c5cedafddacd56633f"
SHIP_PARTS = ("soh/assets/yml", "soh/assets/custom")
SHIP_SRC = os.path.join(BUILD, "upstream", "Shipwright")
# Torch, at the commit Ship of Harkinian 9.3.1 has as its submodule
TORCH_URL = "https://github.com/HarbourMasters/Torch.git"
TORCH_REV = "2ab12fe9660aec04e02ee89fe81baed304a1a1d6"
TORCH_SRC = os.path.join(BUILD, "upstream", "torch")

TOOLS_BUILD = os.path.join(BUILD, "extract")
TORCH_EXE = os.path.join(TOOLS_BUILD, "Release", "soh-torch.exe")
PACKER_EXE = os.path.join(TOOLS_BUILD, "Release", "soh-o2r-packer.exe")
ARCHIVES_STAMP = os.path.join(DATA, "made-from.txt")


def say(msg):
    print("setup: " + msg, flush=True)


def die(msg):
    print("\nsetup: " + msg, flush=True)
    raise SystemExit(1)


def run(cmd, cwd=None):
    r = subprocess.run(cmd, cwd=cwd)
    if r.returncode != 0:
        die("failed (exit %d): %s" % (r.returncode, " ".join('"%s"' % c if " " in c else c for c in cmd)))


def git_head(path):
    if not os.path.exists(os.path.join(path, ".git")):
        return None
    r = subprocess.run(["git", "-C", path, "rev-parse", "HEAD"], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None


def remove_tree(path):
    """shutil.rmtree, through the read-only files git leaves in its object store."""
    def retry(func, p, _exc):
        os.chmod(p, stat.S_IWRITE)
        func(p)
    if os.path.isdir(path):
        shutil.rmtree(path, onerror=retry)


def ship_version():
    with open(os.path.join(ROOT, "Shipwright", "CMakeLists.txt"), encoding="utf-8") as f:
        m = re.search(r"project\(Ship VERSION (\d+\.\d+\.\d+)", f.read())
    return m.group(1)


# ---------------------------------------------------------------- the ROM

def find_rom():
    for n in ROM_NAMES:
        p = os.path.join(ROOT, n)
        if os.path.isfile(p):
            return p
    return None


def rom_in_order(data):
    """The ROM in .z64 (big-endian) order: a .v64 swaps each pair of bytes, a .n64 each four."""
    head = data[:4]
    if head == b"\x80\x37\x12\x40":
        return data
    if head == b"\x37\x80\x40\x12":
        b = bytearray(data)
        b[0::2], b[1::2] = data[1::2], data[0::2]
        return bytes(b)
    if head == b"\x40\x12\x37\x80":
        b = bytearray(data)
        b[0::4], b[1::4], b[2::4], b[3::4] = data[3::4], data[2::4], data[1::4], data[0::4]
        return bytes(b)
    return None


def known_roms():
    """{sha1: name} of the ROMs Ship of Harkinian 9.3.1 extracts (its asset definitions' config.yml), or None before
    they're fetched."""
    cfg = os.path.join(SHIP_SRC, "soh", "assets", "yml", "config.yml")
    if not os.path.isfile(cfg):
        return None
    out, sha = {}, None
    with open(cfg, encoding="utf-8") as f:
        for line in f:
            m = re.match(r"^([0-9a-f]{40}):", line)
            if m:
                sha = m.group(1)
            m = re.match(r"^\s+name:\s*(.+)$", line)
            if m and sha:
                out[sha] = m.group(1).strip()
                sha = None
    return out


def rom_ok():
    """(sha1, None) for a ROM in order at build/rom/baserom.z64, or (None, why not)."""
    src = find_rom()
    if src is None:
        return None, "no ROM: put your Ocarina of Time ROM in %s as baserom.z64" % ROOT
    with open(src, "rb") as f:
        data = rom_in_order(f.read())
    if data is None:
        return None, "%s is not an N64 ROM (its first bytes are none of the three byte orders)" % os.path.basename(src)
    sha = hashlib.sha1(data).hexdigest()
    known = known_roms()
    if known is not None and sha not in known:
        return None, ("%s is not a ROM Ship of Harkinian %s extracts (SHA-1 %s); the USA 1.2 one is %s"
                      % (os.path.basename(src), SHIP_TAG, sha, TESTED_SHA1))
    have = None
    if os.path.isfile(ROM):
        with open(ROM, "rb") as f:
            have = hashlib.sha1(f.read()).hexdigest()
    if have != sha:
        os.makedirs(os.path.dirname(ROM), exist_ok=True)
        with open(ROM, "wb") as f:
            f.write(data)
    return sha, None


# ---------------------------------------------------- Visual Studio, CMake

def find_vs_install():
    """The newest Visual Studio (2022 or later) with the x64 C++ tools, by vswhere, which every Visual Studio installer
    puts in the same place: (its folder, its major version -- 17 for 2022, 18 for 2026), or None."""
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                           "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if not os.path.isfile(vswhere):
        return None
    r = subprocess.run([vswhere, "-latest", "-products", "*", "-version", "[17.0,",
                        "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                        "-format", "json", "-utf8"],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    try:
        found = json.loads(r.stdout) if (r.returncode == 0 and r.stdout) else []
    except ValueError:
        found = []
    if not found:
        return None
    path = found[0].get("installationPath", "")
    try:
        major = int(found[0].get("installationVersion", "0").split(".")[0])
    except ValueError:
        major = 0
    return (path, major) if os.path.isdir(path) else None


def vs_generator(cmake, major):
    """The Visual Studio generator of `major` this CMake has ("Visual Studio 17 2022"), or None."""
    r = subprocess.run([cmake, "--help"], capture_output=True, text=True, encoding="utf-8", errors="replace")
    for line in r.stdout.splitlines():
        name = line.split("=")[0].strip().lstrip("*").strip()
        if name.endswith("[arch]"):
            name = name[:-len("[arch]")].strip()
        if name.startswith("Visual Studio %d " % major):
            return name
    return None


def find_cmake():
    """(cmake, generator) able to build with the Visual Studio found -- its own CMake first, then one on the PATH -- or
    (a cmake, None) when none knows it, or (None, None) with no CMake at all."""
    vs = find_vs_install()
    cands = []
    if vs:
        p = os.path.join(vs[0], "Common7", "IDE", "CommonExtensions", "Microsoft", "CMake", "CMake", "bin", "cmake.exe")
        if os.path.isfile(p):
            cands.append(p)
    c = shutil.which("cmake")
    if c and all(os.path.normcase(c) != os.path.normcase(o) for o in cands):
        cands.append(c)
    if vs:
        for c in cands:
            gen = vs_generator(c, vs[1])
            if gen:
                return c, gen
    return (cands[0] if cands else None), None


def rxdk_root():
    return os.environ.get("RXDK_ROOT") or os.path.join(os.environ.get("ProgramData") or r"C:\ProgramData", "RXDK")


# ---------------------------------------------------------------- --check

def check():
    ok = True
    py = sys.version_info >= (3, 8)
    say("Python: %d.%d%s" % (sys.version_info[0], sys.version_info[1], "" if py else " (3.8 or later needed)"))
    ok &= py
    git = shutil.which("git")
    say("Git: %s" % (git or "missing (install Git for Windows: https://git-scm.com/download/win)"))
    ok &= git is not None
    rx = rxdk_root()
    clang = os.path.join(rx, "llvm", "xbox-windows-x64", "bin", "clang.exe")
    imagebld = os.path.join(rx, "tools", "imagebld.exe")
    rx_ok = os.path.isfile(clang) and os.path.isfile(imagebld)
    say("RXDK: %s" % (rx if rx_ok else "missing in %s (RXDK > RXDK Window > Install Prerequisites)" % rx))
    ok &= rx_ok
    vs = find_vs_install()
    say("Visual Studio C++ tools: %s" % (vs[0] if vs else
                                          "missing (Visual Studio 2022 or later, Desktop development with C++)"))
    ok &= vs is not None
    cm, gen = find_cmake()
    if cm is None:
        say("CMake: missing (Visual Studio's C++ CMake tools for Windows, or cmake.org)")
    elif gen is None:
        say("CMake: %s does not know this Visual Studio (install a newer CMake)" % cm)
    else:
        say("CMake: %s (%s)" % (cm, gen))
    ok &= gen is not None
    src = find_rom()
    if src is None:
        say("ROM: missing (put your Ocarina of Time ROM here as baserom.z64)")
        ok = False
    else:
        sha, why = rom_ok()
        known = known_roms() or {}
        if sha:
            say("ROM: %s, %s%s" % (os.path.basename(src), known.get(sha, "SHA-1 " + sha),
                                   "" if known else " (checked against Ship of Harkinian's list once it's fetched)"))
        else:
            say("ROM: " + why)
        ok &= sha is not None
    say("Ship of Harkinian %s assets: %s" % (SHIP_TAG, "fetched" if git_head(SHIP_SRC) == SHIP_REV else
                                              "not fetched yet (the build fetches them)"))
    say("Torch: %s" % ("fetched" if git_head(TORCH_SRC) == TORCH_REV else "not fetched yet (the build fetches it)"))
    for label, path in (("archive tools", TORCH_EXE), ("data/oot.o2r", os.path.join(DATA, "oot.o2r")),
                        ("data/soh.o2r", os.path.join(DATA, "soh.o2r"))):
        say("%s: %s" % (label, "built" if os.path.isfile(path) else "not built yet"))
    say("everything needed is here" if ok else "something is missing (above)")
    return ok


# ------------------------------------------------------------------ steps

def fetch_at(url, rev, dst, sparse=None, tag=None):
    """dst as a checkout of url at exactly rev (shallow; `sparse`: only those folders)."""
    if git_head(dst) == rev:
        return
    remove_tree(dst)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if sparse:
        # a partial clone of the tag: the folders' files are all that's downloaded
        run(["git", "-c", "advice.detachedHead=false", "clone", "-q", "--depth", "1", "--branch", tag,
             "--filter=blob:none", "--sparse", url, dst])
        run(["git", "-C", dst, "sparse-checkout", "set"] + list(sparse))
    else:
        os.makedirs(dst)
        run(["git", "init", "-q", dst])
        run(["git", "-C", dst, "remote", "add", "origin", url])
        run(["git", "-C", dst, "fetch", "-q", "--depth", "1", "origin", tag or rev])
        run(["git", "-C", dst, "checkout", "-q", "FETCH_HEAD"])
    head = git_head(dst)
    if head != rev:
        die("%s at %s is commit %s, not %s: upstream has changed it (setup.py pins %s)" % (url, tag or rev, head, rev,
                                                                                         rev))


def step_fetch():
    if shutil.which("git") is None:
        die("Git not found (install Git for Windows: https://git-scm.com/download/win)")
    if git_head(SHIP_SRC) != SHIP_REV:
        say("fetching Ship of Harkinian %s's asset definitions and extra files (only those folders)" % SHIP_TAG)
        fetch_at(SHIP_URL, SHIP_REV, SHIP_SRC, sparse=SHIP_PARTS, tag=SHIP_TAG)
    if git_head(TORCH_SRC) != TORCH_REV:
        say("fetching Torch")
        fetch_at(TORCH_URL, TORCH_REV, TORCH_SRC)
    say("fetching the libraries (deps/)")
    run([sys.executable, os.path.join(ROOT, "tools", "fetch_deps.py")])


def step_tools(jobs):
    vs = find_vs_install()
    if vs is None:
        die("Visual Studio 2022 or later with the C++ tools not found (Desktop development with C++)")
    cmake, gen = find_cmake()
    if cmake is None:
        die("CMake not found (Visual Studio's C++ CMake tools for Windows, or cmake.org)")
    if gen is None:
        die("%s is too old for Visual Studio %d: install a newer CMake, or Visual Studio's C++ CMake tools" %
            (cmake, vs[1]))
    cache = os.path.join(TOOLS_BUILD, "CMakeCache.txt")
    if os.path.isfile(cache):
        with open(cache, encoding="utf-8", errors="replace") as f:
            text = f.read()
        if ("CMAKE_GENERATOR:INTERNAL=%s\n" % gen) not in text:
            say("build/extract was set up for another Visual Studio: starting it again")
            remove_tree(TOOLS_BUILD)
    say("configuring Ship of Harkinian's archive tools with %s (Torch's libraries are downloaded the first time)"
        % gen)
    # CMAKE_POLICY_VERSION_MINIMUM: CMake 4 (Visual Studio 2026's) refuses a project that says it works with CMake
    # older than 3.5, as yaml-cpp (one of Torch's) says; this is CMake's own way to build it anyway
    run([cmake, "-S", os.path.join(ROOT, "tools", "extract"), "-B", TOOLS_BUILD, "-G", gen, "-A", "x64",
         "-DCMAKE_GENERATOR_INSTANCE=" + vs[0], "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
         "-DTORCH_DIR=" + TORCH_SRC.replace("\\", "/")])
    say("building them")
    run([cmake, "--build", TOOLS_BUILD, "--config", "Release", "--parallel", str(jobs)])
    for exe in (TORCH_EXE, PACKER_EXE):
        if not os.path.isfile(exe):
            die("%s was not built" % os.path.relpath(exe, ROOT))


def step_archives(rom_sha):
    """data/oot.o2r (or oot-mq.o2r) and data/soh.o2r, made again when the ROM or the tools changed."""
    version = ship_version()
    made_from = "rom %s\nship %s\ntorch %s\ntools %d %d\n" % (rom_sha, SHIP_REV, TORCH_REV,
                                                             int(os.path.getmtime(TORCH_EXE)),
                                                             int(os.path.getmtime(PACKER_EXE)))
    have = None
    if os.path.isfile(ARCHIVES_STAMP):
        with open(ARCHIVES_STAMP, encoding="utf-8") as f:
            have = f.read()
    if have == made_from and glob.glob(os.path.join(DATA, "oot*.o2r")) and os.path.isfile(
            os.path.join(DATA, "soh.o2r")):
        say("data/: the archives are up to date")
        return
    remove_tree(DATA)
    os.makedirs(DATA)
    say("extracting the game from the ROM into data/ (a few minutes)")
    run([TORCH_EXE, "--src", os.path.join(SHIP_SRC, "soh", "assets", "yml"), "--dest", DATA, "--version", version,
         ROM])
    say("packing Ship of Harkinian's own files into data/soh.o2r")
    run([PACKER_EXE, os.path.join(SHIP_SRC, "soh", "assets", "custom"), os.path.join(DATA, "soh.o2r"), version])
    if not glob.glob(os.path.join(DATA, "oot*.o2r")) or not os.path.isfile(os.path.join(DATA, "soh.o2r")):
        die("the archives were not made (data/ has %s)" % (", ".join(os.listdir(DATA)) or "nothing"))
    with open(ARCHIVES_STAMP, "w", encoding="utf-8") as f:
        f.write(made_from)


def step_build(tester, jobs):
    say("building the game with the RXDK%s" % (" (with the tester tools)" if tester else ""))
    run([sys.executable, os.path.join(ROOT, "tools", "build.py"), "-j", str(jobs)] + ([] if tester else ["--release"]))
    config = "Tester" if tester else "Release"
    iso = os.path.join(ROOT, "out", config, "XISO", NAME + ".iso")
    if not os.path.isfile(iso):
        die("the disc image was not made")
    say("done: %s" % os.path.relpath(iso, ROOT))
    say("      %s (the same files, to copy to a hard disk)" %
        os.path.relpath(os.path.join(ROOT, "out", config, "Build", NAME), ROOT))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="say what is there and what is missing, and stop")
    ap.add_argument("--tester", action="store_true",
                    help="build with the tester tools (log files on the hard disk, overlay, black box): out/Tester/")
    ap.add_argument("--skip-build", action="store_true", help="stop before compiling the game")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()
    if a.check:
        raise SystemExit(0 if check() else 1)
    if sys.version_info < (3, 8):
        die("Python 3.8 or later is needed")
    if find_rom() is None:
        die("no ROM: put your Ocarina of Time ROM in %s as baserom.z64" % ROOT)
    step_fetch()
    rom_sha, why = rom_ok()
    if rom_sha is None:
        die(why)
    say("ROM: %s" % (known_roms() or {}).get(rom_sha, rom_sha))
    step_tools(a.jobs)
    step_archives(rom_sha)
    if a.skip_build:
        say("stopped before the game's build (--skip-build)")
        return
    step_build(a.tester, a.jobs)


if __name__ == "__main__":
    main()
