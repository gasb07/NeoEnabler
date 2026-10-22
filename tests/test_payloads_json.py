"""payloads.json for PS5 Payload Manager; no console or network access."""
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("payloads_json", Path(__file__).resolve().parents[1] / "tools/payloads_json.py")
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)


def base_name(filename):
    """Payload Manager's name without version: an installed file with the same one is offered an update."""
    t = re.sub(r"\.(elf|bin)$", "", filename, flags=re.I)
    m = re.search(r"[_-]v?(\d+[\d.a-z-]+)", t, re.I)
    if m:
        t = t.replace(m.group(0), "")
    return re.sub(r"[_-]ps[45]$", "", t, flags=re.I)


class PayloadsJsonTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.dir = Path(self.temp.name)
        (self.dir / "manager.c").write_text('#define VERSION "NeoEnabler 0.4.2"\n', encoding="utf-8")
        (self.dir / "neo-manager.elf").write_bytes(b"\x7fELF test")

    def tearDown(self):
        self.temp.cleanup()

    def run_tool(self, *extra):
        out = self.dir / "payloads.json"
        tool.main(["--repo", "gasb07/NeoEnabler", "--manager", str(self.dir / "manager.c"),
                   "--elf", str(self.dir / "neo-manager.elf"), "--date", "2026-10-23", "--out", str(out), *extra])
        return json.loads(out.read_text(encoding="utf-8"))

    def test_entry(self):
        data = self.run_tool()
        self.assertEqual(len(data), 1)
        e = data[0]
        self.assertEqual(e["version"], "v0.4.2")
        self.assertEqual(e["filename"], "neo-manager_v0.4.2.elf")
        self.assertEqual(e["url"], "https://github.com/gasb07/NeoEnabler/releases/download/v0.4.2/neo-manager.elf")
        self.assertEqual(e["checksum"], hashlib.sha256(b"\x7fELF test").hexdigest())
        self.assertEqual(e["last_update"], "2026-10-23")
        for key in ("name", "filename", "url", "source", "description", "version", "category"):
            self.assertTrue(e[key])

    def test_update_matches_previous_release(self):
        filename = self.run_tool()[0]["filename"]
        self.assertEqual(base_name(filename), base_name("neo-manager_v0.4.1.elf"))
        self.assertNotEqual(filename, "neo-manager_v0.4.1.elf")

    def test_missing_version(self):
        (self.dir / "manager.c").write_text("int main;\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            self.run_tool()

    def test_bad_date(self):
        with self.assertRaises(SystemExit):
            tool.main(["--repo", "a/b", "--date", "23-10-2026"])


if __name__ == "__main__":
    unittest.main()
