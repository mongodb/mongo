#!/usr/bin/env python3
import re

import yaml

try:
    from yaml import CDumper as Dumper
    from yaml import CLoader as Loader
except ImportError:
    raise RuntimeError("Why no cYaml?")
    # from yaml import Loader, Dumper

import dataclasses
import json
import os
import sys
from dataclasses import dataclass
from datetime import datetime
from glob import glob
from pathlib import Path  # if you haven't already done so
from typing import NoReturn

from codeowners import CodeOwners

file = Path(__file__).resolve()
parent, root = file.parent, file.parents[1]
sys.path.append(str(file.parent))
# os.chdir(parent.parent)  # repo root (uncomment for python debugger)

import cindex as clang
from cindex import Config, Cursor, CursorKind, Index, LinkageKind, RefQualifierKind, TranslationUnit
from cindex import File as ClangFile

# Monkey patch some features into clang's python binding. Keeping commented out for now in case we decide not to use modified lib.
# clang.functionList.append(("clang_File_isEqual", [ClangFile, ClangFile], ctypes.c_int))
# clang.functionList.append(("clang_Cursor_hasAttrs", [Cursor], ctypes.c_uint))
# clang.Cursor.__hash__ = lambda self: self.hash
# clang.File.__eq__ = lambda self, other: other is not None and bool(
#     clang.conf.lib.clang_File_isEqual(self, other)
# )
# def get_specialized_template(node: Cursor):
#     return Cursor.from_cursor_result(clang.conf.lib.clang_getSpecializedCursorTemplate(node), node)
# def has_attrs(node: Cursor):
#     return node.has_attrs()


# This papers over a difference between using clang-19 or clang-12
def is_tu(c: Cursor | CursorKind):
    if isinstance(c, Cursor):
        c = c.kind

    OLD_TRANSLATION_UNIT = 300  # clang-12
    return c == CursorKind.TRANSLATION_UNIT or c.value == OLD_TRANSLATION_UNIT


out_from_env = os.environ.get("MOD_SCANNER_OUTPUT", None)
is_local = out_from_env is None


file_mod_map: dict[str, str] = {}

with open(root / ".github/CODEOWNERS") as f:
    code_owners = CodeOwners(f.read())

with open(parent / "modules.yaml") as f:

    def parseModules():
        raw_mods = yaml.load(f, Loader=Loader)
        lines = []
        for mod, globs in raw_mods.items():
            for glob in globs:
                lines.append(f"/{glob} @10gen/{mod}")
                if glob.endswith(".idl"):
                    lines.append(f"/{glob[:-4]} _gen.* @10gen/{mod}")
        # If multiple rules match, later wins. So put rules with more
        # specificity later. For all of our current rules, longer means more
        # specific.
        lines.sort(key=lambda l: len(l.split()[0]))
        return "\n".join(lines)

    modules = CodeOwners(parseModules())


def perr(*values):
    print(*values, file=sys.stderr)


def perr_exit(*values) -> NoReturn:
    perr(*values)
    sys.exit(1)


DETAIL_REGEX = re.compile(r"(detail|internal)s?$")


def get_visibility(c: Cursor, scanning_parent=False):
    if is_tu(c):
        return "UNKNOWN"  # break recursion

    prefix = "mongo::mod::"
    shallow = "shallow::"
    if c.has_attrs():
        for child in c.get_children():
            if child.kind != CursorKind.ANNOTATE_ATTR:
                continue
            attr = child.spelling
            if not attr.startswith(prefix):
                continue
            attr = attr[len(prefix) :]
            if attr.startswith(shallow):
                if scanning_parent:
                    continue  # shallow doesn't apply to children
                attr = attr[len(shallow) :]
            assert attr in ("public", "private", "unfortunately_public")
            return attr

    # Some rules for implicitly private decls
    # TODO: Unfortunately these rules are violated on 64 declarations,
    # so it can't be enabled yet.
    #
    # - Some of the forTest methods appear to be intended as helpers for
    #   consumers writing tests. We may want to use a different suffix like
    #   "forTests" for that.
    # - The usages of details namespace violations are more tricky, and there
    #   appear to be a few kinds:
    #   - True violations: we should fix these.
    #   - Files not mapped to modules correctly: we should fix the mapping.
    #   - APIs intended to be used from macro implementations: We might be
    #     able to fix these by using clang_getFileLocation rather than
    #     clang_getInstantiationLocation, but I don't think we want to do
    #     that everywhere and it isn't currently exposed from python.
    #     For now we may just want to mark those as public.
    #   - Types not intended to be named directly by consumers, but used as
    #     part of public APIs (eg return types or base classes) such that
    #     consumers are expected to use their APIs. Maybe they should be
    #     declared public anyway?
    if 0:  # :(
        if c.spelling.endswith("forTest"):
            return "private"

        # details and internal namespaces
        if c.kind == CursorKind.NAMESPACE and DETAIL_REGEX.match(c.spelling):
            return "private"

    return get_visibility(c.semantic_parent, scanning_parent=True)


