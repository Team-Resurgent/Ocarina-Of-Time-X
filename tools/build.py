#!/usr/bin/env python3
"""Ocarina-Of-Time-X: the original Xbox build of libultraship and Ship of Harkinian (Shipwright).

setup.py runs this once the archives are in data/; run on its own, it rebuilds the game from them.

    python tools/build.py [-k] [-j N] [--only GROUP[,GROUP]] [--file PATTERN]
                          [--no-link] [--clean] [--cmd PATTERN] [--no-lto]

Compiles with the RXDK's clang and the flags its Rxdk.Cli uses (copied from
`Rxdk.Cli build --verbose`), then links, runs imagebld and packs the disc the
way Rxdk.Cli does:

    build/obj/<group>/...      objects, rebuilt when a source, a header it
                               read (clang's depfile) or its command changes
    build/obj-lto/<group>/...  the same for the groups that are ThinLTO bitcode
                               (LTO_GROUPS; --no-lto builds them native, as a
                               quicker link or to compare)
    build/log/<group>/...      each failed file's compiler output
    build/Ocarina-Of-Time-X.exe   the linked PE (with line tables, for symbols)
    build/Ocarina-Of-Time-X.xbe   the title
    out/Release/Build/Ocarina-Of-Time-X/       the disc's files: default.xbe, the
                               archives (data/*.o2r) and the menu font, as copied
                               to a hard disk
    out/Release/XISO/Ocarina-Of-Time-X.iso     the disc image of them

Groups, in link order: deps (spdlog, tinyxml2, zlib, monocypher, stb),
lus (libultraship minus ImGui, SDL, scripting, MPQ), game (SoH's C: the
decomp in soh/src and soh/soh/*.c), port (SoH's C++), xbox (xbox/src: the
backends and main). Every object is linked (not archived), so SoH's
self-registering hooks (ShipInit) stay in; --gc-sections drops what nothing
reaches.

-k keeps going past failures and lists them all (the porting loop);
--cmd prints the compile command for the files matching PATTERN.

There is one build. The test tools (xbox/include/lusx_debug.h: the log and
black box files, the LED signal and the overlay) are in it, off until the port
menu's Debug page turns them on (Logging, Test overlay).
"""
import argparse
import concurrent.futures
import glob
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
LUS = os.path.join(ROOT, "libultraship")
SHIP = os.path.join(ROOT, "Shipwright")
SOH = os.path.join(SHIP, "soh")
DEPS = os.path.join(ROOT, "deps")
XBOX = os.path.join(ROOT, "xbox")

RXDK = os.environ.get("RXDK_ROOT") or os.path.join(os.environ.get("ProgramData") or r"C:\ProgramData", "RXDK")
LLVM = os.path.join(RXDK, "llvm", "xbox-windows-x64")
CLANG = os.path.join(LLVM, "bin", "clang.exe")
SDK_INC = os.path.join(RXDK, "sdk", "include")
SDK_LIB = os.path.join(RXDK, "sdk", "lib")
LIBCXX_INC = os.path.join(SDK_INC, "c++", "v1")

TITLE = "Ocarina-Of-Time-X"  # the files' names (build/Ocarina-Of-Time-X.xbe, out/*/XISO/Ocarina-Of-Time-X.iso)
# Ship of Harkinian's release the Shipwright/ tree is (9.3.1), for its build stamp
SOH_COMMIT = "f0599f9"
TITLE_NAME = "Ocarina Of Time-X"  # the name the dashboard shows (the XBE's title name, at most 40 characters)
# Team Resurgent's (TR, 0x5452) number 0x0020 in its list (the TitleID-DB repo). Each title ID has its own T: and U:
# drives, so Lus-X's configs and saves stay apart from these.
TITLE_ID = "0x54520020"
# The ID builds before 2026-11-13 had (LX-002): listed as an alternate ID so the game may mount that title's drives
# and copy its saves across once (xbox/src/xbox_save.cpp, CopyFromOldTitle)
OLD_TITLE_ID = "0x4C580002"
STACK = 262144
LIBS = ["libd3d8.lib", "libxgraphics.lib", "libdsound.lib", "libxapi.lib", "libc.lib", "libcpp.lib", "libkernel.lib"]

# ThinLTO, as Melee-X and Prime-X release (+3.5% fps alone on Melee-X's console, more with its PGO). It also saves RAM:
# seeing across files it finds the C++ that can't throw, which .eh_frame then leaves out (2.5 MB -> 1 MB). deps and xbox
# stay native: xbox holds what the code generator calls on its own (lusx_string.c), and in bitcode LTO would drop it.
LTO_GROUPS = ("lus", "game", "port")
# ...and these as one full-LTO module, not ThinLTO's one a file. The COFF linker keeps every object's .eh_frame whole:
# with SoH's C++ as 422 ThinLTO modules, each with its own copies of the headers' inline functions (nlohmann json,
# std::function, the hook templates), 640,000 of its 673,000 unwind records were for copies the link dropped, 17.5 MB
# of the 18.5 (2026-11-08). As one module, the copies are one, and what nothing calls is gone before code is made.
FULL_LTO_GROUPS = ("port",)
LTO = True
# lld imports a function into its callers up to this size; its default of 100 grew Prime-X's .text 37%, which a 64 MB
# console pays for and a 16 KB L1 doesn't repay
LTO_IMPORT_LIMIT = 10


