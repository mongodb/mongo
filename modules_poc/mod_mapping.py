#!/usr/bin/env python3

import os
import sys
from glob import glob
from pathlib import Path  # if you haven't already done so
from typing import NamedTuple, NoReturn

import codeowners
import regex as re
import yaml
from codeowners import CodeOwners

try:
    from yaml import CLoader as Loader
except ImportError:
    raise RuntimeError("Why no cYaml?")

file = Path(__file__).resolve()
parent, root = file.parent, file.parents[1]
sys.path.append(str(file.parent))

from cindex import Cursor
from cindex import File as ClangFile


def perr(*values):
    print(*values, file=sys.stderr)


def perr_exit(*values) -> NoReturn:
    perr(*values)
    sys.exit(1)


if codeowners.path_to_regex("/**/bar").match("/foobar"):
    # Detect an outdated version suffering from https://github.com/sbdchd/codeowners/issues/43.
    # We need to update to at least 0.8.0 to get the fix.
    perr_exit("please run buildscripts/poetry_sync.sh to update dependencies")


with open(root / ".github/CODEOWNERS") as f:
    code_owners = CodeOwners(f.read())

with open(parent / "modules.yaml") as f:

    def parseModules():
        raw_mods = yaml.load(f, Loader=Loader)
        lines = []
        for mod, info in raw_mods.items():
            for glob in info["files"]:
                lines.append(f"/{glob} @10gen/{mod}")
        # If multiple rules match, later wins. So put rules with more
        # specificity later. For all of our current rules, longer means more
        # specific.
        lines.sort(key=lambda l: len(l.split()[0]))
        return "\n".join(lines)

    modules = CodeOwners(parseModules())


def normpath_for_file(f: Cursor | ClangFile | str | None) -> str | None:
    if f is None:
        return None
    if isinstance(f, Cursor):
        return normpath_for_file(f.location.file)

    name = f.name if type(f) == ClangFile else f
    if "/third_party/" in name:
        return None

    offset = name.find("src/mongo")
    if offset == -1:
        return None

    name = name[offset:]
    return os.path.normpath(name)  # fix up a/X/../b/c.h -> a/b/c.h


file_mod_map: dict[str, str] = {}


def mod_for_file(f: ClangFile | str | None) -> str | None:
    name = normpath_for_file(f)
    if not name:
        return None

    if name and name.endswith("_gen.h") or name.endswith("_gen.cpp"):
        name = re.sub(r"_gen\.(h|cpp)$", ".idl", name)

    if name in file_mod_map:
        return file_mod_map[name]

    match modules.of(name):
        case []:
            mod = "__NONE__"
        case [[kind, mod]]:
            assert kind == "TEAM"
            ignore = "@10gen/"
            assert mod.startswith(ignore)
            mod = mod[len(ignore) :]
        case owners:
            perr_exit(
                f"ERROR: multiple owners for file {name}: {', '.join(mod for (_, mod) in owners)}"
            )
    file_mod_map[name] = mod
    return mod


def teams_for_file(f: ClangFile | str | None):
    name = normpath_for_file(f)
    if name is None:
        return []

    # No need to cache since this is called once per file
    teams = []
    for kind, owner in code_owners.of(name):
        if kind != "TEAM":  # ignore both individual engineers and svc-auto-approve-bot
            continue
        ignore = "@10gen/"
        assert owner.startswith(ignore)
        owner = owner[len(ignore) :]
        owner = owner.replace("-", "_")  # easier for processing with jq
        teams.append(owner)

    return teams if teams else ["__NO_OWNER__"]


def glob_paths():
    for path in glob("src/mongo/**/*", recursive=True):
        if "/third_party/" in path:
            continue
        extensions = ("h", "cpp", "idl", "c", "defs", "inl", "hpp")
        if not any(path.endswith(f".{ext}") for ext in extensions):
            continue
        yield path


def dump_modules() -> None:
    out: dict[str, dict[str, dict[str, list[str]]]] = {}
    for path in glob_paths():
        mod = mod_for_file(path)
        assert mod  # None would mean not first-party, but that is already filtered out.
        (dir, leaf) = path.rsplit("/", 1)
        for team in teams_for_file(path):
            # In cases where multiple teams own a file, this will list the file multiple times.
            # This is intended to play nicely with teams trying to filter to just the files they own.
            out.setdefault(mod, {}).setdefault(team, {}).setdefault(dir, []).append(leaf)

    for teams in out.values():
        for dirs in teams.values():
            for files in dirs.values():
                files.sort()
    yaml.dump(out, open("modules_dump.yaml", "w"))


def dump_list() -> None:
    for line in sorted(f"{path} -- {mod_for_file(path)}" for path in glob_paths()):
        print(line)


def validate_modules() -> bool:
    def glob_is_prefix(short: str, long: str):
        # Simplistic but good enough for now. In particular, I want to make sure we would
        # catch things like "foo*" and "*bar*" both matching "foobar".
        assert len(short) <= len(long)  # argument are sorted by length before calling
        if short == long:
            return False  # duplicates are treated as errors
        if long.startswith(short):
            return True  # foo and foo/ are prefixes of foo/bar
        if short.endswith("*") and long.startswith(short[:-1]):
            return True  # foo* is a prefix of foo/bar and foobar
        return False

    class Info(NamedTuple):
        mod: str
        glob: str

    info_for_line = {
        info[3]: Info(
            mod=info[2][0][1].removeprefix("@10gen/"),
            glob=info[1][1:],
        )
        for info in modules.paths
    }
    seen_lines = set[int]()

    failed = False
    for path in glob_paths():
        matches = list(modules.matching_lines(path))
        for match in matches:
            seen_lines.add(match[1])

        if not matches:
            teams = " and ".join(teams_for_file(path))
            perr(f"Error: {path} owned by {teams} doesn't match any globs in modules.yaml")
            failed = True

        if len(matches) <= 1:
            continue

        infos = sorted((info_for_line[match[1]] for match in matches), key=lambda i: len(i.glob))
        for i in range(0, len(infos)):
            for j in range(i, len(infos)):
                a = infos[i]
                b = infos[j]
                if a.mod != b.mod and not glob_is_prefix(a.glob, b.glob):
                    perr(
                        f"Error: {path} matches multiple globs that are neither prefixes nor same module:"
                    )
                    for info in infos:
                        perr(f"  {info.glob}  ({info.mod})")
                    failed = True
                    break
            else:
                continue
            break  # break out of outer loop

    for line, info in info_for_line.items():
        if line not in seen_lines:
            perr(f"Error: glob '{info.glob}' in module {info.mod} doesn't match any files")
            failed = True
    return failed


def main():
    if len(sys.argv) == 2:
        match sys.argv[1]:
            case "--dump-modules":
                sys.exit(dump_modules())
            case "--dump-modules-list":
                sys.exit(dump_list())
            case "--validate-modules":
                sys.exit(validate_modules())

    perr_exit(f"Usage: {sys.argv[0]} (--dump-modules|--dump-modules-list|--validate-modules)")


if __name__ == "__main__":
    main()

# cspell: perr cindex