def mod_for_file(f: ClangFile | str | None) -> str | None:
    if f is None:
        return None

    name = f.name if type(f) == ClangFile else f
    prefix = "src/mongo/"
    offset = name.find(prefix)
    if offset == -1:
        return None
    rest = name[offset + len(prefix) :]
    if "/third_party/" in rest:
        return None

    rest = os.path.normpath(rest)  # fix up a/X/../b/c.h -> a/b/c.h
    if rest in file_mod_map:
        return file_mod_map[rest]

    # TODO use a real module definition file rather than deriving from CODEOWNERS.
    use_codeowners = False
    source = code_owners if use_codeowners else modules
    owners = []
    for kind, owner in source.of(prefix + rest):
        if kind != "TEAM":
            continue
        ignore = "@10gen/"
        assert owner.startswith(ignore)
        owners.append(owner[len(ignore) :])
    assert use_codeowners or len(owners) <= 1
    mod = "+".join(owners)
    mod = mod if mod else "__NONE__"
    file_mod_map[rest] = mod
    return mod


@dataclass
class Decl:
    display_name: str
    usr: str
    # mangled_name: str
    loc: str
    kind: str
    mod: str | None
    linkage: str
    defined: bool
    spelling: str
    visibility: str
    sem_par: str
    lex_par: str
    used_from: dict[str, set[str]] = dataclasses.field(default_factory=dict, compare=False)

    def def_or_decled(self) -> str:
        return "defined" if self.defined else "declared"

    @staticmethod
    def from_cursor(c: Cursor, mod=None):
        return Decl(
            display_name=fully_qualified(c),
            spelling=c.spelling,
            usr=c.get_usr(),
            # mangled_name=c.mangled_name,
            loc=pretty_location(c.location),
            linkage=c.linkage.name,
            kind=c.kind.name,
            mod=mod or mod_for_file(c.location.file),
            defined=c.is_definition(),
            visibility=get_visibility(c),
            sem_par=c.semantic_parent.get_usr(),
            lex_par=c.lexical_parent.get_usr(),
        )


def pretty_location(loc: clang.SourceLocation | clang.Cursor):
    if isinstance(loc, Cursor):
        loc = loc.location
    name = loc.file.name if loc.file else "<unknown>"
    # return f"{name}({loc.line},{loc.column})"  # MSVC format
    return f"{name}:{loc.line}:{loc.column}"  # gcc format


decls = dict[str, Decl]()


def fully_qualified(c: Cursor):
    parts = []
    while c is not None and not is_tu(c):
        spelling = c.displayname
        if spelling:
            if c.is_const_method():
                spelling += " const"
            match c.type.get_ref_qualifier():
                case RefQualifierKind.LVALUE:
                    spelling += " &"
                case RefQualifierKind.RVALUE:
                    spelling += " &&"
            parts.append(spelling)
        c = c.semantic_parent
    if not parts:
        return ""
    assert parts
    if parts[-1] == "mongo":
        parts.pop()
    else:
        parts.append("")

    parts.reverse()
    return "::".join(parts)


def add_decl(d: Decl):
    if d.usr not in decls:
        decls[d.usr] = d
        return

    old = decls[d.usr]
    if old.mod != d.mod:
        perr(
            f"{d.loc}:warning: {d.kind} {d.display_name} {d.def_or_decled()} in module {d.mod} "
            + f"after previously being {old.def_or_decled()} in module {old.mod}"
        )
        perr(f"{old.loc}:note: prior definition here")

    if d.defined and old.defined:
        # print(d.kind)
        # print(d.kind == CursorKind.TYPEDEF_DECL)
        # if d.kind == CursorKind.TYPEDEF_DECL:
        #     return  # TODO: how to handle this?
        if d == old:
            return  # it doesn't matter, ignore it
        if not any(
            special_case in d.display_name
            for special_case in ("(unnamed ", "UFDeductionHelper", "<IsConst, IndexScanStats>")
        ) and not d.spelling.startswith("(anonymous "):
            return  # ignore
            print("detected duplicate definitions!")
            print(d.loc, d)
            print(old.loc, old)
            assert not (d.defined and old.defined)

    if d.defined and not old.defined:
        assert not d.used_from
        d.used_from = old.used_from
        decls[d.usr] = d

    # TODO consider merging otherwise?


