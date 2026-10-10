"""Tests for mingw-sdk.py.  Run them with `just test`."""
import hashlib
import importlib.util
import tempfile
import unittest
from pathlib import Path
from unittest import mock


def _load_script(filename):
    """Import a helper script whose hyphenated name is not a module name."""
    path = Path(__file__).resolve().with_name(filename)
    spec = importlib.util.spec_from_file_location(path.stem.replace("-", "_"), path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


MINGW = _load_script("mingw-sdk.py")


class DownloadTests(unittest.TestCase):
    def test_reuses_a_cached_file_without_fetching(self):
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "pkg.bin"
            dest.write_bytes(b"cached")
            # file:///nonexistent would raise if download() actually fetched it.
            self.assertEqual(MINGW.download("file:///nonexistent/enquber", dest), dest)
            self.assertEqual(dest.read_bytes(), b"cached")

    def test_force_refetches_over_the_cache(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "source.bin"
            source.write_bytes(b"fresh")
            dest = root / "pkg.bin"
            dest.write_bytes(b"stale")
            MINGW.download(source.as_uri(), dest, force=True)
            self.assertEqual(dest.read_bytes(), b"fresh")

    def test_replaces_a_cached_file_with_the_wrong_checksum(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "source.bin"
            source.write_bytes(b"good")
            dest = root / "pkg.bin"
            dest.write_bytes(b"corrupt")
            digest = hashlib.sha256(b"good").hexdigest()
            MINGW.download(source.as_uri(), dest, expected_sha256=digest)
            self.assertEqual(dest.read_bytes(), b"good")


class FetchPackageTests(unittest.TestCase):
    def _package(self, sha256):
        return {"filename": "pkg.tar.zst", "sha256": sha256}

    def test_replaces_a_corrupt_cached_package(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "source"
            source.mkdir()
            (source / "pkg.tar.zst").write_bytes(b"right")
            cache = root / "cache"
            (cache / "packages").mkdir(parents=True)
            (cache / "packages" / "pkg.tar.zst").write_bytes(b"corrupt")
            with mock.patch.object(MINGW, "MIRROR", source.as_uri()):
                path = MINGW.fetch_package(
                    self._package(hashlib.sha256(b"right").hexdigest()), cache)
            self.assertEqual(path.read_bytes(), b"right")

    def test_removes_a_mismatching_download_and_raises(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "source"
            source.mkdir()
            (source / "pkg.tar.zst").write_bytes(b"wrong")
            cache = root / "cache"
            with mock.patch.object(MINGW, "MIRROR", source.as_uri()), \
                    self.assertRaises(ValueError):
                MINGW.fetch_package(
                    self._package(hashlib.sha256(b"right").hexdigest()), cache)
            self.assertFalse((cache / "packages" / "pkg.tar.zst").exists())


class LoadDbTests(unittest.TestCase):
    def _cache_with_db(self, tmp):
        cache = Path(tmp)
        (cache / "db").mkdir()
        (cache / "db" / "stale").write_text("x", encoding="utf-8")
        (cache / "mingw64.db").write_bytes(b"cached")
        return cache

    def test_refresh_forces_a_download(self):
        with tempfile.TemporaryDirectory() as tmp:
            cache = self._cache_with_db(tmp)
            with mock.patch.object(MINGW, "download",
                                   return_value=cache / "mingw64.db") as download, \
                    mock.patch.object(MINGW.subprocess, "run"):
                MINGW.load_db(cache, refresh=True)
            self.assertTrue(download.call_args.kwargs.get("force"))

    def test_cached_db_is_not_downloaded_again(self):
        with tempfile.TemporaryDirectory() as tmp:
            cache = self._cache_with_db(tmp)
            with mock.patch.object(MINGW, "download") as download, \
                    mock.patch.object(MINGW.subprocess, "run"):
                MINGW.load_db(cache, refresh=False)
            download.assert_not_called()


class ResolveTests(unittest.TestCase):
    def test_names_the_unresolved_dependency(self):
        with self.assertRaises(SystemExit) as caught:
            MINGW.resolve({}, {}, ["libmissing"], set())
        self.assertIn("libmissing", str(caught.exception))

    def test_follows_a_provides_alias(self):
        packages = {"real": {"depends": []}}
        self.assertEqual(MINGW.resolve(packages, {"libvirtual": "real"},
                                       ["libvirtual"], set()), ["real"])


if __name__ == "__main__":
    unittest.main()
