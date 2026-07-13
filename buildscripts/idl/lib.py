# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0
"""Library functions and utility methods used across user-facing IDL scripts."""

import os
import shutil
import subprocess

from buildscripts.idl.idl import parser, syntax
from buildscripts.idl.idl.compiler import CompilerImportResolver


def list_idls(directory: str) -> set[str]:
    """Find all IDL files in directory, using git ls-files when inside a git repo."""
    if shutil.which("git") is not None:
        result = subprocess.run(
            [
                "git",
                "ls-files",
                "--cached",
                "--others",
                "--exclude-standard",
                "--",
                ":(glob)**/*.idl",
            ],
            capture_output=True,
            text=True,
            cwd=directory,
        )
        if result.returncode == 0:
            idls = {os.path.join(directory, p) for p in result.stdout.splitlines()}
            # git ls-files can report files that have been removed from the working tree. Omit those.
            return {idl for idl in idls if os.path.isfile(idl)}
    return {
        os.path.join(dirpath, filename)
        for dirpath, _, filenames in os.walk(directory)
        for filename in filenames
        if not filename.startswith(".") and filename.endswith(".idl")
    }


def parse_idl(idl_path: str, import_directories: list[str]) -> syntax.IDLParsedSpec:
    """Parse an IDL file or throw an error."""
    parsed_doc = parser.parse(open(idl_path), idl_path, CompilerImportResolver(import_directories))

    if parsed_doc.errors:
        parsed_doc.errors.dump_errors()
        raise ValueError(f"Cannot parse {idl_path}")

    return parsed_doc


def is_third_party_idl(idl_path: str) -> bool:
    """Check if an IDL file is under a third party directory."""
    third_party_idl_subpaths = [os.path.join("third_party", "mozjs"), "win32com"]

    for file_name in third_party_idl_subpaths:
        if file_name in idl_path:
            return True

    return False


def get_all_feature_flags(idl_dirs: list[str] = None):
    """Generate a dict of all feature flags with their default value."""
    default_idl_dirs = ["src", "buildscripts"]

    if not idl_dirs:
        idl_dirs = default_idl_dirs

    all_flags = {}
    for idl_dir in idl_dirs:
        for idl_path in sorted(list_idls(idl_dir)):
            if is_third_party_idl(idl_path):
                continue
            # Most IDL files do not contain feature flags.
            # We can discard these quickly without expensive YAML parsing.
            with open(idl_path) as idl_file:
                if "feature_flags" not in idl_file.read():
                    continue
            with open(idl_path) as idl_file:
                doc = parser.parse_file(idl_file, idl_path)
            for feature_flag in doc.spec.feature_flags:
                all_flags[feature_flag.name] = feature_flag

    return all_flags