# These are completely skipped during decl finding
skip_kinds = {
    # parameters
    CursorKind.PARM_DECL,
    CursorKind.TEMPLATE_TYPE_PARAMETER,
    CursorKind.TEMPLATE_NON_TYPE_PARAMETER,
    CursorKind.TEMPLATE_TEMPLATE_PARAMETER,
    # Function bodies
    CursorKind.COMPOUND_STMT,
    CursorKind.CXX_TRY_STMT,
    # Useless
    CursorKind.CXX_ACCESS_SPEC_DECL,  # doesn't have children
    CursorKind.STATIC_ASSERT,
    #
    # TODO Consider for future for things like hidden friends
    CursorKind.FRIEND_DECL,
}

skip_mods: tuple[str, ...] = ()


def find_decls(mod: str, c: Cursor):
    if c.location.file:
        assert mod_for_file(c.location.file) == mod  # maybe

    if c.kind.is_declaration() and c.kind != CursorKind.NAMESPACE and c.spelling:
        add_decl(Decl.from_cursor(c))

    if c.kind == CursorKind.TYPE_ALIAS_TEMPLATE_DECL:
        return

    for child in c.get_children():
        if child.kind in skip_kinds:
            continue
        if child.kind.is_attribute():
            continue
        find_decls(mod, child)


def find_usages(mod: str, c: Cursor):
    ref = c.referenced
    # Handle children first. This makes it possible to use early returns below
    for child in c.get_children():
        # Don't count friendship as a "usage". This causes problems since the friend decl
        # becomes the canonical decl for the type for any TU that doesn't see the definition.
        # "Hidden friend" definitions *are* traversed.
        if c.kind == CursorKind.FRIEND_DECL and not child.is_definition():
            return

        assert child != c
        assert ref is None or child != ref or ref.kind == CursorKind.OVERLOADED_DECL_REF
        find_usages(mod, child)

    if ref is None or ref == c:
        return

    if ref.kind in (
        CursorKind.NAMESPACE,
        CursorKind.NAMESPACE_ALIAS,
        CursorKind.TEMPLATE_TEMPLATE_PARAMETER,
        CursorKind.TEMPLATE_TYPE_PARAMETER,
        CursorKind.TEMPLATE_NON_TYPE_PARAMETER,
        CursorKind.PARM_DECL,
    ):
        return

    # NOTE: This is for templated variables and their specializations. Ideally these
    # would be tracked, but we only have 27 template variables (4 of which are used
    # cross-module) and they are generating thousands of unique declarations because
    # libclang doesn't expose enough info for us to merge them well. This massively
    # skews the results because they are 10% of all decls!
    # TODO: we should at least check that private decls aren't used from the wrong mod
    # before returning.
    if ref.kind == CursorKind.UNEXPOSED_DECL:
        return

    # This is for local variables
    if ref.linkage == LinkageKind.NO_LINKAGE and ref.kind in (
        CursorKind.VAR_DECL,
        CursorKind.UNEXPOSED_DECL,
    ):
        # double check that we aren't missing any cross-module usages.
        # fails only on a mozjs .defs X-macro file.
        # assert ref.location.file == c.location.file
        return

    # Unfortuntely libclang's c api doesn't handle implicitly declared methods
    # well. In particular it often points at a location of a forward decl of the
    # class rather than the definition, even if both are visible. And then the
    # rest of our handling doesn't work correctly. And it also doesn't have a
    # way to distinguish implicit methods from explicitly defaulted ones. So we
    # just resolve all defaulted methods to the type and continue from there.
    if ref.is_default_method():
        ref = ref.semantic_parent

    # assert not c.location.file or mod_for_file(c.location.file) == mod

    # unresolve implicit instantiations
    while templ := ref.specialized_template:
        if templ.location != ref.location and templ.extent != ref.extent:
            templ_def = templ.get_definition()
            if not templ_def or templ_def.location != ref.location:
                break
        ref = templ

    # Everything after this is about finding the best location and shouldn't change the USR. Asserted later.
    usr = ref.get_usr()
    if not usr:
        return

    if usr in decls:
        # We've already done the work to get th info for this decl.
        d = decls[usr]
    else:
        definition = ref.get_definition()
        # In clang terms, the "canonical" declaration is the first one seen in the TU.
        canonical = ref.canonical
        assert canonical
        if ref.kind not in (
            CursorKind.TYPEDEF_DECL,
            CursorKind.TYPE_ALIAS_DECL,
        ):  # Hit a clang bug :(
            assert canonical.get_usr() == usr
            if definition:
                assert definition.get_usr() == usr

        # Ignore any declarations not declared in a header.
        # TODO what if a local type is passed to a template? For now doesn't matter because we
        # don't look at usages from instantiations.
        if (not (file := canonical.location.file)) or file.name.endswith(".cpp"):
            return

        # This was decided by manually examining the unique kinds from the output.
        type_kinds = (
            CursorKind.ENUM_DECL,
            CursorKind.STRUCT_DECL,
            CursorKind.CLASS_DECL,
            CursorKind.CLASS_TEMPLATE,
            CursorKind.CLASS_TEMPLATE_PARTIAL_SPECIALIZATION,
            # Unsure about these:
            # CursorKind.TYPE_ALIAS_DECL,
            # CursorKind.TYPEDEF_DECL,
            # CursorKind.TYPE_ALIAS_TEMPLATE_DECL,
        )

        # For types, prefer the definition, otherwise use the canonical decl, but only if the definition is in a header.
        ref = canonical
        if ref.kind in type_kinds:
            if definition and definition.location.file.name.endswith(".h"):
                ref = definition

        # ignore uses from the same file - this avoids recording local variables
        # if ref.location.file == c.location.file:
        #     return

        decl_mod = mod_for_file(ref.location.file)
        if not decl_mod or decl_mod in skip_mods:
            return

        d = Decl.from_cursor(ref, decl_mod)
        decls[usr] = d

        if definition and ref != definition:
            def_mod = mod_for_file(definition.location.file)
            # Note def_mod is None means third_party, not __NONE__ module
            if def_mod != decl_mod and def_mod is not None:
                print(f"WARNING: decl_mod '{decl_mod}' != def_mod '{def_mod}' for {d.display_name}")
                print(f"decl: {pretty_location(ref)}")
                print(f"defn: {pretty_location(definition)}")
            else:
                d.defined = True

    # ignore usages from the same module
    # if d.mod == mod or mod.startswith(d.mod):
    #     return

    d.used_from.setdefault(mod, set()).add(pretty_location(c))