def clang_resource_inc():
    found = sorted(glob.glob(os.path.join(LLVM, "lib", "clang", "*", "include")))
    if not found:
        raise SystemExit("build: no clang resource headers under %s" % LLVM)
    return found[-1]


def builtins_lib():
    found = sorted(glob.glob(os.path.join(LLVM, "lib", "clang", "*", "lib", "windows", "libclang_rt.builtins-i386.a")))
    if not found:
        raise SystemExit("build: no libclang_rt.builtins-i386.a under %s" % LLVM)
    return found[-1]


# ---------------------------------------------------------------- flags

# What Rxdk.Cli passes (RXDK 1.3.9), but -O2 (Rxdk.Cli: -O3; size matters
# here), SSE maths (as Majora-X), function/data sections for --gc-sections,
# and _WIN32 undefined for C as well as C++: libultraship then takes its
# POSIX paths (picolibc), never <windows.h>.
def rxdk_flags(lang):
    f = []
    if lang == "c++":
        # -fno-assume-unique-vtables: clang 24's exact dynamic_cast to a final class
        # with a virtual base (LUS's controller mappings) asserts in codegen on this target
        f += ["-nostdinc++", "-fexceptions", "-frtti", "-D_LIBCPP_ENABLE_CXX17_REMOVED_AUTO_PTR",
              "-I", LIBCXX_INC, "-fms-compatibility-version=19.20", "-D_GNU_SOURCE", "-fno-assume-unique-vtables"]
    f += ["-U_WIN32", "-U__MINGW32__",
          "-target", "i686-pc-windows-gnu", "-isystem", clang_resource_inc(), "-O2",
          "-fno-sanitize=undefined", "-march=pentium3", "-msse", "-mfpmath=sse", "-mlong-double-64",
          # (not -ffreestanding/-fno-builtin: with them no sqrtf, fabsf, memcpy or memset is ever inlined,
          # a call and picolibc's wrapper each, through all of the decomp's maths and the mixer)
          # nothing reads errno after maths: a sqrtf is then the instruction, with no call behind it for a negative
          "-fno-math-errno",
          "-fno-stack-protector", "-fms-extensions", "-fms-compatibility", "-nostdinc",
          "-include", "picolibc.h", "-include", os.path.join(XBOX, "include", "lusx_prelude.h"), "-D_XBOX", "-DXBOX", "-U_DEBUG", "-D__ASSERT_VERBOSE",
          "-femulated-tls", "-gline-tables-only", "-ffunction-sections", "-fdata-sections",
          # GCC bitfield layout everywhere: the game's and libultraship's GBI structs must agree
          # (Microsoft's, this target's default, makes Gfx 16 bytes); the SDK's bitfields are
          # all one type a struct, which lay out the same either way
          "-mno-ms-bitfields",
          # everything is linked statically: no mingw .refptr indirection for extern data
          "-fno-auto-import",
          "-Wno-c++11-narrowing", "-Wno-address-of-temporary", "-Wno-ignored-pragma-intrinsic", "-Wno-multichar",
          "-Wno-unused-command-line-argument", "-Wno-deprecated-enum-enum-conversion", "-Wno-pragma-pack"]
    f += ["-std=c++20"] if lang == "c++" else ["-std=gnu11"]
    return f


def cmake_sets(path):
    """set(NAME "value") lines of a CMake file, ${VAR}s left for the caller."""
    out = {}
    with open(path, encoding="utf-8") as fh:
        for m in re.finditer(r'^\s*set\(\s*(\w+)\s+"([^"]*)"', fh.read(), re.M):
            out.setdefault(m.group(1), m.group(2))
    return out


def cvar_defines():
    # SoH's names first (lus-cvars.cmake renames LUS's), then LUS's defaults
    sets = {}
    for p in (os.path.join(SHIP, "CMake", "soh-cvars.cmake"), os.path.join(SHIP, "CMake", "lus-cvars.cmake"),
              os.path.join(LUS, "cmake", "cvars.cmake")):
        for k, v in cmake_sets(p).items():
            sets.setdefault(k, v)
    for k, v in sets.items():
        sets[k] = re.sub(r"\$\{(\w+)\}", lambda m: sets.get(m.group(1), ""), v)
    return ['-D%s="%s"' % (k, v) for k, v in sorted(sets.items()) if k.startswith("CVAR_")]


