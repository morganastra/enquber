#!/usr/bin/env python3
"""Deploy and run the Windows cross build under Wine.

windeployqt (from the MSYS2 SDK) copies the Qt DLLs and plugins next to a copy
of enquber.exe under build/windows/deploy, and this tool then walks the PE
import table to copy the remaining MinGW runtime and third-party DLLs that
windeployqt does not know about (libqrencode, ICU, and friends).  Finally the
deployed executable is started under Wine.

The Wine prefix defaults to a dedicated one so the app does not touch the
user's default prefix; the first run initializes it and may take a moment.

Examples:

    packaging/windows/wine-run.py                  # deploy, then run
    packaging/windows/wine-run.py --skip-deploy    # run what is already deployed
    packaging/windows/wine-run.py --debug          # keep Wine's debug output

Exit codes: 0 when the app exits cleanly, 1 when deployment or Wine fails.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SDK_DEFAULT = Path(os.environ.get("ENQUBER_MINGW_PREFIX") or Path.home() / ".local/msys2-mingw64")
WINEPREFIX_DEFAULT = Path(os.environ.get("WINEPREFIX") or Path.home() / ".local/share/wineprefixes/enquber")
OBJDUMP = "x86_64-w64-mingw32-objdump"
# Runtime DLLs are taken from the compiler that built the app first; the SDK
# copy is the fallback.  Debian/Fedora put the sysroot elsewhere.
MINGW_BIN_CANDIDATES = [
    "/usr/x86_64-w64-mingw32/bin",
    "/usr/x86_64-w64-mingw32/sys-root/mingw/bin",
]

# DLLs Windows itself provides.  An import that is in neither the MinGW/SDK
# index nor one of these is a missing dependency: the app would fail to start,
# so deploy() stops on it instead of shipping an incomplete tree.
WINDOWS_SYSTEM_DLLS = {
    "advapi32.dll", "authz.dll", "avrt.dll", "bcrypt.dll", "cabinet.dll", "cfgmgr32.dll",
    "combase.dll", "comctl32.dll", "comdlg32.dll", "crypt32.dll", "d2d1.dll",
    "d3d9.dll", "d3d10.dll", "d3d11.dll", "d3d12.dll", "d3dcompiler_47.dll",
    "dbghelp.dll", "dnsapi.dll", "dsound.dll", "dwmapi.dll", "dwrite.dll",
    "dxgi.dll", "dxva2.dll", "evr.dll", "gdi32.dll", "gdiplus.dll", "hid.dll",
    "imm32.dll", "iphlpapi.dll", "kernel32.dll", "ksuser.dll", "mf.dll",
    "mfplat.dll", "mfreadwrite.dll", "mpr.dll", "msimg32.dll", "msvcrt.dll",
    "mswsock.dll", "ncrypt.dll", "netapi32.dll", "normaliz.dll", "ntdll.dll",
    "ole32.dll", "oleacc.dll", "oleaut32.dll", "opengl32.dll", "powrprof.dll",
    "propsys.dll", "psapi.dll", "rpcrt4.dll", "secur32.dll", "setupapi.dll",
    "shcore.dll", "shell32.dll", "shlwapi.dll", "ucrtbase.dll", "urlmon.dll",
    "user32.dll", "userenv.dll", "usp10.dll", "uxtheme.dll", "version.dll",
    "wer.dll", "windowscodecs.dll", "winhttp.dll", "wininet.dll", "winmm.dll",
    "winspool.drv", "wintrust.dll", "ws2_32.dll", "wtsapi32.dll",
}
WINDOWS_SYSTEM_PREFIXES = ("api-ms-win-", "ext-ms-")


def dll_dependencies(path):
    """Return the DLL names imported by a PE file."""
    output = subprocess.run([OBJDUMP, "-p", str(path)], check=True,
                            capture_output=True, text=True).stdout
    return re.findall(r"DLL Name: (\S+)", output)


def is_windows_system_dll(name):
    """Whether name is a DLL Windows provides itself."""
    key = name.lower()
    return key in WINDOWS_SYSTEM_DLLS or key.startswith(WINDOWS_SYSTEM_PREFIXES)


def collect_dlls(target_dir, roots, index, dependencies=dll_dependencies):
    """Copy the DLLs that roots import from index into target_dir.

    Returns (copied, missing): the names copied, and the imported names that
    are in neither index nor the Windows system set.  dependencies() yields the
    imports of one file and is injectable for tests.
    """
    queue = list(roots)
    seen = {path.name.lower() for path in queue}
    copied, missing = [], []
    while queue:
        for name in dependencies(queue.pop(0)):
            key = name.lower()
            if key in seen:
                continue
            seen.add(key)
            source = index.get(key)
            if source is None:
                if not is_windows_system_dll(key):
                    missing.append(name)
                continue
            shutil.copy2(source, target_dir / source.name)
            copied.append(source.name)
            queue.append(target_dir / source.name)
    return copied, missing


def wine_env(args):
    env = os.environ.copy()
    env["WINEPREFIX"] = str(args.wineprefix)
    env.setdefault("WINEDLLOVERRIDES", "mscoree,mshtml=")
    if args.debug:
        env.pop("WINEDEBUG", None)
    else:
        env["WINEDEBUG"] = "-all"
    return env


def deploy(args, exe, env):
    """Copy the exe and its DLLs/plugins into build/windows/deploy."""
    target_dir = args.build_dir / "deploy"
    shutil.rmtree(target_dir, ignore_errors=True)
    target_dir.mkdir(parents=True)
    shutil.copy2(exe, target_dir / exe.name)

    windeployqt = args.sdk / "mingw64/bin/windeployqt6.exe"
    if not windeployqt.is_file():
        sys.exit(f"error: {windeployqt} not found; fetch the SDK with "
                 "packaging/windows/mingw-sdk.py")
    print(f"Deploying Qt runtime with {windeployqt.name} ...", flush=True)
    subprocess.run(
        [args.wine, str(windeployqt), "--release", "--no-translations",
         "--no-compiler-runtime", "--no-opengl-sw", "--no-system-d3d-compiler",
         "--skip-plugin-types", "generic,networkinformation,tls",
         str(target_dir / exe.name)],
        env=env, check=True,
    )

    # windeployqt only copies Qt's own libraries; walk the import table for the
    # rest (libqrencode, ICU, the MinGW runtime, ...).
    search_dirs = [Path(path) for path in MINGW_BIN_CANDIDATES if Path(path).is_dir()]
    search_dirs.append(args.sdk / "mingw64/bin")
    index = {}
    for directory in search_dirs:
        for entry in directory.iterdir():
            if entry.suffix.lower() == ".dll":
                index.setdefault(entry.name.lower(), entry)

    copied, missing = collect_dlls(
        target_dir, [*target_dir.rglob("*.dll"), target_dir / exe.name], index)
    if missing:
        sys.exit("error: unresolved import(s): " + ", ".join(sorted(missing)) +
                 "\n  neither in the MinGW SDK nor a known Windows system DLL; "
                 "the app would fail to start")
    print(f"Copied {len(copied)} runtime libraries: {', '.join(sorted(copied))}", flush=True)
    return target_dir / exe.name


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", type=Path, default=REPO_ROOT / "build/windows",
                        help="CMake build directory (default: %(default)s)")
    parser.add_argument("--sdk", type=Path, default=SDK_DEFAULT,
                        help="MSYS2 SDK prefix (default: %(default)s)")
    parser.add_argument("--wine", default="wine", help="Wine executable (default: %(default)s)")
    parser.add_argument("--wineprefix", type=Path, default=WINEPREFIX_DEFAULT,
                        help="Wine prefix (default: %(default)s)")
    parser.add_argument("--skip-deploy", action="store_true",
                        help="run the existing build/windows/deploy tree")
    parser.add_argument("--deploy-only", action="store_true", help="stop after deploying")
    parser.add_argument("--debug", action="store_true", help="keep Wine's debug output")
    parser.add_argument("app_args", nargs=argparse.REMAINDER, help="arguments for enquber.exe")
    args = parser.parse_args()

    env = wine_env(args)
    if args.skip_deploy:
        deployed_exe = args.build_dir / "deploy" / "enquber.exe"
        if not deployed_exe.is_file():
            sys.exit(f"error: {deployed_exe} not found; run without --skip-deploy first")
    else:
        exe = args.build_dir / "enquber.exe"
        if not exe.is_file():
            sys.exit(f"error: {exe} not found; build it with 'just build-windows' first")
        args.wineprefix.mkdir(parents=True, exist_ok=True)
        deployed_exe = deploy(args, exe, env)

    if args.deploy_only:
        print(f"Deployed to {deployed_exe.parent}")
        return

    print(f"Running: {args.wine} {deployed_exe} {', '.join(args.app_args)}".rstrip(), flush=True)
    result = subprocess.run([args.wine, str(deployed_exe), *args.app_args],
                            cwd=deployed_exe.parent, env=env, check=False)
    sys.exit(result.returncode)


if __name__ == "__main__":
    main()
