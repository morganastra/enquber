"""Tests for package-windows.py.  Run them with `just test`."""
import importlib.util
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path


def _load_script(filename):
    """Import a helper script whose hyphenated name is not a module name."""
    path = Path(__file__).resolve().with_name(filename)
    spec = importlib.util.spec_from_file_location(path.stem.replace("-", "_"), path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


PACKAGE = _load_script("package-windows.py")
TEMPLATE = Path(__file__).resolve().with_name("AppxManifest.xml.in")
APPX = "{http://schemas.microsoft.com/appx/manifest/foundation/windows10}"


class VersionQuadTests(unittest.TestCase):
    def test_pads_to_four_numeric_parts(self):
        self.assertEqual(PACKAGE.version_quad("0.1.1"), "0.1.1.0")

    def test_drops_non_numeric_parts(self):
        self.assertEqual(PACKAGE.version_quad("1.2.3-rc4"), "1.2.3.4")


class XmlAttrTests(unittest.TestCase):
    def test_escapes_attribute_specials(self):
        self.assertEqual(PACKAGE.xml_attr('A & B "C" <D>'),
                         "A &amp; B &quot;C&quot; &lt;D&gt;")

    def test_leaves_an_ordinary_value_alone(self):
        self.assertEqual(PACKAGE.xml_attr("CN=Enquber"), "CN=Enquber")


class RenderManifestTests(unittest.TestCase):
    def test_template_keeps_the_placeholders(self):
        text = TEMPLATE.read_text(encoding="utf-8")
        for token in ("@MSIX_IDENTITY@", "@MSIX_PUBLISHER@", "@APP_VERSION_QUAD@"):
            self.assertIn(token, text)

    def test_special_characters_stay_well_formed_and_verbatim(self):
        text = PACKAGE.render_manifest(
            TEMPLATE.read_text(encoding="utf-8"), 'Weird"Name', "CN=A & B Co", "0.1.1")
        identity = ET.fromstring(text).find(f"{APPX}Identity")
        self.assertIsNotNone(identity)
        self.assertEqual(identity.get("Name"), 'Weird"Name')
        self.assertEqual(identity.get("Publisher"), "CN=A & B Co")
        self.assertEqual(identity.get("Version"), "0.1.1.0")


if __name__ == "__main__":
    unittest.main()
