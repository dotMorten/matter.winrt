#!/usr/bin/env python3

import argparse
import pathlib
import subprocess


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--midlrt", required=True)
    parser.add_argument("--cppwinrt", required=True)
    parser.add_argument("--sdk-metadata", required=True)
    parser.add_argument("--idl", required=True)
    parser.add_argument("--winmd", required=True)
    parser.add_argument("--generated", required=True)
    args = parser.parse_args()

    winmd = pathlib.Path(args.winmd).resolve()
    generated = pathlib.Path(args.generated).resolve()
    midlrt = pathlib.Path(args.midlrt).resolve()
    cppwinrt = pathlib.Path(args.cppwinrt).resolve()
    sdk_metadata = pathlib.Path(args.sdk_metadata).resolve()
    winmd.parent.mkdir(parents=True, exist_ok=True)
    generated.mkdir(parents=True, exist_ok=True)

    subprocess.run(
        [
            str(midlrt),
            "/winrt",
            "/nomidl",
            "/metadata_dir",
            str(sdk_metadata),
            "/out",
            str(winmd.parent),
            "/winmd",
            winmd.name,
            str(pathlib.Path(args.idl).resolve()),
        ],
        check=True,
    )
    subprocess.run(
        [
            str(cppwinrt),
            "-input",
            str(winmd),
            "-reference",
            "sdk",
            "-output",
            str(generated),
            "-component",
            str(generated / "stubs"),
            "-name",
            "Matter.Windows.Controller",
            "-pch",
            ".",
        ],
        check=True,
    )


if __name__ == "__main__":
    main()
