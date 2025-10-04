#!/usr/bin/env python3

import dataclasses
import functools
import itertools
import json
import os
import sys
from dataclasses import dataclass
from datetime import datetime
from functools import cache, cached_property
from glob import glob
from pathlib import Path  # if you haven't already done so
from typing import Literal, NoReturn

import pyzstd
import regex as re
import yaml

try:
    from yaml import CDumper as Dumper
except ImportError:
    raise RuntimeError("Why no cYaml?")

file = Path(__file__).resolve()
parent, root = file.parent, file.parents[1]
sys.path.append(str(file.parent))
# os.chdir(parent.parent)  # repo root (uncomment for python debugger)

import cindex as clang
from cindex import (
    AccessSpecifier,
    Config,
    Cursor,
    CursorKind,
    Index,
    LinkageKind,
    RefQualifierKind,
    TranslationUnit,
)
from mod_mapping import mod_for_file, normpath_for_file


def perr(*values):
    print(*values, file=sys.stderr)


def perr_exit(*values) -> NoReturn:
    perr(*values)
    sys.exit(1)


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


def is_tu(c: Cursor | CursorKind):
    if isinstance(c, Cursor):
        c = c.kind
    return c == CursorKind.TRANSLATION_UNIT


out_from_env = os.environ.get("MOD_SCANNER_OUTPUT", None)
is_local = out_from_env is None