# _LANGUAGE_C: LUS's GBI headers define it for _MSC_VER or __GNUC__, and in
# MS-compatibility mode the RXDK's clang has neither
COMMON_DEFS = ["-DNDEBUG", "-D_LANGUAGE_C", "-DNOMINMAX", "-DF3DEX_GBI_2", "-DCONTROLLERBUTTONS_T=uint32_t", "-DSPDLOG_COMPILED_LIB",
               "-DSPDLOG_ACTIVE_LEVEL=2",  # info and up; trace/debug compiled out
               "-DSPDLOG_NO_THREAD_ID", "-DLUS_XBOX", "-DLUS_NO_IMGUI",
               # Fast3D's texture cache: each entry keeps its GPU texture until the slot is reused
               "-DTEXTURE_CACHE_MAX_SIZE=512",
               "-DLUS_VERSION_EPOCH=2", "-DLUS_VERSION_MAJOR=0", "-DLUS_VERSION_MINOR=0", "-DLUS_VERSION_PATCH=0"]

DEP_INCS = [os.path.join(DEPS, "spdlog", "include"), os.path.join(DEPS, "json", "include"),
            os.path.join(DEPS, "tinyxml2"), os.path.join(DEPS, "zlib"), os.path.join(DEPS, "stb"),
            os.path.join(DEPS, "thread-pool", "include"), os.path.join(DEPS, "monocypher", "src"),
            os.path.join(DEPS, "monocypher", "src", "optional"), os.path.join(DEPS, "dr_libs")]

GEN = os.path.join(BUILD, "gen")

LUS_INCS = [os.path.join(XBOX, "include"), os.path.join(GEN, "lus"), os.path.join(LUS, "include"),
            os.path.join(LUS, "src")] + DEP_INCS

# Dear ImGui, real, for SoH's files and its own (first, ahead of xbox/include's types-only imgui.h, which
# libultraship's files see); its configuration is xbox/include/sohx_imconfig.h
IMGUI = os.path.join(DEPS, "imgui")
IMGUI_DEFS = ['-DIMGUI_USER_CONFIG="sohx_imconfig.h"']

# soh/CMakeLists.txt's include directories: include/, src/, assets/ and soh/ itself ("soh/OTRGlobals.h")
SOH_INCS = [IMGUI, os.path.join(XBOX, "include"), os.path.join(SOH, "include"), os.path.join(SOH, "src"),
            os.path.join(SOH, "assets"), SOH, os.path.join(LUS, "include"), os.path.join(GEN, "soh")] + DEP_INCS

SOH_DEFS = ["-D_CONSOLE", "-DLOG_LEVEL_GAME_PRINTS=6", "-DUNICODE", "-D_UNICODE"] + IMGUI_DEFS

# SoH has no precompiled headers of its own; these are the headers nearly every file of a group reads, as clang
# PCHs (a C++ file reading z64.h does it inside extern "C", so the C++ list stays with the C++ side)
SOH_PCH_CXX = ["libultraship/bridge/consolevariablebridge.h", "ship/Context.h", "ship/resource/ResourceManager.h",
               "ship/window/Window.h", "spdlog/spdlog.h", "string", "vector", "map", "unordered_map", "filesystem",
               "fstream", "algorithm", "functional", "variant", "memory",
               os.path.join(SOH, "soh", "Enhancements", "game-interactor", "GameInteractor.h")]
SOH_PCH_C = [os.path.join(SOH, "include", n) for n in ("global.h", "z64.h", "macros.h", "functions.h", "variables.h")]


# ---------------------------------------------------------------- sources

def rel(p):
    return os.path.relpath(p, ROOT).replace("\\", "/")


def gather(base, patterns, exclude):
    out = []
    for pat in patterns:
        out += glob.glob(os.path.join(base, pat), recursive=True)
    ex = [re.compile(e) for e in exclude]
    return sorted(p for p in set(out) if not any(e.search(rel(p)) for e in ex))


# libultraship: what the Xbox does not have or does not want
LUS_EXCLUDE = [
    r"/fast/backends/gfx_(sdl2|opengl|dxgi|direct3d11|direct3d_common|metal|metal_shader)\.cpp$",
    r"/fast/Fast3dGui\.cpp$",
    r"/ship/window/gui/",                       # ImGui
    r"/libultraship/window/gui/",               # ImGui: gfx debugger, input editor
    r"/physicaldevice/SDLAddRemoveDeviceEventHandler\.cpp$",  # a GuiWindow polling SDL events
    r"/audio/(SDL|Wasapi|CoreAudio)AudioPlayer\.cpp$",
    r"/ship/port/mobile/",
    r"/ship/scripting/", r"/ship/security/",    # tcc scripting and its keys
    r"\.mm$", r"/utils/AppleFolderManager",
    r"/ship/resource/archive/OtrArchive\.cpp$",  # MPQ
]

# soh/CMakeLists.txt's own exclusions from the decomp (src/), then what the Xbox leaves out
SOH_EXCLUDE = [
    r"/soh/src/dmadata/", r"/soh/src/elf_message/",
    r"/soh/src/libultra/(io|os|rmon)/",
    r"/soh/src/libultra/libc/(?!sprintf\.c$)",  # (CMake puts sprintf.c back)
    r"/soh/src/libultra/gu/(cosf|lookat|lookathil|perspective|position|sinf|sqrtf|us2dex)\.c$",
    r"/soh/src/boot/build\.c$",  # written by the PC CMake configure; build/gen/soh/build.c is the Xbox's
    r"/soh/soh/Extractor/",  # the archives are made on a PC (tools/extract)
    # speech: SAPI, eSpeak and macOS only
    r"/soh/soh/Enhancements/speechsynthesizer/(Darwin|SAPI|ESpeak)",
    # online play (SDL2_net)
    r"/soh/soh/Network/",
]

