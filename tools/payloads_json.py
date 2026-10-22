#!/usr/bin/env python3
"""Write the payloads.json source that PS5 Payload Manager (pldmgr) reads, for one release.

A release attaches it next to neo-manager.elf, so the source URL
https://github.com/<repo>/releases/latest/download/payloads.json always lists the latest release. The file name
carries the version: Payload Manager offers an update when an installed file has the same name without its
version (neo-manager_v0.4.0.elf -> neo-manager_v0.4.1.elf). Uses the Python standard library only.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

DESCRIPTION = ("Starts PS4 games in PS4 Pro mode with the NeoEnabler game patches. "
               "Load ps-patch-system first, then NeoEnabler.")


def version_of(manager_c):
    m = re.search(r'^#define VERSION "NeoEnabler ([^"]+)"$', Path(manager_c).read_text(encoding="utf-8"), re.M)
    if not m:
        raise ValueError(f"no VERSION in {manager_c}")
    return m.group(1)


def entry(repo, version, elf, date):
    tag = f"v{version}"
    url = f"https://github.com/{repo}/releases/download/{tag}/neo-manager.elf"
    return {
        "name": "NeoEnabler",
        "filename": f"neo-manager_{tag}.elf",
        "url": url,
        "source": f"https://github.com/{repo}/releases",
        "source_direct": url,
        "description": DESCRIPTION,
        "last_update": date,
        "version": tag,
        "category": "Utilities & Tools",
        "checksum": hashlib.sha256(Path(elf).read_bytes()).hexdigest(),
    }


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--repo", required=True, help="owner/name of the GitHub repository")
    p.add_argument("--manager", default="src/manager.c", help="source file with the VERSION define")
    p.add_argument("--elf", default="build/neo-manager.elf")
    p.add_argument("--date", required=True, help="release date, YYYY-MM-DD")
    p.add_argument("--out", default="build/payloads.json")
    a = p.parse_args(argv)
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", a.date):
        p.error("--date must be YYYY-MM-DD")
    data = [entry(a.repo, version_of(a.manager), a.elf, a.date)]
    Path(a.out).write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