class DecoratedCursor(Cursor):
    # All USRs start with 'c:'. Local USRs then have a filename+'@' followed by
    # an optional number+'@'. Global USRs just start with 'c:@'
    _USR_GLOBALIZER_REGEX = re.compile(r"c:[\w\.\-]+@(\d+@)?")

    # CursorKinds that represent types. For these we prefer definition locations.
    # This was decided by manually examining the unique kinds from the output.
    TYPE_KINDS = {
        CursorKind.ENUM_DECL,
        CursorKind.STRUCT_DECL,
        CursorKind.UNION_DECL,
        CursorKind.CLASS_DECL,
        CursorKind.CLASS_TEMPLATE,
        CursorKind.CLASS_TEMPLATE_PARTIAL_SPECIALIZATION,
        # Unsure about these:
        # CursorKind.TYPE_ALIAS_DECL,
        # CursorKind.TYPEDEF_DECL,
        # CursorKind.TYPE_ALIAS_TEMPLATE_DECL,
    }

    def __init__(self, c: Cursor):
        # Unfortunately, need to decompose and have Cursor constructor recompose.
        super().__init__(c._kind_id, c.xdata, c.data)
        self._tu = c._tu

    @staticmethod
    @cache
    def normalize(c: Cursor):
        assert c.kind != CursorKind.NAMESPACE  # Should not be called with this.

        # unresolve implicit instantiations
        while templ := c.specialized_template:
            # Clang unfortunate behavior: The method to "unspecialize" a template
            # will both go from implicit instantiation to the template *and* go from
            # an explicit specialization to the primary template. Ideally, we
            # would only do the first, but that isn't an option. So we try to fake
            # it by only using the result if the locations are the same. However,
            # in some cases (notably including template methods of a template class),
            # clang will jump from the definition to the declaration, and neither
            # orig.canonical or result.get_definition() works to get the same location.
            # So we compromise: fully unspecialize non-type templates (variables and
            # functions), but require the locs to match on types. This is important
            # because class specializations can have different members than their
            # primary template and we want to handle those correctly. We ignore all
            # child declarations of functions, so that isn't a problem there.
            # This still chokes on explicit and extern template instantiations, but
            # it isn't clear how to fix that.
            if c.kind in DecoratedCursor.TYPE_KINDS:
                if templ.location != c.location and templ.extent != c.extent:
                    templ_def = templ.get_definition()
                    if not templ_def or templ_def.location != c.location:
                        break
            c = templ

        usr = c.get_usr()
        definition = c.get_definition()
        # In clang terms, the "canonical" declaration is the first one seen in the TU.
        canonical = c.canonical
        assert canonical
        if c.kind not in (
            CursorKind.TYPEDEF_DECL,
            CursorKind.TYPE_ALIAS_DECL,
        ):  # Hit a clang bug :(
            assert canonical.get_usr() == usr
            if definition:
                assert definition.get_usr() == usr

        # For types, prefer the definition if it is in a header, otherwise use the canonical decl.
        c = canonical
        if c.kind in DecoratedCursor.TYPE_KINDS:
            if definition and definition.location.file.name.endswith(".h"):
                c = definition

        return DecoratedCursor(c)

    @cached_property
    def raw_parent(self):
        if is_tu(self.semantic_parent):
            # We never want to treat TUs as parents.
            return

        assert self.semantic_parent
        return DecoratedCursor(self.semantic_parent)

    @cached_property
    def normalized_parent(self):
        if not self.raw_parent:
            return None

        if self.raw_parent.kind == CursorKind.NAMESPACE:
            return self.raw_parent  # Note: returning same object to share cached properties.

        return DecoratedCursor.normalize(self.raw_parent)

    @property
    def normalized_parents(self):
        p = self.normalized_parent
        while p and not is_tu(p):
            yield p
            p = p.normalized_parent

    @cached_property
    def raw_usr(self):
        return self.get_usr()

    @cached_property
    def globalized_usr(self):
        """
        Removes the file and unique number clang adds to some USRs without external linkage.
        This includes (among other cases) anything that has a lambda as part of its type,
        and namspace-scope constant integers. This interferes with our normalizing of USRs
        because it breaks the rule that everything's USR starts with its partent's USR.
        Globalizing restores that property.

        I have manually verified that this does not cause problematic collisions between USRs.
        There were only 4 groups of declarations that ended up with the same USR after
        globalizing. 3 were all function-local lambdas that get filtered out with other
        function-local declarations, and the last was the decay operator for lambdas
        used to build a hand-rolled VTable in a class's private section.
        """
        usr = DecoratedCursor._USR_GLOBALIZER_REGEX.sub("c:@", self.raw_usr)
        return usr

    @cached_property
    def normalized_usr(self):
        """
        Like globalized_usr, but replaces the raw_parent's USR prefix with the normalized _parent's USR
        """
        usr = self.globalized_usr
        if not usr or self.kind == CursorKind.NAMESPACE or not self.raw_parent:
            # Namespaces don't undergo any normalization, so we can break the cycle here.
            return usr

        assert usr.startswith(self.raw_parent.globalized_usr)
        return self.normalized_parent.normalized_usr + usr[len(self.raw_parent.globalized_usr) :]

    @cached_property
    def definition(self):
        d = self.get_definition()
        if not d:
            return None
        if d == self:
            return self  # keep cache
        return DecoratedCursor(self)

    @property  # no need to cache
    def has_definition(self):
        return self.definition is not None

    @cached_property
    def string_for_context(self):
        if self.kind == CursorKind.STATIC_ASSERT:
            return self.kind.name
        else:
            return f"{self.kind.name} {fully_qualified(self, 'spelling')}"


DETAIL_REGEX = re.compile(r"(detail|internal)s?$")


@dataclass
class GetVisibilityResult:
    attr: str
    alt: str | None
    parent: DecoratedCursor | None  # only None for UNKNOWN
    non_ns_parent: DecoratedCursor | None