# SoH's GUI: its menus, windows and editors, which call Dear ImGui (xbox/include/imgui.h has only its types). What the
# game needs from them is in xbox/src/port/port_nogui.cpp.
SOH_GUI_EXCLUDE = [
    # (its widgets and item tables stay: game files call them, with nothing drawn; and so does the save editor, for
    # its name decoding: --gc-sections drops the rest of a file that only a little of is used)
    r"/soh/soh/SohGui/(?!(UIWidgets|ImGuiUtils)\.cpp$)",
    r"/soh/soh/Enhancements/debugger/(MessageViewer|SohConsoleWindow|SohGfxDebuggerWindow|SohStatsWindow|actorViewer|"
    r"colViewer|dlViewer|hookDebugger|valueViewer)\.cpp$",
    r"/soh/soh/Enhancements/controls/(InputViewer|SohInputEditorWindow|Mapper)\.cpp$",  # (Mapper: SDL joysticks)
    r"/soh/soh/Enhancements/timesplits/",
    r"/soh/soh/Notification/",
]

# The normal game only (the user's call, 2026-11-08): file select offers no randomizer, boss rush or speedrun quest
# (FileSelectEnhancements.cpp), so what only makes or shows a randomizer seed goes: the world graph's 35 area files, the
# hint lists, the plandomizer and two tracker windows. The randomizer's in-game side stays (the game's code asks
# IS_RANDO all over); the 38 functions it calls in these files are empty in xbox/src/port/sohx_normal_only.cpp.
SOH_NORMAL_ONLY_EXCLUDE = [
    r"/soh/soh/Enhancements/randomizer/location_access/",
    r"/soh/soh/Enhancements/randomizer/3drando/hint_list",
    r"/soh/soh/Enhancements/randomizer/(Plandomizer|randomizer_hint_tracker|randomizer_tracker_windows)\.cpp$",
]


def groups():
    stb_impl = os.path.join(GEN, "stb_impl.c")
    g = []
    g.append(dict(
        name="deps", incs=DEP_INCS, defs=COMMON_DEFS, extra=["-w", "-ffreestanding", "-fno-builtin"],
        srcs=[os.path.join(DEPS, "spdlog", "src", n + ".cpp") for n in
              ("spdlog", "stdout_sinks", "color_sinks", "file_sinks", "async", "cfg", "bundled_fmtlib_format")] +
             [os.path.join(DEPS, "tinyxml2", "tinyxml2.cpp")] +
             [os.path.join(DEPS, "zlib", n + ".c") for n in
              ("adler32", "crc32", "inflate", "inffast", "inftrees", "zutil")] +
             [os.path.join(DEPS, "monocypher", "src", "monocypher.c"),
              os.path.join(DEPS, "monocypher", "src", "optional", "monocypher-ed25519.c"), stb_impl]))
    g.append(dict(
        # Dear ImGui's core, for SoH's calls into it (never drawn: sohx_imconfig.h); cold, so -Os
        name="imgui", incs=[IMGUI, os.path.join(XBOX, "include")], defs=COMMON_DEFS + IMGUI_DEFS,
        extra=["-w", "-Os"],
        srcs=[os.path.join(IMGUI, n + ".cpp") for n in ("imgui", "imgui_draw", "imgui_tables", "imgui_widgets")]))
    g.append(dict(
        name="lus", incs=LUS_INCS, defs=COMMON_DEFS + cvar_defines(),
        extra=["-Wno-everything"],
        # the display-list interpreter and the NV2A backend are the hottest CPU code at 60 fps: -O3
        hot=[r"/fast/interpreter\.cpp$", r"/fast/backends/gfx_direct3d8\.cpp$"], hot_opt="-O3",
        srcs=gather(os.path.join(LUS, "src"), ["**/*.cpp", "**/*.c"], LUS_EXCLUDE)))
    g.append(dict(
        name="game", incs=SOH_INCS, defs=COMMON_DEFS + cvar_defines() + SOH_DEFS,
        # as SoH builds on an x86 PC: signed char, GCC bitfields; and as its CMake, the decomp's
        # pointer/integer mixing allowed (errors by default in this clang)
        extra=["-w", "-fno-strict-aliasing", "-fwrapv", "-mno-ms-bitfields",
               "-Wno-error=incompatible-pointer-types", "-Wno-error=int-conversion",
               "-Wno-error=incompatible-function-pointer-types", "-Wno-error=implicit-function-declaration",
               "-Wno-error=implicit-int", "-Wno-error=return-type"],
        pch=SOH_PCH_C,
        # files that define what those headers declare otherwise (build.c's const version, stubs.c's buffers):
        # their own includes only, as on a PC
        nopch=[r"/gen/soh/build\.c$", r"/soh/soh/stubs\.c$"],
        srcs=gather(SOH, ["src/**/*.c", "soh/**/*.c"], SOH_EXCLUDE + SOH_GUI_EXCLUDE + SOH_NORMAL_ONLY_EXCLUDE)))
    g.append(dict(
        name="port", incs=SOH_INCS, defs=COMMON_DEFS + cvar_defines() + SOH_DEFS,
        # -Os: SoH's C++ is mostly cold (enhancement hooks, the randomizer, resource loaders); what runs
        # every frame stays -O2 (hot)
        extra=["-w", "-fno-strict-aliasing", "-mno-ms-bitfields", "-Os"],
        hot=[r"/soh/soh/frame_interpolation\.cpp$", r"/soh/soh/Enhancements/game-interactor/",
             r"/soh/soh/OTRGlobals\.cpp$"],
        pch=SOH_PCH_CXX,
        # with xbox/src/port: what the game needs from the GUI files left out; and libultraship's GUI window base
        # classes, which SoH's windows derive from (built here, against the real ImGui: libultraship's group has none)
        srcs=gather(SOH, ["soh/**/*.cpp"], SOH_EXCLUDE + SOH_GUI_EXCLUDE + SOH_NORMAL_ONLY_EXCLUDE) +
             gather(XBOX, ["src/port/**/*.cpp"], []) +
             [os.path.join(LUS, "src", "ship", "window", "gui", n + ".cpp") for n in ("GuiElement", "GuiWindow")]))
    g.append(dict(
        # (as libultraship's: xbox/include's imgui.h, not the real one)
        name="xbox", incs=[os.path.join(XBOX, "src")] + SOH_INCS[1:] + LUS_INCS, defs=COMMON_DEFS + cvar_defines(),
        # as the game's: the mixer's ADPCM nibble sign-extension overflows int (UB without -fwrapv)
        extra=["-Wno-everything", "-fwrapv", "-fno-strict-aliasing"],
        # (src/audio: UltraX's MMX mixer, set up for Majora's Mask's DMEM layout; SoH's own mixer.c for now)
        srcs=gather(XBOX, ["src/**/*.cpp", "src/**/*.c"], [r"^xbox/src/port/", r"^xbox/src/audio/"])))
    return g


