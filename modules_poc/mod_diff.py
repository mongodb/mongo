#!/usr/bin/env python3

import os
import subprocess
import sys
from typing import Iterable, NoReturn

import browse
from browse import Decl


def perr_exit(message: str) -> NoReturn:
    print(message, file=sys.stderr)
    os._exit(1)  # Exit immediately without cleanup


visibility_rank = {
    v: i
    for i, v in enumerate(
        [
            # Roughly ordered from most to least public
            "UNKNOWN",
            "open",
            "public",
            "use_replacement",
            "needs_replacement",
            "private",
            "file_private",
        ]
    )
}


def sorted_decls(decls: Iterable[Decl]):
    # NOTE: d.loc correctly compares line and column as numbers.
    return sorted(decls, key=lambda d: (visibility_rank[d.visibility], d.loc))


def print_decl_recursive(decl: Decl, indent_level=0, parent: Decl = None):
    """Prints a declaration and its semantic children recursively."""
    indent = " " * (4 * indent_level)
    line_parts = [indent]

    if parent is None or parent.visibility != decl.visibility:
        line_parts.append(f"{decl.visibility.lower()}: ")

    display_name = decl.display_name
    if parent:
        display_name = display_name.removeprefix(parent.display_name + "::")
    line_parts.append(f"{decl.fancy_kind.plain} {display_name}")

    total_usages = sum(len(loc_set) for loc_set in decl.transitive_usages.values())
    ext_usages = sum(len(locs) for mod, locs in decl.transitive_usages.items() if mod != decl.mod)
    line_parts.append(f"; // {total_usages} usages, {ext_usages} external")

    print("".join(line_parts))

    if decl.visibility.endswith("private") and ext_usages:
        # This is only reachable if the merger exits with an error when checking
        # visibility. But it dumps the json first so we can still get here.
        print(f"{indent}^^ERROR^^: private declaration has external usages!")

    # Sort children for most readable/scannable output.
    for child_decl in sorted_decls(decl.sem_children):
        print_decl_recursive(child_decl, indent_level + 1, parent=decl)


def get_changed_files_git() -> list[str]:
    """Gets a list of changed files compared to origin/master."""
    try:
        merge_base = subprocess.run(
            ["git", "merge-base", "origin/master", "HEAD"],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()

        if not merge_base:
            perr_exit("Error: Could not determine merge base with origin/master.")

        result = subprocess.run(
            ["git", "diff", "--name-only", merge_base],
            capture_output=True,
            text=True,
            check=True,
        )
        changed_files = result.stdout.strip().split("\n")
        return [f for f in changed_files if f]  # Filter out empty strings
    except subprocess.CalledProcessError as e:
        perr_exit(f"Error running git: {e.stderr}")
    except FileNotFoundError:
        perr_exit("Error: 'git' command not found. Ensure Git is installed and in your PATH.")


def main():
    changed_files = get_changed_files_git()

    if not changed_files:
        perr_exit(
            "No changed files found compared to origin/master, or an error occurred.",
        )

    print(
        "// WARNING: This script will only show declarations that the tooling sees usages of.\n"
        "// There are various reasons why a declaration may not be seen as used by the tooling,\n"
        "// especially for aliases, so use this as a guide, rather than relying on it 100%.\n"
    )

    missing_files = []
    files = {}
    for filepath, file in browse.files.items():
        # Normalize to put generated files in the same place as sources
        filepath = filepath[filepath.index("src/mongo/") :]
        assert filepath not in files, f"Duplicate file entry: {filepath}"
        files[filepath] = file

    for filepath in changed_files:
        human_filepath = filepath
        if filepath.endswith(".idl"):
            # Convert .idl files to generated .h files
            filepath = f"{filepath[:-4]}_gen.h"
            human_filepath = f"{filepath} (generated from {human_filepath})"

        if (
            not filepath.startswith("src/mongo/")
            or "/third_party/" in filepath
            or not any(filepath.endswith(ext) for ext in (".h", ".hpp", ".inl", ".ipp", ".defs"))
        ):
            # Only consider first-party headers
            continue

        if filepath not in files:
            if filepath.endswith(".h"):
                missing_files.append(human_filepath)
            continue

        print(f"// {human_filepath}")

        sorted_top_level_decls = sorted_decls(files[filepath].top_level_decls)

        for decl in sorted_top_level_decls:
            print_decl_recursive(decl)
        print()  # Newline for separation between files

    for filepath in missing_files:
        print(f"// WARNING: {filepath} was modified but has no usages. Review manually.")


if __name__ == "__main__":
    main()
    os._exit(0)  # Don't wait for garbage collection or other cleanup, just exit immediately
