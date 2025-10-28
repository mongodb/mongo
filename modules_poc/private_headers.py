#!/usr/bin/env python3

import os
import sys
from pathlib import Path
from itertools import chain
from codeowners import path_to_regex
from collections.abc import Callable
from re import Pattern

import typer
import glob

sys.path.append(os.path.dirname(os.path.abspath(__file__)))
from browse import load_decls, File, is_submodule_usage
from mod_mapping import mod_for_file, teams_for_file, glob_paths, normpath_for_file
from merge_decls import get_file_family_regex

REPO_ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def normalize_path(header_path: str) -> str:
    """Normalize and convert the path from *_gen.h to *.idl if applicable."""
    header_path = normpath_for_file(header_path)
    gen_suffix = "_gen.h"
    if header_path.endswith(gen_suffix):
        return header_path[: -len(gen_suffix)] + ".idl"
    return header_path


def path_passes_filters(
    file_path: str,
    pattern: Pattern | None = None,
    module: str | None = None,
    team: str | None = None,
) -> bool:
    if pattern and not pattern.match(file_path):
        return False
    if module and module != mod_for_file(file_path):
        return False
    if team and team not in teams_for_file(file_path):
        return False
    return True


PathToFile = dict[str, File]


def get_all_paths_to_files_passing_filter(
    pattern: Pattern | None = None, module: str | None = None, team: str | None = None
) -> PathToFile:
    """Load Files from merged_decls.json, return all Files that pass the string filters."""
    res = {}
    try:
        files = load_decls()
    except FileNotFoundError:
        print("Error: merged_decls.json not found. Run merge_decls.py first.")
        sys.exit(1)

    for file in files:
        normpath = normalize_path(file.name)
        if path_passes_filters(normpath, pattern, module, team):
            res[normpath] = file
    return res


def get_excluded_headers(paths_files: PathToFile) -> set[str]:
    """
    From the given files, get all headers that have external module usage or already have some visibility marked.
    These files will be excluded from being marked private.
    """
    headers_to_exclude = set()
    for path, file in paths_files.items():
        has_unknown = False
        for decl in file.all_decls:
            if decl.visibility == "UNKNOWN":
                has_unknown = True
                for mod in decl.direct_usages:
                    if not is_submodule_usage(file, mod):
                        headers_to_exclude.add(path)
                        break

        # Already marked.
        if not has_unknown:
            headers_to_exclude.add(path)

    return headers_to_exclude


def find_for_test_violations(paths_files: PathToFile) -> list[tuple[str, str]]:
    """Find unmarked _forTest functions (which default to FILE_PRIVATE) that are used outside of its file family."""
    violations = []

    for path, file in paths_files.items():
        file_family_regex = get_file_family_regex(file.name)
        for decl in file.all_decls:
            if not (decl.visibility == "UNKNOWN" and decl.spelling.endswith("_forTest")):
                continue

            if any(
                not file_family_regex.match(l.loc.file)
                for locs in decl.direct_usages.values()
                for l in locs
            ):
                violations.append((decl.loc, decl.display_name))

    return violations


def add_modules_include(file_path: str, write_unless_dry: Callable) -> bool:
    """Add `#include "mongo/util/modules.h"` to a header file if not already present."""
    full_path = Path(REPO_ROOT) / file_path
    if not full_path.exists():
        return False

    try:
        content = full_path.read_text()
        if '#include "mongo/util/modules_incompletely_marked_header.h"' in content:
            content = content.replace(
                '#include "mongo/util/modules_incompletely_marked_header.h"',
                '#include "mongo/util/modules.h"',
            )
            full_path.write_text(content)
            return True

        lines = content.split("\n")
        insert_index = None

        has_includes = False
        for i, line in enumerate(lines):
            stripped = line.strip()
            if stripped == "#pragma once":
                insert_index = i + 1
            elif stripped.startswith("#include"):
                has_includes = True
                insert_index = i
                break
            elif stripped.startswith("#if"):
                break

        if insert_index is None:
            raise Exception(
                f'Could not find place to insert "#include" in {file_path}, '
                "need to manually insert."
            )

        if insert_index and not has_includes:
            lines.insert(insert_index, "")
            insert_index += 1

        lines.insert(insert_index, '#include "mongo/util/modules.h"')

        write_unless_dry(full_path, "\n".join(lines))
        return True

    except Exception as e:
        print(f"  Error modifying {file_path}: {e}")
        return False