# ---------------------------------------------------------------- generated

def write_if_changed(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    old = None
    if os.path.isfile(path):
        with open(path, encoding="utf-8", newline="") as fh:
            old = fh.read()
    if old != text:
        with open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write(text)


def generate():
    # a header and not a -D, so switching rebuilds only the files that include it: a -D is in every command. 1: the test
    # tools are in every build, off until the Debug page turns them on; 0 here would leave them out
    for inc in ("lus", "soh"):
        write_if_changed(os.path.join(GEN, inc, "lusx_debug_config.h"), "#define LUSX_DEBUG_TOOLS 1\n")
    write_if_changed(os.path.join(GEN, "stb_impl.c"),
                     "#define STB_IMAGE_IMPLEMENTATION\n#define STBI_NO_THREAD_LOCALS\n#define STBI_NO_SIMD\n#include \"stb_image.h\"\n")
    write_if_changed(os.path.join(GEN, "lus", "ship", "install_config.h"),
                     '#define CMAKE_INSTALL_PREFIX "D:\\\\"\n#define DISABLE_DLL_LOADER\n')
    # soh/src/boot/build.c, from build.c.in (SoH's CMake writes it into the tree;
    # here it goes to build/gen and the tree's copy, if any, is left alone)
    with open(os.path.join(SOH, "src", "boot", "build.c.in"), encoding="utf-8") as fh:
        text = fh.read()
    with open(os.path.join(SHIP, "CMakeLists.txt"), encoding="utf-8") as fh:
        ver = re.search(r"project\(Ship VERSION (\d+)\.(\d+)\.(\d+)", fh.read())
    # the build name as CMake makes it: "Dewey" and the NATO word for the patch number
    nato = ("Alfa Bravo Charlie Delta Echo Foxtrot Golf Hotel India Juliett Kilo Lima Mike November Oscar Papa Quebec "
            "Romeo Sierra Tango Uniform Victor Whiskey Xray Yankee Zulu").split()
    subs = {"PROJECT_BUILD_NAME": "Dewey " + nato[int(ver.group(3))], "PROJECT_TEAM": "github.com/harbourmasters",
            "CMAKE_PROJECT_VERSION_MAJOR": ver.group(1), "CMAKE_PROJECT_VERSION_MINOR": ver.group(2),
            "CMAKE_PROJECT_VERSION_PATCH": ver.group(3), "CMAKE_PROJECT_GIT_BRANCH": "Ocarina-Of-Time-X",
            "CMAKE_PROJECT_GIT_COMMIT_HASH": SOH_COMMIT,
            "CMAKE_PROJECT_GIT_COMMIT_TAG": ""}
    text = re.sub(r"@(\w+)@", lambda m: subs.get(m.group(1), ""), text)
    write_if_changed(os.path.join(GEN, "soh", "build.c"), text)


def extra_sources():
    return {"game": [os.path.join(GEN, "soh", "build.c")]}


# ---------------------------------------------------------------- compiling

def obj_path(group, src):
    r = os.path.relpath(src, ROOT)
    if r.startswith(".."):
        r = os.path.basename(src)
    # bitcode and native objects apart, so a --no-lto build never recompiles the other's
    obj = "obj-lto" if LTO and group in LTO_GROUPS else "obj"
    return os.path.join(BUILD, obj, group, os.path.splitext(r)[0] + ".obj")


def lang_of(src):
    return "c++" if src.endswith((".cpp", ".cc", ".cxx")) else "c"


def compile_cmd(g, src, obj, pch=None):
    lang = lang_of(src)
    cmd = [CLANG] + rxdk_flags(lang) + g["extra"] + g["defs"]
    for inc in g["incs"]:
        cmd += ["-I", inc]
    cmd += ["-I", SDK_INC]
    hot = any(re.search(h, rel(src)) for h in g.get("hot", []))
    if pch:
        # a PCH is made at the group's optimisation: a hot file takes its headers as text
        cmd += ["-include", pch[:-len(".pch")]] if hot else ["-include-pch", pch]
    if hot:
        cmd += [g.get("hot_opt", "-O2")]
    if LTO and g["name"] in LTO_GROUPS:
        cmd += ["-flto" if g["name"] in FULL_LTO_GROUPS else "-flto=thin"]
    cmd += ["-x", lang, "-c", src, "-o", obj, "-MD", "-MF", obj + ".d"]
    return cmd


def deps_newer(obj, cmd_text):
    d = obj + ".d"
    c = obj + ".cmd"
    if not (os.path.isfile(obj) and os.path.isfile(d) and os.path.isfile(c)):
        return True
    with open(c, encoding="utf-8") as fh:
        if fh.read() != cmd_text:
            return True
    t = os.path.getmtime(obj)
    with open(d, encoding="utf-8", errors="replace") as fh:
        text = fh.read().replace("\\\n", " ")
    m = re.match(r"^.*?\.obj:\s", text, re.S)
    body = text[m.end():] if m else text
    for dep in body.split():
        dep = dep.replace("\\ ", " ")
        try:
            if os.path.getmtime(dep) > t:
                return True
        except OSError:
            return True
    return False


def run(cmd, log):
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    out = (r.stdout or "") + (r.stderr or "")
    if r.returncode != 0:
        os.makedirs(os.path.dirname(log), exist_ok=True)
        with open(log, "w", encoding="utf-8") as fh:
            fh.write(" ".join(cmd) + "\n\n" + out)
    return r.returncode, out


def build_pch(g, lang):
    headers = g.get("pch")
    if not headers:
        return None
    hdr = os.path.join(GEN, "pch", "%s_%s.h" % (g["name"], lang.replace("+", "x")))
    text = "".join('#include "%s"\n' % h.replace("\\", "/") if os.path.isabs(h) else "#include <%s>\n" % h
                   for h in headers)
    write_if_changed(hdr, text)
    out = hdr + ".pch"
    cmd = [CLANG] + rxdk_flags(lang) + g["extra"] + g["defs"]
    for inc in g["incs"]:
        cmd += ["-I", inc]
    cmd += ["-I", SDK_INC, "-x", lang + "-header", hdr, "-o", out, "-MD", "-MF", out + ".d"]
    text_cmd = "\n".join(cmd)
    if deps_newer_pch(out, text_cmd):
        rc, msg = run(cmd, os.path.join(BUILD, "log", g["name"], "pch_%s.log" % lang))
        if rc != 0:
            print("build: %s's %s precompiled header failed:\n%s" % (g["name"], lang, first_errors(msg)))
            return False
        with open(out + ".cmd", "w", encoding="utf-8") as fh:
            fh.write(text_cmd)
    return out


def deps_newer_pch(out, cmd_text):
    # the .d names the .pch as its target, not an .obj
    d = out + ".d"
    c = out + ".cmd"
    if not (os.path.isfile(out) and os.path.isfile(d) and os.path.isfile(c)):
        return True
    with open(c, encoding="utf-8") as fh:
        if fh.read() != cmd_text:
            return True
    t = os.path.getmtime(out)
    with open(d, encoding="utf-8", errors="replace") as fh:
        text = fh.read().replace("\\\n", " ")
    body = text.split(": ", 1)[1] if ": " in text else text
    for dep in body.split():
        try:
            if os.path.getmtime(dep.replace("\\ ", " ")) > t:
                return True
        except OSError:
            return True
    return False


def first_errors(text, n=3):
    lines = [l for l in text.splitlines() if " error: " in l or l.startswith("error:") or "fatal error" in l]
    return "\n".join(lines[:n]) if lines else "\n".join(text.splitlines()[-5:])


def compile_group(g, jobs, keep_going, file_filter, show_cmd):
    srcs = g["srcs"] + extra_sources().get(g["name"], [])
    if file_filter:
        srcs = [s for s in srcs if re.search(file_filter, rel(s))]
    pchs = {}
    for lang in ("c", "c++"):
        if g.get("pch") and any(lang_of(s) == lang for s in srcs):
            # each language's PCH holds its own header list: C++ SoH's, C the decomp's
            if (lang == "c++") == (g["name"] == "port"):
                p = build_pch(g, lang)
                if p is False:
                    return [], ["%s precompiled header" % lang]
                pchs[lang] = p
    todo, objs = [], []
    for s in srcs:
        o = obj_path(g["name"], s)
        nopch = any(re.search(p, rel(s)) for p in g.get("nopch", []))
        cmd = compile_cmd(g, s, o, None if nopch else pchs.get(lang_of(s)))
        if show_cmd and re.search(show_cmd, rel(s)):
            print(subprocess.list2cmdline(cmd))
        objs.append(o)
        text = "\n".join(cmd)
        if deps_newer(o, text):
            todo.append((s, o, cmd, text))
    failed = []
    if not todo:
        return objs, failed
    print("build: %s: %d of %d to compile" % (g["name"], len(todo), len(srcs)))
    t0 = time.time()
    done = 0

    def one(item):
        s, o, cmd, text = item
        os.makedirs(os.path.dirname(o), exist_ok=True)
        log = os.path.join(BUILD, "log", g["name"], os.path.splitext(rel(s))[0].replace("/", "_") + ".log")
        rc, out = run(cmd, log)
        if rc == 0:
            with open(o + ".cmd", "w", encoding="utf-8") as fh:
                fh.write(text)
            if os.path.isfile(log):
                os.remove(log)
        return s, rc, out

    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        futs = [ex.submit(one, it) for it in todo]
        for f in concurrent.futures.as_completed(futs):
            s, rc, out = f.result()
            done += 1
            if rc != 0:
                failed.append(s)
                print("  FAIL %s\n%s" % (rel(s), "\n".join("       " + l for l in first_errors(out).splitlines())))
                if not keep_going:
                    for x in futs:
                        x.cancel()
                    break
            if done % 100 == 0:
                print("  %d/%d (%.0f s)" % (done, len(todo), time.time() - t0))
    print("build: %s: %d compiled, %d failed (%.0f s)" % (g["name"], done - len(failed), len(failed),
                                                           time.time() - t0))
    return objs, failed


# ---------------------------------------------------------------- linking

EH_BEGIN = '.section .eh_frame,"dr"\n.globl ___eh_frame_start\n___eh_frame_start:\n'
EH_END = ('.section .eh_frame$z,"dr"\n.globl ___eh_frame_end\n.globl ___rxdk_eh_frame_end_requires_rxdk_tools_1_3_6\n'
          '___eh_frame_end:\n___rxdk_eh_frame_end_requires_rxdk_tools_1_3_6:\n')


def archive(name, objs):
    """deps and libultraship as static libraries, as on the PC: an object is linked only when
    something needs it (SoH's own stubs.c stands in for several of LUS's libultra files)."""
    lib = os.path.join(BUILD, "lib", name + ("-lto" if LTO and name in LTO_GROUPS else "") + ".lib")
    os.makedirs(os.path.dirname(lib), exist_ok=True)
    if os.path.isfile(lib) and all(os.path.getmtime(o) <= os.path.getmtime(lib) for o in objs):
        return lib
    if os.path.isfile(lib):
        os.remove(lib)
    rsp = lib + ".rsp"
    with open(rsp, "w", encoding="utf-8") as fh:
        fh.write("\n".join('"%s"' % o.replace("\\", "/") for o in objs) + "\n")
    r = subprocess.run([os.path.join(LLVM, "bin", "llvm-ar.exe"), "rcs", lib, "@" + rsp])
    if r.returncode != 0:
        raise SystemExit("build: archiving %s failed" % name)
    return lib


def link(objs):
    lk = os.path.join(BUILD, "link")
    os.makedirs(lk, exist_ok=True)
    eh = []
    for name, text in (("rxdk_eh_begin", EH_BEGIN), ("rxdk_eh_end", EH_END)):
        s = os.path.join(lk, name + ".S")
        write_if_changed(s, text)
        o = os.path.join(lk, name + ".o")
        r = subprocess.run([CLANG, "-target", "i686-pc-windows-gnu", "-march=pentium3", "-mlong-double-64", "-c", s,
                            "-o", o])
        if r.returncode != 0:
            raise SystemExit("build: assembling %s failed" % s)
        eh.append(o)
    rsp = os.path.join(lk, "objs.rsp")
    with open(rsp, "w", encoding="utf-8") as fh:
        fh.write("\n".join('"%s"' % o.replace("\\", "/") for o in objs) + "\n")
    exe = os.path.join(BUILD, TITLE + ".exe")
    cmd = [CLANG, eh[0], "@" + rsp] + [os.path.join(SDK_LIB, l) for l in LIBS] + [builtins_lib(), eh[1],
           "-target", "i686-pc-windows-gnu", "-march=pentium3", "-nostdlib", "-nostartfiles",
           "-Wl,--image-base=0x10000", "-Wl,--gc-sections", "-Wl,--error-limit=0", "-Wl,-Map=" + os.path.join(lk, TITLE + ".map"),
           # xbox_glue.cpp's strlen, which names the caller of a strlen on a bad pointer
           "-Wl,--wrap=strlen",
           "-fuse-ld=lld", "-e", "XapiTitleStartup", "-o", exe]
    if LTO:
        # -emulated-tls as the compiles' -femulated-tls: it's a code generator's option, not kept in the bitcode, and
        # without it LTO quietly makes native TLS (__tls_index). The cache: a file's change recompiles that module.
        cmd += ["-Wl,-mllvm,-emulated-tls", "-Wl,-mllvm,-import-instr-limit=%d" % LTO_IMPORT_LIMIT,
                "-Wl,--thinlto-jobs=%d" % (os.cpu_count() or 4),
                "-Wl,--thinlto-cache-dir=" + os.path.join(BUILD, "lto-cache").replace("\\", "/")]
    print("build: linking %d objects" % len(objs))
    rc, out = run(cmd, os.path.join(BUILD, "log", "link.log"))
    if rc != 0:
        undef = sorted(set(re.findall(r"undefined symbol: (.+)", out)))
        print("build: link failed: %d undefined symbols (build/log/link.log)" % len(undef))
        for u in undef[:60]:
            print("  " + u)
        raise SystemExit(1)
    xbe = os.path.join(BUILD, TITLE + ".xbe")
    # the dashboard's title and save images, made from the game's Ocarina icon (tools/make_icons.py)
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import make_icons
    icons = make_icons.main()
    images = ["/TITLEIMAGE:" + icons[0], "/DEFAULTSAVEIMAGE:" + icons[1]] if icons else []
    r = subprocess.run([os.path.join(RXDK, "tools", "imagebld.exe"), "/in:" + exe, "/out:" + xbe, "/nologo",
                        "/stack:%d" % STACK, "/nolibwarn", "/TESTID:" + TITLE_ID, "/TESTALTID:" + OLD_TITLE_ID,
                        "/TESTNAME:" + TITLE_NAME,
                        "/TESTVERSION:1"] + images, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stdout.write(r.stdout + r.stderr)
        raise SystemExit("build: imagebld failed")
    out = os.path.join(ROOT, "out", "Release")
    stage = os.path.join(out, "Build", TITLE)
    if os.path.isdir(stage):
        shutil.rmtree(stage)
    os.makedirs(stage)
    shutil.copy(xbe, os.path.join(stage, "default.xbe"))
    shutil.copy(os.path.join(XBOX, "assets", "menu.ttf"), stage)  # the port's menu reads it when it opens
    data = os.path.join(ROOT, "data")
    archives = sorted(glob.glob(os.path.join(data, "*.o2r")))
    for a in archives:
        shutil.copy(a, stage)
    os.makedirs(os.path.join(out, "XISO"), exist_ok=True)
    iso = os.path.join(out, "XISO", TITLE + ".iso")
    r = subprocess.run([os.path.join(RXDK, "tools", "xdvdfs.exe"), "pack", stage, iso], capture_output=True,
                       text=True)
    if r.returncode != 0:
        sys.stdout.write(r.stdout + r.stderr)
        raise SystemExit("build: xdvdfs failed")
    print("build: %s (%.2f MB), %s (%.1f MB; archives: %s)" % (
        rel(xbe), os.path.getsize(xbe) / 1048576.0, rel(iso), os.path.getsize(iso) / 1048576.0,
        ", ".join(os.path.basename(a) for a in archives) or "none -- run setup.py: it makes oot.o2r and soh.o2r in data/"))
    size = subprocess.run([os.path.join(LLVM, "bin", "llvm-size.exe"), "-A", exe], capture_output=True, text=True)
    sys.stdout.write(size.stdout)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-k", "--keep-going", action="store_true")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--only", help="comma-separated groups to compile (no link)")
    ap.add_argument("--file", help="compile only sources whose path matches this regex (no link)")
    ap.add_argument("--cmd", help="print the compile command of sources matching this regex")
    ap.add_argument("--no-link", action="store_true")
    ap.add_argument("--clean", action="store_true")
    ap.add_argument("--no-lto", action="store_true", help="native objects: a quicker link, or to compare")
    a = ap.parse_args()
    global LTO
    LTO = not a.no_lto
    if a.clean and os.path.isdir(BUILD):
        shutil.rmtree(BUILD)
    generate()
    # its "built" line is __DATE__ and __TIME__: compiled every time, so a log says which build it's from
    os.utime(os.path.join(XBOX, "src", "xbox_main.cpp"))
    only = set(a.only.split(",")) if a.only else None
    all_objs, all_failed, libs = [], [], []
    for g in groups():
        if only and g["name"] not in only:
            continue
        objs, failed = compile_group(g, a.jobs, a.keep_going, a.file, a.cmd)
        if g["name"] in ("deps", "imgui", "lus") and not failed and not (a.no_link or only or a.file):
            libs.append(archive(g["name"], objs))
        else:
            all_objs += objs
        all_failed += failed
        if failed and not a.keep_going:
            break
    if all_failed:
        print("build: %d files failed (logs in build/log)" % len(all_failed))
        raise SystemExit(1)
    if not (a.no_link or only or a.file):
        # the game's objects, then libultraship and its dependencies as libraries
        link(all_objs + libs)


if __name__ == "__main__":
    main()