def get_visibility(
    c: DecoratedCursor, scanning_parent=False, last_non_ns_parent=None
) -> GetVisibilityResult:
    if c.kind != CursorKind.NAMESPACE:
        last_non_ns_parent = c
    is_internal_namespace = c.kind == CursorKind.NAMESPACE and DETAIL_REGEX.search(c.spelling)
    in_complete_header = normpath_for_file(c) in complete_headers

    # ideally this would be in an if c.has_attrs() block, but that seems to not work in all cases.
    # TODO: try again when on a newer clang. Also might be worth seeing if we can narrow down
    # the cases where it doesn't work.
    for child in c.get_children():
        if child.kind != CursorKind.ANNOTATE_ATTR:
            continue
        terms = child.spelling.split("::")
        if not (len(terms) >= 3 and terms.pop(0) == "mongo" and terms.pop(0) == "mod"):
            continue
        if terms[0] == "shallow":
            terms.pop(0)
            assert terms
            if scanning_parent:
                continue  # shallow doesn't apply to children
        attr = terms.pop(0)
        if is_internal_namespace and attr not in ("private", "file_private"):
            perr(
                pretty_location(c.location)
                + ": namespaces ending in 'detail(s)' or 'internal(s)' are implicitly private."
                + " Attributes other than MONGO_MOD_FILE_PRIVATE are ignored."
            )
            break
        if terms:
            alt = "::".join(terms)
            assert attr in ("use_replacement",)
            # Must specify an alternate API
            if not alt or alt.isspace():
                perr_exit(
                    pretty_location(child.location)
                    + ": MONGO_MOD_USE_REPLACEMENT() must specify the replacement API"
                )
        else:
            alt = None
            assert attr in (
                "open",
                "public",
                "private",
                "file_private",
                "parent_private",
                "needs_replacement",
            )
            if attr == "open" and scanning_parent:
                # "open" only applies to the current class and makes semantic children public.
                attr = "public"
            if attr == "parent_private":
                mod = mod_for_file(c.location.file)
                if "." not in mod:
                    perr_exit(
                        pretty_location(child.location)
                        + f": MONGO_MOD_PARENT_PRIVATE used in top-level module {mod}"
                    )
        return GetVisibilityResult(attr, alt, c, last_non_ns_parent)

    # Apply high-priority defaults that override parent's visibility.
    if in_complete_header:
        # details and internal namespaces
        if is_internal_namespace:
            return GetVisibilityResult("private", None, c, last_non_ns_parent)

        if c.spelling.endswith("_forTest"):
            return GetVisibilityResult("file_private", None, c, last_non_ns_parent)

        if not scanning_parent:
            # TODO consider making PROTECTED also default to module private
            if c.access_specifier == AccessSpecifier.PRIVATE:
                return GetVisibilityResult("private", None, c, last_non_ns_parent)

    if c.normalized_parent:
        parent_vis = get_visibility(
            c.normalized_parent, scanning_parent=True, last_non_ns_parent=last_non_ns_parent
        )
    else:
        parent_vis = GetVisibilityResult("UNKNOWN", None, None, None)  # break recursion

    # Apply low-priority defaults that defer to parent's visibility
    if not scanning_parent and parent_vis.attr == "UNKNOWN" and in_complete_header:
        return GetVisibilityResult("private", None, c, last_non_ns_parent)

    return parent_vis


complete_headers = set[str]()
incomplete_headers = set[str]()


def make_vis_from(c: DecoratedCursor | None):
    if not c:
        return None
    return {
        "usr": c.normalized_usr,
        "display_name": fully_qualified(c),
        "kind": c.kind.name,
        "loc": pretty_location(c.location),
        "mod": mod_for_file(c.location.file),
    }


@dataclass
class Decl:
    display_name: str
    usr: str
    raw_usr: str
    # mangled_name: str
    loc: str
    kind: str
    mod: str | None
    linkage: str
    defined: bool
    spelling: str
    visibility: str
    alt: str
    vis_from: dict[str, str]
    vis_from_non_ns: dict[str, str]
    sem_par: str
    lex_par: str
    used_from: dict[str, set[str]] = dataclasses.field(default_factory=dict, compare=False)

    def def_or_decled(self) -> str:
        return "defined" if self.defined else "declared"

    @staticmethod
    def from_cursor(c: Cursor, mod=None):
        if not isinstance(c, DecoratedCursor):
            c = DecoratedCursor(c)
        vis = get_visibility(c)
        return Decl(
            display_name=fully_qualified(c),
            spelling=c.spelling,
            usr=c.normalized_usr,
            raw_usr=c.raw_usr,
            # mangled_name=c.mangled_name,
            loc=pretty_location(c.location),
            linkage=c.linkage.name,
            kind=c.kind.name,
            mod=mod or mod_for_file(c.location.file),
            defined=c.has_definition,
            visibility=vis.attr,
            alt=vis.alt,
            vis_from=make_vis_from(vis.parent),
            vis_from_non_ns=make_vis_from(vis.non_ns_parent),
            sem_par=c.normalized_parent.normalized_usr if c.normalized_parent else None,
            lex_par=(
                DecoratedCursor(c.lexical_parent).normalized_usr
                if not is_tu(c.lexical_parent)
                else None
            ),
        )