seen = set[Cursor]()


def ast(node: Cursor):
    templ = node.specialized_template
    usr = node.get_usr()
    if node in seen:
        return {
            "b_kind": node.kind.name,
            "c_usr": usr,
            "d_display": node.displayname,
            "e_location": pretty_location(node.location),
        }
    seen.add(node)

    if 0:  # toggle filtering
        children = [ast(c) for c in node.get_children()]
    else:
        children = []
        for c in node.get_children():
            if c.location.file is None:
                children.append(ast(c))
                continue

            if "src/mongo/" not in c.location.file.name:
                continue
            if c.kind == CursorKind.COMPOUND_STMT:
                continue

            children.append(ast(c))
    return {
        "b_kind": node.kind.name,
        "c_usr": str(usr),
        "d_display": str(node.displayname),
        "d_spelling": str(node.spelling),
        "e_location": pretty_location(node.location),
        "ee_mod": mod_for_file(node.location.file),
        # "f_extent.start": str(node.extent.start),
        # "g_extent.end": str(node.extent.end),
        "h_is_definition": node.is_definition(),
        "h_is_decl": node.kind.is_declaration(),
        "h_linkage": node.linkage.name,
        "z_ref": (ast(node.referenced) if node.referenced and node.referenced != node else None),
        "z_templ": ast(templ) if templ else None,
        "zz_children": children,
    }


class Timer:
    def __init__(self):
        self.start = datetime.now()

    def mark(self, label: str):
        if is_local:
            elapsed = datetime.now() - self.start
            print(f"{label}: {elapsed}")


timer = Timer()


def dump_modules():
    out: dict[str, dict[str, list[str]]] = {}
    for path in glob("src/mongo/**/*", recursive=True):
        if "/third_party/" in path:
            continue
        if not any(path.endswith(f".{ext}") for ext in ("h", "cpp", "c", "idl")):
            continue
        mod = mod_for_file(path)
        if not mod:
            mod = "__NONE__"
        (dir, leaf) = path.rsplit("/", 1)
        out.setdefault(mod, {}).setdefault(dir, []).append(leaf)
    print(len(out))

    for dirs in out.values():
        for files in dirs.values():
            files.sort()
    yaml.dump(out, open("modules.yaml", "w"))


