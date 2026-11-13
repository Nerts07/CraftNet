#!/usr/bin/env python3
"""Extract resources from a local Java Edition client JAR."""
import argparse
from pathlib import Path, PurePosixPath
import shutil
import zipfile

MAX_ENTRY = 16 * 1024 * 1024
MAX_TOTAL = 512 * 1024 * 1024


def selected(name):
    parts = PurePosixPath(name).parts
    if len(parts) < 4 or parts[0] != "assets":
        return False
    return ((parts[2] == "textures" and (name.endswith(".png") or name.endswith(".png.mcmeta")))
            or (parts[2] in ("models", "blockstates") and name.endswith(".json")))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("client_jar", type=Path)
    parser.add_argument("--output", type=Path, default=Path("assets/imported"))
    args = parser.parse_args()
    root = args.output.resolve()
    total = 0
    count = 0
    with zipfile.ZipFile(args.client_jar) as archive:
        entries = []
        for entry in archive.infolist():
            name = entry.filename
            path = PurePosixPath(name)
            if entry.is_dir() or not selected(name):
                continue
            if (".." in path.parts or "\\" in name or
                    any(":" in part for part in path.parts) or entry.file_size > MAX_ENTRY):
                raise ValueError(f"Unsafe or oversized archive entry: {name}")
            total += entry.file_size
            if total > MAX_TOTAL:
                raise ValueError("Selected resources exceed the extraction limit")
            entries.append(entry)
        for entry in entries:
            destination = root.joinpath(*PurePosixPath(entry.filename).parts)
            if not destination.resolve().is_relative_to(root):
                raise ValueError(f"Output escapes destination: {destination}")
            destination.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(entry) as source, destination.open("wb") as target:
                shutil.copyfileobj(source, target, length=64 * 1024)
            count += 1
    print(f"Extracted {count} resource files ({total} bytes) to {root}")


if __name__ == "__main__":
    main()