def pretty_location(loc: clang.SourceLocation | clang.Cursor):
    if isinstance(loc, Cursor):
        if loc.location.file:
            loc = loc.location
        else:
            # Clang bug: For some reason, usages of conversion operators lack a
            # location, but have an extent. Use the start of the extent instead.
            extent_start = loc.extent.start  # type: clang.SourceLocation
            loc = extent_start
    # NOTE: not using normpath_for_file() here because we don't want to convert
    # bazel-out/blah/src/mongo/beep to src/mongo/beep. All paths output by pretty_location
    # should be relative to the repo root. This is important for the browser to be able to
    # load the file. We still want to use os.path.normpath to fix up foo/bar/../baz to  foo/baz.
    name = os.path.normpath(loc.file.name) if loc.file else "<unknown>"
    # return f"{name}({loc.line},{loc.column})"  # MSVC format
    return f"{name}:{loc.line}:{loc.column}"  # gcc format


decls = dict[str, Decl]()


def fully_qualified(c: DecoratedCursor, kind: Literal["displayname", "spelling"] = "displayname"):
    parts = []
    for c in itertools.chain((c,), c.normalized_parents):
        spelling = getattr(c, kind)
        if spelling:
            if c.is_const_method():
                spelling += " const"
            match c.type.get_ref_qualifier():
                case RefQualifierKind.LVALUE:
                    spelling += " &"
                case RefQualifierKind.RVALUE:
                    spelling += " &&"
            parts.append(spelling)
    if not parts:
        return ""

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


function_kinds = {
    CursorKind.CONSTRUCTOR,
    CursorKind.CONVERSION_FUNCTION,
    CursorKind.CXX_METHOD,
    CursorKind.DESTRUCTOR,
    CursorKind.FUNCTION_DECL,
    CursorKind.FUNCTION_TEMPLATE,
}


def is_local_decl(c: Cursor):
    assert c.kind.is_declaration
    # Checking linkage first avoids doing expensive check for things we know can't be local.
    if c.linkage not in (LinkageKind.NO_LINKAGE, LinkageKind.INTERNAL):
        return False

    # Important: this skips over the input c itself, since we don't want to consider
    # functions as local decls, unless they are inside of another function.
    while (c := c.semantic_parent) and not is_tu(c):
        if c.kind in function_kinds:
            return True
    return False


context_kinds = (
    function_kinds
    | DecoratedCursor.TYPE_KINDS
    | {  # Type Aliases
        CursorKind.TYPE_ALIAS_DECL,
        CursorKind.TYPEDEF_DECL,
        CursorKind.TYPE_ALIAS_TEMPLATE_DECL,
    }
    | {  # Misc
        CursorKind.FIELD_DECL,  # Member variables
        CursorKind.UNEXPOSED_DECL,  # template variables
        CursorKind.CONCEPT_DECL,
    }
)

# These are only considered for context at namespace scope (when context is None)
namespace_scope_context_kinds = {
    CursorKind.VAR_DECL,
    CursorKind.STATIC_ASSERT,
}


