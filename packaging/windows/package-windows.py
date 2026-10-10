#!/usr/bin/env python3
"""Package the deployed Windows build for distribution.

Packs the tree produced by packaging/windows/wine-run.py --deploy-only into a
zip whose top-level folder is named after the project version, for example
enquber-0.1.1-win64.zip containing enquber-0.1.1-win64/enquber.exe with the Qt
runtime beside it.  That folder is what you send to a Windows user: the
executable only runs when the library and plugin folders next to it are
included.

With --installer an NSIS setup executable is built as well, from
packaging/windows/enquber.nsi.  It installs the same tree per user below
%LOCALAPPDATA%\\Programs\\Enquber, creates Start menu shortcuts and an entry
in Settings > Apps; makensis must be on PATH (or passed with --makensis).

With --msix an MSIX package is built for the Microsoft Store from
packaging/windows/AppxManifest.xml.in and the tile logos in data/icon/msix.
It describes a full-trust packaged desktop application; the Store re-signs
MSIX packages after certification, so the package needs no certificate of its
own (sideloading it does).  makemsix from Microsoft's MSIX SDK is needed (or
pass --makemsix).

The version comes from the build tree's CMakeCache.txt.

Examples:

    just package-windows
    just installer-windows
    just msix-windows
    packaging/windows/wine-run.py --deploy-only
    packaging/windows/package-windows.py --installer --msix

Exit codes: 0 on success, 1 when the deploy tree is missing or incomplete.
"""

import argparse
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path
from xml.sax.saxutils import escape

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BUILD_DIR = REPO_ROOT / "build/windows"
NSI_SCRIPT = REPO_ROOT / "packaging" / "windows" / "enquber.nsi"
LICENSE_FILE = REPO_ROOT / "COPYING"
ICON_FILE = REPO_ROOT / "data" / "icon" / "enquber.ico"
WELCOME_BITMAP = REPO_ROOT / "data" / "icon" / "enquber-welcome.bmp"
HEADER_BITMAP = REPO_ROOT / "data" / "icon" / "enquber-header.bmp"
MSIX_MANIFEST = REPO_ROOT / "packaging" / "windows" / "AppxManifest.xml.in"
MSIX_ASSET_DIR = REPO_ROOT / "data" / "icon" / "msix"
MSIX_ASSET_NAMES = (
    "StoreLogo.png",
    "Square44x44Logo.png",
    "Square150x150Logo.png",
    "Square310x310Logo.png",
    "Wide310x150Logo.png",
)


def project_version(build_dir: Path) -> str:
    """Return CMAKE_PROJECT_VERSION from the build tree's cache."""
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        sys.exit(f"error: {cache} not found; configure the windows preset first")
    match = re.search(r"^CMAKE_PROJECT_VERSION:[^=]*=(\S+)$",
                      cache.read_text(encoding="utf-8"), re.MULTILINE)
    if not match:
        sys.exit(f"error: CMAKE_PROJECT_VERSION not found in {cache}")
    return match.group(1)


def version_quad(version: str) -> str:
    """The four-part numeric version NSIS wants for VIProductVersion."""
    parts = [part for part in re.split(r"[^0-9]+", version) if part]
    return ".".join([*parts, "0", "0", "0", "0"][:4])


def xml_attr(value: str) -> str:
    """Escape a string for use inside a double-quoted XML attribute."""
    return escape(value, {'"': "&quot;"})


def render_manifest(template: str, identity: str, publisher: str, version: str) -> str:
    """Fill the @PLACEHOLDERS@ of an MSIX manifest template.

    The identity and publisher come from the command line and land in XML
    attributes, so they are escaped; the version is numeric and the publisher
    display name is a constant.
    """
    values = {
        "@MSIX_IDENTITY@": xml_attr(identity),
        "@MSIX_PUBLISHER@": xml_attr(publisher),
        "@MSIX_PUBLISHER_NAME@": "Enquber",
        "@APP_VERSION_QUAD@": version_quad(version),
    }
    for token, value in values.items():
        template = template.replace(token, value)
    return template


