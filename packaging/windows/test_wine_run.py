"""Tests for wine-run.py.  Run them with `just test`."""
import importlib.util
import tempfile
import unittest
from pathlib import Path


def _load_script(filename):
    """Import a helper script whose hyphenated name is not a module name."""
    path = Path(__file__).resolve().with_name(filename)
    spec = importlib.util.spec_from_file_location(path.stem.replace("-", "_"), path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


WINE_RUN = _load_script("wine-run.py")


class SystemDllTests(unittest.TestCase):
    def test_known_windows_dlls(self):
        for name in ("KERNEL32.dll", "user32.dll", "authz.dll", "ucrtbase.dll"):
            self.assertTrue(WINE_RUN.is_windows_system_dll(name), name)

    def test_api_set_prefixes(self):
        for name in ("api-ms-win-core-file-l1-1-0.dll",
                     "ext-ms-win-gdi-draw-l1-1-0.dll"):
            self.assertTrue(WINE_RUN.is_windows_system_dll(name), name)

    def test_mingw_and_qt_dlls_are_not_system_dlls(self):
        for name in ("libqrencode.dll", "libstdc++-6.dll", "Qt6Core.dll"):
            self.assertFalse(WINE_RUN.is_windows_system_dll(name), name)


class CollectDllTests(unittest.TestCase):
    def _collect(self, target, imports, index):
        return WINE_RUN.collect_dlls(
            target, [target / "app.exe"], index,
            dependencies=lambda path: imports.get(path.name, []))

    def test_reports_an_unresolved_import(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp)
            (target / "app.exe").write_bytes(b"")
            copied, missing = self._collect(target, {"app.exe": ["libmissing.dll"]}, {})
            self.assertEqual(copied, [])
            self.assertEqual(missing, ["libmissing.dll"])

    def test_ignores_windows_system_dlls(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp)
            (target / "app.exe").write_bytes(b"")
            imports = {"app.exe": ["KERNEL32.dll", "api-ms-win-core-file-l1-1-0.dll"]}
            copied, missing = self._collect(target, imports, {})
            self.assertEqual(copied, [])
            self.assertEqual(missing, [])

    def test_copies_an_indexed_dll_and_follows_it(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / "deploy"
            target.mkdir()
            (target / "app.exe").write_bytes(b"")
            source = Path(tmp) / "libextra.dll"
            source.write_bytes(b"dll")
            imports = {"app.exe": ["libextra.dll"], "libextra.dll": ["libleaf.dll"]}
            copied, missing = self._collect(target, imports, {"libextra.dll": source})
            self.assertEqual(copied, ["libextra.dll"])
            self.assertEqual(missing, ["libleaf.dll"])
            self.assertTrue((target / "libextra.dll").is_file())


if __name__ == "__main__":
    unittest.main()