def find_usages(mod: str, c: Cursor, context: DecoratedCursor | None):
    if c.kind == CursorKind.ANNOTATE_ATTR and c.spelling.startswith("mongo::mod::"):
        if not any(normpath_for_file(c) in s for s in (complete_headers, incomplete_headers)):
            perr_exit(
                f"{pretty_location(c)}:ERROR: usage of MONGO_MOD macro without directly including "
                + '"mongo/util/modules.h" or modules_incompletely_marked_header.h'
            )

    if c.kind in context_kinds or (context is None and c.kind in namespace_scope_context_kinds):
        context = DecoratedCursor(c)

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
        find_usages(mod, child, context)

    if ref is None or ref == c:
        return

    if ref.kind in (
        CursorKind.NAMESPACE,
        CursorKind.NAMESPACE_ALIAS,
        CursorKind.TEMPLATE_TEMPLATE_PARAMETER,
        CursorKind.TEMPLATE_TYPE_PARAMETER,
        CursorKind.TEMPLATE_NON_TYPE_PARAMETER,
        CursorKind.PARM_DECL,
        CursorKind.NO_DECL_FOUND,
    ):
        return

    if ref.kind == CursorKind.OVERLOADED_DECL_REF:
        # These come up when parsing a dependently-typed call. Unfortunately they
        # are not very useful, so they are one of many cases where we can't get
        # good info out of templates.
        assert not ref.get_usr()
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

    if not ref.canonical.location.file:
        # These are pre-declared in the compiler with no source location. In some cases,
        # they are redeclared in the stdlib, but canonicalization points them back
        # at the internal declaration. Make sure that this isn't causing us to skip
        # any first-party declarations.
        assert not ref.location.file or mod_for_file(ref.location.file) is None
        return

    if c.kind == CursorKind.CALL_EXPR and c.referenced.kind != CursorKind.CONSTRUCTOR:
        # For a call expression like a.b(c) or a::b(c) the whole thing is considered a reference of
        # a.b, but unfortunately the reported location is that of a, while we'd really want it to be
        # that of b. This was frequently resulting in two usage locations being reported for each
        # method call. Luckily, in most cases, the first child of the call expression (or one of its
        # transitive children) is the sub-expression a.b, which has a reference to b with the right
        # location. So we can safely ignore the call expression and rely on the post-order traversal
        # already adding a relevant used_from reference for this expression. The one exception is
        # that for constructor calls, the child refers to the *type* a::b, rather than the specific
        # constructor a::b::b() chosen, so we still need to add this location (even if not ideal) to
        # ensure that we mark the constructor's usage. I ran this with an assert to check that this
        # doesn't cause us to lose any usages, but it is too slow to keep (O(n^2) for n calls to a
        # method in a TU, causing some TUs to take several minutes).
        return

    if is_local_decl(ref):
        return

    # Unfortunately libclang's c api doesn't handle implicitly declared methods
    # well. In particular it often points at a location of a forward decl of the
    # class rather than the definition, even if both are visible. And then the
    # rest of our handling doesn't work correctly. And it also doesn't have a
    # way to distinguish implicit methods from explicitly defaulted ones. So we
    # just resolve all defaulted methods to the type and continue from there.
    if ref.is_default_method():
        ref = ref.semantic_parent

    if ref.spelling.startswith("__builtin_"):
        return

    # assert not c.location.file or mod_for_file(c.location.file) == mod
    ref = DecoratedCursor.normalize(ref)

    # Ignore any declarations not declared in a header.
    # TODO what if a local type is passed to a template? For now doesn't matter because we
    # don't look at usages from instantiations.
    if ref.location.file.name.endswith(".cpp") or ref.location.file.name.endswith(".cc"):
        return

    usr = ref.normalized_usr
    if not usr:
        return

    if usr in decls:
        # We've already done the work to get the info for this decl.
        d = decls[usr]
    else:
        decl_mod = mod_for_file(ref.location.file)
        if not decl_mod or decl_mod in skip_mods:
            return

        d = Decl.from_cursor(ref, decl_mod)
        decls[usr] = d

        if ref.definition and ref != ref.definition:
            def_mod = mod_for_file(ref.definition.location.file)
            # Note def_mod is None means third_party, not __NONE__ module
            if def_mod != decl_mod and def_mod is not None:
                print(f"WARNING: {d.display_name} is declared and defined in different modules")
                print(f"  decl: {pretty_location(ref)} ({decl_mod})")
                print(f"  defn: {pretty_location(ref.definition)} ({def_mod})")

    # ignore usages from the same module
    # if d.mod == mod or mod.startswith(d.mod):
    #     return

    # if this fails, something is missing in context_kinds or namespace_scope_context_kinds
    assert context

    usage = f"{pretty_location(c.location)} {context.string_for_context}"
    d.used_from.setdefault(mod, set()).add(usage)

    # Check that cross-module inheritance only uses "open" bases that explicitly chose to allow it.
    # For now we also allow "UNKNOWN" visibility, because that means that the base class hasn't been
    # marked for visibility. When the owner of that class gets to it, they see errors if other modules
    # inherit from it, and can then decide whether to mark it as open or not.
    # NOTE: the way that this check is implemented, it allows trivially bypassing by using something like
    # class Bad : public std::type_identity_t<ClosedBase> {}; but we are trying to catch accidental misuses,
    # not malicious ones. If we find this is a problem, we can probably improve it by asking clang to
    # resolve any typedefs. It won't help if the consumer is a template and the base is a dependent type,
    # but that is a general problem with the scanner.
    if c.kind == CursorKind.CXX_BASE_SPECIFIER and not (
        d.visibility in ("open", "UNKNOWN") or d.mod == mod or mod.startswith(d.mod)
    ):
        perr_exit(
            f"ERROR: {d.display_name} is used as a base class of {fully_qualified(context)}, "
            f"but they are in different modules. This is only allowed for classes marked MONGO_MOD_OPEN."
            f"\n    base: {d.loc} ({d.mod})"
            f"\n    child: {pretty_location(c)} ({mod})"
            f"\n    If you think that the base should be open, please contact the owner."
            # TODO extract the slack channel from the module metadata and add it here
        )


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
        "c_par_usr": str(node.semantic_parent.get_usr() if node.semantic_parent else None),
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

    for include in tu.get_includes():
        if "src/mongo" not in include.include.name:
            continue

        # Note: using bytes to avoid unicode handling overhead since the
        # needles we are looking for are ascii-only.
        content = Path(include.include.name).read_bytes()
        if b'"mongo/util/modules.h"' in content:
            complete_headers.add(normpath_for_file(include.include))
        elif b'"mongo/util/modules_incompletely_marked_header.h"' in content:
            incomplete_headers.add(normpath_for_file(include.include))
    timer.mark("checked header completeness")
    return tu