def add_mod_visibility_to_idl(idl_path: str, write_unless_dry: Callable) -> bool:
    """Add `mod_visibility: private` to an IDL file."""
    full_path = Path(REPO_ROOT) / idl_path
    if not full_path.exists():
        return False

    try:
        lines = full_path.read_text().split("\n")

        # Look for 'global:' section or create it.
        global_line_index = None
        for i, line in enumerate(lines):
            if line == "global:":
                global_line_index = i
                break

        if global_line_index is not None:
            insert_index = global_line_index + 1
            lines.insert(insert_index, f"    mod_visibility: private")
        else:
            # Find the end of header comments.
            insert_index = 0
            for i, line in enumerate(lines):
                stripped = line.strip()
                if stripped == "" or stripped.startswith("#"):
                    insert_index = i + 1
                else:
                    break

            lines[insert_index : insert_index + 2] = ("global:", f"    mod_visibility: private", "")

        write_unless_dry(full_path, "\n".join(lines))
        return True

    except Exception as e:
        print(f"Error modifying IDL {idl_path}: {e}")
        return False


def main(
    pattern: str | None = typer.Option(
        None,
        "--glob",
        "-g",
        help="Glob pattern to filter files. `*/transport/*` and `transport` both match against `src/mongo/transport/hello_metrics.h`.",
    ),
    module: str | None = typer.Option(
        None, "--module", "-m", help="Module name to filter files e.g. `core.unittest`."
    ),
    team: str | None = typer.Option(
        None, "--team", "-t", help="Code owner team to filter files e.g. `server_programmability`."
    ),
    dry_run: bool = typer.Option(
        False, "--dry-run", "-n", help="Show what would be done without making changes"
    ),
):
    """
    Mark headers as private that have no external usage detected. Flag usages of `_forTest` functions used
    outside of their file family.
    """

    # Closure for dry_run
    def write_unless_dry(path: Path, content: str):
        if not dry_run:
            path.write_text(content)

    # Print warning first
    print(
        "WARNING: This tool marks all files where there are no currently detected external usages."
    )
    print("That does not necessarily mean that it is intended to be private.")
    print("A human should review to ensure that this matches intent.")

    all_paths_to_files = get_all_paths_to_files_passing_filter(
        path_to_regex(pattern) if pattern else None,
        module,
        team,
    )

    # Get headers with external usage. This is faster than finding headers without external usage.
    headers_to_exclude = get_excluded_headers(all_paths_to_files)

    print(
        f"\nThe following {len(headers_to_exclude)} files contain external usages"
        " and will not be modified:"
    )
    for file_path in sorted(headers_to_exclude):
        print(f"  {file_path}")

    # Find files that can be marked as private by taking the difference between [all headers] and
    # [headers with external usages, headers with some visibility already marked].
    private_candidates = sorted(all_paths_to_files.keys() - headers_to_exclude)

    print(f"\nFound {len(private_candidates)} files to mark as private:")

    headers_modified = []
    idl_files_modified = []

    # Possibly modify files for #include or mod_visibility.
    for file_path in private_candidates:
        print(f"  {file_path} - {mod_for_file(file_path)} - {', '.join(teams_for_file(file_path))}")

        if file_path.endswith(".idl"):
            if add_mod_visibility_to_idl(file_path, write_unless_dry):
                idl_files_modified.append(file_path)
        else:
            if add_modules_include(file_path, write_unless_dry):
                headers_modified.append(file_path)

    # Output results of modifying files.
    if not dry_run:
        if headers_modified:
            print(
                f'\nModified {len(headers_modified)} source files to add `#include "mongo/util/modules.h"`'
            )
            for file_path in headers_modified:
                print(f"  {file_path}")

        if idl_files_modified:
            print(f"Modified {len(idl_files_modified)} IDL files to add `mod_visibility: private`")
            for file_path in idl_files_modified:
                print(f"  {file_path}")
        print("Please run 'bazel run format' to clean up changes to headers")
    else:
        print(
            f"\nDry run complete. Would modify {len(headers_modified) + len(idl_files_modified)} files."
        )

    # Output `_forTest` violations.
    print("\nChecking for _forTest functions that will become FILE_PRIVATE...")
    violations = find_for_test_violations(all_paths_to_files)
    if violations:
        print("WARNING: Found _forTest functions with external usage.")
        print(
            "WARNING: Consider marking with MONGO_MOD_PRIVATE to allow continued use within the module."
        )
        for loc, display_name in violations:
            print(f'  "{display_name}" - {loc}')
    else:
        print("No _forTest violations found.")


if __name__ == "__main__":
    typer.run(main)
