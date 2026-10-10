#!/usr/bin/env python3
"""Fetch the MinGW-w64 SDK used by the Windows cross build.

The cross build needs Qt 6 and libqrencode built for Windows, which the AUR
only offers as very large source packages.  This tool instead resolves the
runtime dependencies of the desired packages from the MSYS2 repository
database, downloads the prebuilt pacman packages into a cache and extracts
them into a prefix tree (they install into "mingw64" below that prefix, so a
typical prefix looks like ~/.local/msys2-mingw64/mingw64/...).

The toolchain file (cmake/toolchain-mingw-w64.cmake) points at the same
prefix by default, so after a run the usual cross build just works.

Examples:

    packaging/windows/mingw-sdk.py --list          # show what would be installed
    packaging/windows/mingw-sdk.py                 # download and extract
    packaging/windows/mingw-sdk.py --refresh-db    # re-download the package database

Exit codes: 0 on success, 1 on a download or checksum error.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

MIRROR = "https://mirror.msys2.org/mingw/mingw64"
DEFAULT_PACKAGES = ["mingw-w64-x86_64-qt6-base", "mingw-w64-x86_64-qrencode"]
DEFAULT_PREFIX = Path(os.environ.get("ENQUBER_MINGW_PREFIX") or Path.home() / ".local/msys2-mingw64")

# Build-time and optional-runtime only packages.  A shared Qt links its
# dependencies into the Qt DLLs, so the CMake configs neither need these
# packages nor does windeployqt ship them; pruning keeps the download and the
# extracted tree small.  Pass --keep-deps to take the full closure instead.
PRUNE = {
    "mingw-w64-x86_64-dbus",  # only the QtDBus module
    "mingw-w64-x86_64-ncurses",
    "mingw-w64-x86_64-openssl",  # only QtNetwork's TLS backends
    "mingw-w64-x86_64-python",
    "mingw-w64-x86_64-python-packaging",
    "mingw-w64-x86_64-sqlite3",  # only the QtSql drivers
    "mingw-w64-x86_64-tcl",
    "mingw-w64-x86_64-tk",
    "mingw-w64-x86_64-vulkan-loader",  # optional QtGui backend
    "mingw-w64-x86_64-wineditline",
}


def parse_desc(path):
    """Parse a pacman-style desc file into a field -> value dict."""
    fields, current = {}, None
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("%") and line.endswith("%"):
            current = line.strip("%")
            fields[current] = []
        elif current is not None:
            fields[current].append(line)
    return {key: "\n".join(value).strip() for key, value in fields.items()}


def load_db(cache_dir, refresh):
    """Return (packages, provides) from the cached or freshly fetched database."""
    db_dir = cache_dir / "db"
    if refresh or not (db_dir.exists() and any(db_dir.iterdir())):
        archive = download(f"{MIRROR}/mingw64.db", cache_dir / "mingw64.db", force=refresh)
        if db_dir.exists():
            shutil.rmtree(db_dir)
        db_dir.mkdir(parents=True)
        # The database is a zstd-compressed tar archive; bsdtar reads zstd.
        subprocess.run(["bsdtar", "-xf", str(archive), "-C", str(db_dir)], check=True)

    packages, provides = {}, {}
    for entry in sorted(db_dir.iterdir()):
        desc = entry / "desc"
        if not desc.is_file():
            continue
        fields = parse_desc(desc)
        name = fields.get("NAME")
        if not name:
            continue
        packages[name] = {
            "version": fields.get("VERSION", ""),
            "filename": fields.get("FILENAME", ""),
            "sha256": fields.get("SHA256SUM", ""),
            "size": int(fields.get("CSIZE", "0")),
            "depends": fields.get("DEPENDS", "").splitlines(),
        }
        for provide in fields.get("PROVIDES", "").splitlines():
            provides.setdefault(provide.split("=")[0], name)
    return packages, provides


def resolve(packages, provides, roots, pruned):
    """Resolve the transitive dependency closure, skipping pruned packages."""
    todo, seen = list(roots), set()
    while todo:
        name = todo.pop()
        if name in seen:
            continue
        if name not in packages:
            resolved = provides.get(name)
            if resolved is None:
                sys.exit(f"error: cannot resolve dependency {name!r}")
            name = resolved
        seen.add(name)
        for dep in packages[name]["depends"]:
            match = re.match(r"^([^\s<>=]+)", dep)
            if match and match.group(1) not in pruned:
                todo.append(match.group(1))
    return sorted(seen)


def sha256_of(path):
    """Return the hex SHA-256 of a file, read in chunks."""
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download(url, dest, *, expected_sha256=None, force=False, retries=3):
    """Download url to dest and return dest.

    A cached file is reused when it is non-empty, unless force is set or its
    checksum does not match expected_sha256; a cached file that fails the
    checksum is removed and fetched again.
    """
    if not force and dest.exists() and dest.stat().st_size > 0:
        if expected_sha256 is None or sha256_of(dest) == expected_sha256:
            return dest
        dest.unlink()
    dest.parent.mkdir(parents=True, exist_ok=True)
    partial = dest.with_suffix(dest.suffix + ".part")
    for attempt in range(1, retries + 1):
        try:
            with urllib.request.urlopen(url, timeout=120) as response, open(partial, "wb") as out:  # noqa: S310
                shutil.copyfileobj(response, out, length=1 << 20)
            partial.rename(dest)
        except OSError as exc:
            if attempt == retries:
                raise
            print(f"retry {attempt}/{retries}: {exc}", file=sys.stderr, flush=True)
            time.sleep(2)
        else:
            return dest
    raise OSError(f"could not download {url}")


def fetch_package(package, cache_dir):
    """Download one package into the cache and verify its checksum.

    A cached copy is reused only when it matches; a mismatch is removed so the
    next run can fetch a good copy instead of failing on the same file.
    """
    dest = cache_dir / "packages" / package["filename"]
    path = download(f"{MIRROR}/{package['filename']}", dest, expected_sha256=package["sha256"])
    if sha256_of(path) != package["sha256"]:
        path.unlink(missing_ok=True)
        raise ValueError(f"checksum mismatch for {package['filename']}")
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--prefix", type=Path, default=DEFAULT_PREFIX,
                        help="installation prefix (default: %(default)s)")
    parser.add_argument("--cache", type=Path, default=DEFAULT_PREFIX.parent / "msys2-mingw64-cache",
                        help="download cache (default: %(default)s)")
    parser.add_argument("--packages", nargs="+", default=DEFAULT_PACKAGES,
                        help="root packages to install (default: %(default)s)")
    parser.add_argument("--refresh-db", action="store_true", help="re-download the package database")
    parser.add_argument("--keep-deps", action="store_true", help="install the full dependency closure")
    parser.add_argument("--list", action="store_true", help="list the packages instead of installing")
    args = parser.parse_args()

    args.cache.mkdir(parents=True, exist_ok=True)
    packages, provides = load_db(args.cache, args.refresh_db)
    closure = resolve(packages, provides, args.packages, set() if args.keep_deps else PRUNE)

    if args.list:
        for name in closure:
            print(f"{packages[name]['size'] / 1e6:8.1f} MB  {name} {packages[name]['version']}")
        total = sum(packages[name]["size"] for name in closure)
        print(f"\n{len(closure)} packages, {total / 1e6:.0f} MB download")
        return

    print(f"Downloading {len(closure)} packages "
          f"({sum(packages[name]['size'] for name in closure) / 1e6:.0f} MB) ...", flush=True)

    def fetch(name):
        return name, fetch_package(packages[name], args.cache)

    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        try:
            files = dict(pool.map(fetch, closure))
        except ValueError as exc:
            sys.exit(f"error: {exc} (the cached file was removed; re-run to retry)")

    print(f"Extracting to {args.prefix} ...", flush=True)
    args.prefix.mkdir(parents=True, exist_ok=True)
    for name in closure:
        subprocess.run(
            ["bsdtar", "-xf", str(files[name]), "-C", str(args.prefix),
             "--no-same-owner", "--no-same-permissions"],
            check=True,
        )

    manifest = {name: packages[name]["version"] for name in closure}
    (args.prefix / ".msys2-sdk.json").write_text(json.dumps({"packages": manifest}, indent=1),
                                                 encoding="utf-8")
    print(f"Done: {len(closure)} packages in {args.prefix / 'mingw64'}")


if __name__ == "__main__":
    main()