def dump_unused_inputs(outPath: str, tu: TranslationUnit):
    # only looking in src/mongo to cut down on resources, and to reduce the risk of accidentally
    # including some file we shouldn't. Assumption is that third_party and generated sources won't
    # change in a tight feedback loop.
    universe = set(glob("src/mongo/**/*.h", recursive=True))
    timer.mark("globbed")
    for include in tu.get_includes():
        if include.include:
            universe.discard(include.include.name)
    with open(outPath, "w") as file:
        file.write("\n".join(sorted(universe)))
    timer.mark("outfile written")


def main():
    args = sys.argv[1:]

    if len(args) == 0:
        perr_exit("invalid number of arguments")

    if len(args) == 1 and args[0].startswith("--"):
        perr_exit(f"{sys.argv[0]} doesn't support runtime options")

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
        find_usages(mod_for_file(top_level.location.file), top_level, None)
    timer.mark("found usages")

    out_file_name = out_from_env if out_from_env else "decls.yaml"
    if out_file_name.endswith(".zst"):
        uncompressed_file_name = out_file_name[: -len(".zst")]
        open_func = functools.partial(pyzstd.ZstdFile, write_size=2 * 1024 * 1024)
    else:
        uncompressed_file_name = out_file_name
        open_func = open

    with open_func(out_file_name, "w") as f:
        out = [dict(d.__dict__) for d in decls.values() if d.mod not in skip_mods]
        for decl in out:
            # del decl["spelling"]
            del decl["linkage"]
            del decl["raw_usr"]  # Can be helpful when debugging but not worth aggregating.
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
        if uncompressed_file_name.endswith(".json"):
            f.write(json.dumps(out).encode())
        else:
            assert out_file_name.endswith(".yaml")
            yaml.dump(out, f, Dumper=Dumper)
        timer.mark("dumped")


if __name__ == "__main__":
    main()

# cspell: words perr decled displayname cindex templ defn