def write_zip(deploy: Path, output: Path, root: str) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    files = sorted(path for path in deploy.rglob("*") if path.is_file())
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in files:
            archive.write(path, f"{root}/{path.relative_to(deploy).as_posix()}")
    print(f"wrote {output} ({output.stat().st_size / 1e6:.1f} MB, {len(files)} files)")


def write_installer(deploy: Path, output: Path, version: str, makensis: str) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            makensis,
            f"-DAPP_VERSION={version}",
            f"-DAPP_VERSION_QUAD={version_quad(version)}",
            f"-DSOURCE_DIR={deploy}",
            f"-DOUTPUT_FILE={output}",
            f"-DLICENSE_FILE={LICENSE_FILE}",
            f"-DICON_FILE={ICON_FILE}",
            f"-DWELCOME_BITMAP={WELCOME_BITMAP}",
            f"-DHEADER_BITMAP={HEADER_BITMAP}",
            str(NSI_SCRIPT),
        ],
        check=True,
    )
    print(f"wrote {output} ({output.stat().st_size / 1e6:.1f} MB)")


def write_msix(deploy: Path, output: Path, version: str, identity: str,
               publisher: str, makemsix: str) -> None:
    missing = [name for name in MSIX_ASSET_NAMES if not (MSIX_ASSET_DIR / name).is_file()]
    if missing:
        sys.exit(f"error: {MSIX_ASSET_DIR / missing[0]} not found; regenerate the "
                 "icons with tools/generate-icon.py")

    stage = output.parent / "msix"
    shutil.rmtree(stage, ignore_errors=True)
    shutil.copytree(deploy, stage)
    assets = stage / "Assets"
    assets.mkdir()
    for name in MSIX_ASSET_NAMES:
        shutil.copy2(MSIX_ASSET_DIR / name, assets / name)

    manifest = render_manifest(MSIX_MANIFEST.read_text(encoding="utf-8"),
                               identity, publisher, version)
    (stage / "AppxManifest.xml").write_text(manifest, encoding="utf-8")

    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([makemsix, "pack", "-d", str(stage), "-p", str(output)], check=True)
    print(f"wrote {output} ({output.stat().st_size / 1e6:.1f} MB)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR,
                        help="CMake build directory (default: %(default)s)")
    parser.add_argument("--output", type=Path,
                        help="zip to write (default: <build-dir>/enquber-<version>-win64.zip)")
    parser.add_argument("--installer", action="store_true",
                        help="also build the NSIS setup executable")
    parser.add_argument("--makensis",
                        help="makensis to run (default: the first one on PATH)")
    parser.add_argument("--msix", action="store_true",
                        help="also build an MSIX package for the Microsoft Store")
    parser.add_argument("--makemsix",
                        help="makemsix to run (default: the first one on PATH)")
    parser.add_argument("--msix-identity", default="Enquber", metavar="NAME",
                        help="MSIX Identity Name (default: %(default)s)")
    parser.add_argument("--msix-publisher", default="CN=Enquber", metavar="DN",
                        help="MSIX Identity Publisher (default: %(default)s)")
    args = parser.parse_args()

    deploy = args.build_dir / "deploy"
    required = [deploy / "enquber.exe", deploy / "platforms" / "qwindows.dll"]
    missing = [path for path in required if not path.is_file()]
    if missing:
        sys.exit(f"error: {missing[0]} not found; deploy first with "
                 "packaging/windows/wine-run.py --deploy-only")

    version = project_version(args.build_dir)
    root = f"enquber-{version}-win64"
    write_zip(deploy, args.output or args.build_dir / f"{root}.zip", root)

    if args.installer:
        makensis = args.makensis or shutil.which("makensis")
        if not makensis:
            sys.exit("error: makensis not found; install NSIS (the AUR 'nsis' package "
                     "on Arch) or pass --makensis")
        write_installer(deploy, args.build_dir / f"{root}-setup.exe", version, makensis)

    if args.msix:
        makemsix = args.makemsix or shutil.which("makemsix")
        if not makemsix:
            sys.exit("error: makemsix not found; build Microsoft's MSIX SDK (its "
                     "makelinux.sh) or pass --makemsix")
        write_msix(deploy, args.build_dir / f"{root}.msix", version,
                   args.msix_identity, args.msix_publisher, makemsix)
    return 0


if __name__ == "__main__":
    sys.exit(main())