def parseTU(args: list[str] | str):
    if not Config.loaded:
        Config.set_compatibility_check(False)
        external = "external" if os.path.exists("external") else "bazel-out/../../../external"
        paths_to_try = [
            f"{external}/mongo_toolchain_v5/v5/lib/libclang.so",
            f"{external}/mongo_toolchain_v4/v4/lib/libclang.so",
            f"{external}/mongo_toolchain/v4/lib/libclang.so",
        ]
        for path in paths_to_try:
            if os.path.exists(path):
                Config.set_library_file(path)
                break
        else:
            path_lines = "\n\t".join(paths_to_try)  # can't have \ in f-string expr
            perr_exit(f"Unable to find libclang.so. Paths tried:\n\t{path_lines}")

        # Config.set_library_file("/home/ubuntu/clang+llvm-19.1.1-aarch64-linux-gnu/lib/libclang.so")

    if type(args) == str:
        args = [args]

    if len(args) == 1:
        compdb = clang.CompilationDatabase.fromDirectory(".")
        commands = compdb.getCompileCommands(args[0])
        if commands is None:
            perr_exit(f"no compile commands for {args[0]}")

        if len(commands) != 1:
            perr_exit(f"too many compile commands for {args[0]}", commands)

        # print(" ".join(commands[0].arguments))
        args = list(commands[0].arguments)[1:]  # skip executable

    # somehow clang implicitly adds args that it warns about
    cleanArgs = ["-Wno-unused-command-line-argument"]
    for arg in args:
        if arg in ("-MD", "-MMD", "-MF"):
            continue
        if arg.endswith(".d"):
            continue
        cleanArgs.append(arg)
        # print(arg)

    # Disable all warnings. Don't waste time on them when parsing.
    cleanArgs.append("-w")

    index = Index.create()
    timer.mark("preparse")
    tu = index.parse(None, cleanArgs)
    if not tu:
        raise RuntimeError("unable to load input")

    for d in tu.diagnostics:
        perr(d)
    timer.mark("parsed")
    return tu


def dump_unused_inputs(outPath: str, tu: TranslationUnit):
    # only looking in src/mongo to cut down on resources, and to reduce the risk of accidentally
    # including some file we shouldn't. Assumption is that third_party and generated sources won't
    # change in a tight feedback loop.
    universe = set(glob("src/mongo/**/*.h", recursive=True))
    timer.mark("globbed")
    for include in tu.get_includes():
        if include.source:
            universe.discard(include.source.name)
    with open(outPath, "w") as file:
        file.write("\n".join(sorted(universe)))
    timer.mark("outfile written")


def main():
    args = sys.argv[1:] or ["src/mongo/platform/waitable_atomic_test.cpp"]

    if len(args) == 0:
        perr_exit("invalid number of arguments")

    if args == ["--dump-modules"]:
        dump_modules()
        sys.exit()

    tu = parseTU(args)

    if unused_input_path := os.environ.get("MOD_SCANNER_UNUSED", None):
        dump_unused_inputs(unused_input_path, tu)

    assert is_tu(tu.cursor)

    if "DUMP_AST" in os.environ and is_local:  # useful for debugging (never on bazel)
        out = ast(tu.cursor)
        timer.mark("ast processed")
        with open("ast.yaml", "w") as f:
            yaml.dump(out, f, Dumper=Dumper)
        timer.mark("ast dumped")

    # for top_level in tu.cursor.get_children():
    #     if "src/mongo/" not in top_level.location.file.name:
    #         continue
    #     find_decls(mod_for_file(top_level.location.file), top_level)
    # timer.mark("found decls")

    for top_level in tu.cursor.get_children():
        if "src/mongo/" not in top_level.location.file.name:
            continue
        find_usages(mod_for_file(top_level.location.file), top_level)
    timer.mark("found usages")

    with open(out_from_env or "decls.yaml", "w") as f:
        out = [dict(d.__dict__) for d in decls.values() if d.mod not in skip_mods]
        for decl in out:
            # del decl["spelling"]
            del decl["linkage"]
            # del decl["defined"]
            decl["used_from"] = {k: sorted(v) for k, v in decl["used_from"].items()}

        # This makes us only output decls used cross-module. It makes merging much faster,
        # but, it means that we can mask some cross-module usages if something is forward
        # declared in the wrong module. Also this hides definitions from the
        # merger so it can't choose canonical versions. There is still the problem of
        # definitions not used from any TU where they are defined.
        if 0:
            for decl in out:
                if decl["mod"] in decl["used_from"]:
                    del decl["used_from"][decl["mod"]]
            out = list(filter(lambda d: d["used_from"], out))

        timer.mark("processed")
        if f.name.endswith(".json"):
            json.dump(out, f)
        else:
            assert f.name.endswith(".yaml")
            yaml.dump(out, f, Dumper=Dumper)
        timer.mark("dumped")


if __name__ == "__main__":
    main()
