#!/usr/bin/env python3
import argparse, gzip, os, shutil
from pathlib import Path

DEFAULT_EXTS = {".html", ".htm", ".css", ".js", ".svg", ".txt", ".json", ".ico", ".woff", ".woff2"}

def gz_file(src: Path, dst: Path):
    with open(src, "rb") as fin, gzip.GzipFile(filename=src.name, mode="wb", fileobj=open(dst, "wb"), compresslevel=9, mtime=0) as gz:
        shutil.copyfileobj(fin, gz)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input_dir", help="Directory containing web assets")
    ap.add_argument("--keep-originals", action="store_true", help="Keep uncompressed alongside .gz")
    ap.add_argument("--exts", nargs="*", default=list(DEFAULT_EXTS), help="Extensions to gzip")
    args = ap.parse_args()

    root = Path(args.input_dir).resolve()
    for path in root.rglob("*"):
        if path.is_file() and path.suffix.lower() in set(args.exts):
            gz_path = path.with_suffix(path.suffix + ".gz")
            gz_file(path, gz_path)
            if not args.keep_originals:
                # Replace file with gz version by renaming
                path.unlink()
                gz_path.rename(path)  # final name is original (no .gz) for simplest serving
    print("Compression done.")

if __name__ == "__main__":
    main()
