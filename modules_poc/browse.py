#!/usr/bin/env python3
import dataclasses
import json
import os
import shutil
import sys
from dataclasses import dataclass
from functools import cached_property, partial
from pathlib import Path
from typing import Any, NamedTuple, Protocol

import tree_sitter
import tree_sitter_cpp
from rich.text import Text
from textual.app import App, ComposeResult
from textual.binding import Binding, BindingType
from textual.containers import Horizontal
from textual.reactive import reactive
from textual.widgets import Footer, TextArea, Tree
from textual.widgets.text_area import Selection, TextAreaTheme
from textual.widgets.tree import TreeNode

cpp_language = tree_sitter.Language(tree_sitter_cpp.language())
cpp_highlight_query = (Path(__file__).parent / "cpp-highlights.scm").read_text()


class Loc(NamedTuple):
    file: str
    line: int
    col: int

    @classmethod
    def parse(cls, loc: str):
        p, l, c = loc.split(":")
        return cls(sys.intern(p), int(l), int(c))

    @property
    def loc(self):
        """for compatibility with LocAndContext.loc"""
        return self

    def __str__(self):
        return f"{self.file}:{self.line}:{self.col}"


class LocAndContext(NamedTuple):
    loc: Loc
    ctx: str

    @classmethod
    def parse(cls, usage: str):
        loc, _, ctx = usage.partition(" ")
        return cls(Loc.parse(loc), sys.intern(ctx))


class HasUnknownCount(Protocol):
    @property
    def unknown_count(self) -> int: ...


def unknown_count(arg: HasUnknownCount | list[HasUnknownCount]):
    if isinstance(arg, list):
        return sum(map(unknown_count, arg))
    return arg.unknown_count


Usages = dict[str, set[LocAndContext | Loc]]


def fancy_kind(kind: str):
    def style(name):
        return TextAreaTheme.get_builtin_theme("css").get_highlight(name)

    match kind:
        case "CLASS_DECL":
            return Text.assemble(("class", style("class")))
        case "CLASS_TEMPLATE":
            return Text.assemble(("template", style("keyword")), " ", ("class", style("class")))
        case "CLASS_TEMPLATE_PARTIAL_SPECIALIZATION":
            return Text.assemble(("template<>", style("keyword")), " ", ("class", style("class")))
        case "CONCEPT_DECL":
            return Text.assemble(("concept", style("class")))
        case "CONSTRUCTOR":
            return Text.assemble(("ctor", style("type")))
        case "CONVERSION_FUNCTION":
            return Text.assemble(("conversion", style("type")))
        case "CXX_METHOD":
            return Text.assemble(("method", style("type")))
        case "DESTRUCTOR":
            return Text.assemble(("dtor", style("type")))
        case "ENUM_CONSTANT_DECL":
            return Text.assemble(("enumerator", style("number")))
        case "ENUM_DECL":
            return Text.assemble(("enum", style("class")))
        case "FIELD_DECL":
            return Text.assemble(("mem", style("css.property")))
        case "FUNCTION_DECL":
            return Text.assemble(("func", style("type")))
        case "FUNCTION_TEMPLATE":
            return Text.assemble(("template", style("keyword")), " ", ("func", style("type")))
        case "STATIC_ASSERT":
            return Text.assemble(("static_assert", style("keyword")))
        case "STRUCT_DECL":
            return Text.assemble(("struct", style("class")))
        case "TYPEDEF_DECL":
            return Text.assemble(("typedef", style("class")))
        case "TYPE_ALIAS_DECL":
            return Text.assemble(("typedef", style("class")))
        case "TYPE_ALIAS_TEMPLATE_DECL":
            return Text.assemble(("template", style("keyword")), " ", ("typedef", style("class")))
        case "UNEXPOSED_DECL":
            # This seems to be what these show up as in libclang :(
            return Text.assemble(
                ("template", style("keyword")), " ", ("var", style("css.property"))
            )
        case "VAR_DECL":
            return Text.assemble(("var", style("css.property")))
        case _:
            return Text.assemble((kind, style("info_string")))


@dataclass
class Decl:
    display_name: str
    usr: str
    # mangled_name: str
    loc: Loc
    kind: str
    mod: str | None
    defined: bool
    spelling: str
    visibility: str
    alt: str
    sem_par: str
    lex_par: str
    _raw_used_from: Any  # use direct_usages instead
    sem_children: list["Decl"] = dataclasses.field(default_factory=list, compare=False, repr=False)
    lex_children: list["Decl"] = dataclasses.field(default_factory=list, compare=False, repr=False)
    other_mods: Usages = None

    @cached_property
    def unknown_count(self):
        return (1 if self.visibility == "UNKNOWN" else 0) + unknown_count(self.sem_children)

    @cached_property
    def direct_usages(self) -> Usages:
        return {u["mod"]: set(map(LocAndContext.parse, u["locs"])) for u in self._raw_used_from}

    @cached_property
    def transitive_usages(self) -> Usages:
        out: Usages = Usages()
        for mod, locs in self.direct_usages.items():
            out.setdefault(mod, set()).update(locs)
        for child in self.sem_children:
            for mod, locs in child.transitive_usages.items():
                out.setdefault(mod, set()).update(locs)
        return out

    @property
    def fancy_kind(self):
        return fancy_kind(self.kind)

    @property
    def fancy_visibility(self):
        match self.visibility:
            case "public":
                return Text("pub", "green")
            case "private":
                return Text("priv", "yellow3")
            case "file_private":
                return Text("file_priv", "yellow3")
            case "needs_replacement":
                return Text("needs_repl", "orange1")
            case "use_replacement":
                # Not showing self.alt here
                return Text("use_repl", "orange1")
            case "UNKNOWN":
                return Text("¿vis?", "red")
            case _:
                raise ValueError(f"unexpected visibility: {self.visibility}")


@dataclass
class File:
    name: str
    mod: str
    top_level_decls: list[Decl] = dataclasses.field(default_factory=list, compare=False)
    detached_decls: list[Decl] = dataclasses.field(default_factory=list, compare=False)
    all_decls: list[Decl] = dataclasses.field(default_factory=list, compare=False)

    @cached_property
    def unknown_count(self):
        return unknown_count(self.top_level_decls) + unknown_count(self.detached_decls)


def add_decl_node(node: TreeNode, d: Decl):
    # Highlight the "main" part of the name.
    # Assume that the last instance of the name is the main one.
    # TODO: if this is slow, consider moving to a render_label() override
    label = f"[bold bright_white]{d.spelling}[/]".join(d.display_name.rsplit(d.spelling, 1))
    label += f" [i]unknowns:[/]{unknown_count(d)}"
    label += f" [i]usages:[/]{sum(len(u) for u in d.transitive_usages.values())}"
    if d.sem_children:
        label += f" [i]children:[/]{len(d.sem_children)}"
    if d.lex_children:
        label += f" [i]lex_children:[/]{len(d.lex_children)}"
    node.add(Text.assemble(d.fancy_visibility, " ", d.fancy_kind, " ", Text.from_markup(label)), d)


def add_decl_nodes(node: TreeNode, ds: list[Decl]):
    for d in ds:
        add_decl_node(node, d)


def add_mod_loc_mapping_nodes(node: TreeNode, usages: Usages, kind: str, expand=False):
    node = node.add(f"[i]{kind}:[/] ", expand=expand)
    tot = 0
    for mod, locs in sorted(usages.items()):
        tot += len(locs)
        mod_node = node.add(f"{mod}: {len(locs)}", expand=expand)
        for loc in sorted(locs):
            if isinstance(loc, Loc):
                mod_node.add_leaf(Text(str(loc.loc)), loc.loc)
                continue

            # Currently only STATIC_ASSERT doesn't have a name.
            kind, _, name = loc.ctx.partition(" ")
            mod_node.add_leaf(
                Text.assemble(
                    fancy_kind(kind),
                    Text(" " + name, style="bold bright_white") if name else "",
                    f" {loc.loc}",
                ),
                loc.loc,
            )
    node.label += str(tot)


VIM_BINDINGS: list[BindingType] = [
    Binding("down,j", "cursor_down", "Down", show=False),
    Binding("up,k", "cursor_up", "Up", show=False),
    Binding("left,h", "cursor_left", "Left", show=False),
    Binding("right,l", "cursor_right", "Right", show=False),
]


class CodePreview(TextArea):
    BINDINGS = VIM_BINDINGS

    loc: reactive[Loc | None] = reactive(None)

    def __init__(self):
        super().__init__(read_only=True, show_line_numbers=True)
        self.register_language("cpp", cpp_language, cpp_highlight_query)
        self.language = "cpp"
        self.loc = self.app.query_exactly_one(FilesTree).loc

    def watch_loc(self, old: Loc | None, new: Loc | None):
        print("preview new loc", new)
        if new is None:
            self.clear()
            return

        if old is None or old.file != new.file:
            if not os.path.exists(new.file):
                self.notify(f"cannot open file '{new.file}'")
                self.loc = None
                return

            with open(new.file) as file:
                self.border_title = f"[blue]{new.file}/[/]"
                self.load_text(file.read())

        start = (new.line - 1, new.col - 1)  # 0-indexed :(
        self.move_cursor(start)

        rest_of_line = self.get_text_range(start, (start[0] + 1, 0))
        i = 0
        for i, c in enumerate(rest_of_line):
            if not (c.isalnum() or c == "_"):
                break
        self.selection = Selection(start, (start[0], start[1] + i))
        self.scroll_nicely()

    def _on_resize(self):
        super()._on_resize()  # rewraps, so must run first!
        self.scroll_nicely()

    def scroll_nicely(self):
        if not self.loc:
            return

        # Move selection to top, but show some context above, but no more than 20% of screen.
        # As a complication, cursor location is in unwrapped document lines, but scroll-targets are
        # in wrapped screen lines (aka offset). Compute context in screen space rather than document
        # space.
        offset = self.wrapped_document.location_to_offset(self.cursor_location)
        context = min(self.size.height // 5, 4)
        target = max(offset.y - context, 0)
        self.scroll_to(x=0, y=target, animate=False)


class FilesTree(Tree):
    BINDINGS = VIM_BINDINGS + [
        ("m", "mod_select", "Filter by module"),
        ("g", "goto", "Go to declaration"),
        ("f,/", "find_file", "Search for a file"),
        Binding(
            "ctrl+space",  # default uses shift+space, but no term supports that
            "toggle_expand_all",
            "Expand or collapse all",
            show=False,
        ),
    ]

    files = reactive(list[File]())
    loc: reactive[Loc | None] = reactive(None)

    def __init__(self, files: dict[str, File]):
        super().__init__(label="files")
        self.show_root = False
        self.all_files = list(files.values())
        self.files = self.all_files

    def watch_files(self, old: list[File], new: list[File]):
        print(len(new))
        print(len(old))
        if len(old) == len(new) and all(id(old[i]) == id(new[i]) for i in range(len(old))):
            return

        self.clear()
        for file in self.files:
            path = Path(file.name)
            self.root.add(
                label=f":page_facing_up: [gray]{path.parent}[/]/[bold bright_white]{path.name}[/] [i]mod:[/]{file.mod} [i]unknowns:[/]{unknown_count(file)}",
                data=file,
            )

    def watch_loc(self, new: Loc | None):
        print("new loc", new)
        for preview in self.app.query(CodePreview):
            print("found preview")
            preview.loc = new

    def on_tree_node_highlighted(self, event: Tree.NodeHighlighted):
        node = event.node
        while node:
            if type(node.data) == Loc:
                self.loc = node.data
                return
            if type(node.data) == Decl or type(node.data) == LocAndContext:
                self.loc = node.data.loc
                return
            node = node.parent

    def action_goto(self):
        if self.loc:
            if "VSCODE_IPC_HOOK_CLI" in os.environ:
                if not shutil.which("code"):
                    return self.app.notify(
                        "'code' command not found. Please check your $PATH.", severity="error"
                    )
                os.system(f"code -g '{self.loc}'")
            elif "NVIM" in os.environ:
                if not shutil.which("nvim"):
                    return self.app.notify(
                        "'nvim' command not found. Please check your $PATH.", severity="error"
                    )
                nvim = f"nvim --headless --server '{os.environ['NVIM']}'"
                seek = f'<cmd>call setpos(".", [0,{self.loc.line},{self.loc.col},0])<cr>'
                os.system(
                    "("
                    + f"{nvim} --remote '{self.loc.file}' && "
                    + f"{nvim} --remote-send '{seek}'"
                    + ") 2> /dev/null > /dev/null < /dev/null"  # don't let nvim touch the terminal
                )
            else:
                self.app.notify("GoTo only works inside VSCode or nvim terminal")

    def action_mod_select(self):
        def handle_selection(mod):
            print(mod)
            if not mod:
                return

            if mod == "ALL":
                self.files = self.all_files
            else:
                self.files = [f for f in self.all_files if f.mod == mod]

        self.app.search_commands(
            placeholder="Module:",
            commands=[(mod, partial(handle_selection, mod)) for mod in ["ALL"] + sorted(modules)],
        )

    def on_tree_node_expanded(self, event: Tree.NodeExpanded):
        node = event.node
        if node.children:
            return

        if type(node.data) == File:
            return self.fill_file_node(node)
        if type(node.data) == Decl:
            return self.fill_decl_node(node)
        raise ValueError(f"unexpected data of type {type(node.data)}")

    def fill_file_node(self, node: TreeNode[File]):
        file = node.data
        if file.top_level_decls:
            add_decl_nodes(
                node.add(f"top-level decls ({len(file.top_level_decls)})", expand=True),
                file.top_level_decls,
            )
        if file.detached_decls:
            add_decl_nodes(
                node.add("detached decls ({len(file.detached_decls)})", expand=True),
                file.detached_decls,
            )
        # Show flat lists of all decls grouped by visibility.
        vis_map = dict[str, list[Decl]]()
        for decl in file.all_decls:
            vis_map.setdefault(decl.visibility, []).append(decl)
        for vis, decls in sorted(vis_map.items(), key=lambda kv: len(kv[1]), reverse=True):
            add_decl_nodes(
                node.add(f"all {vis} decls ({len(decls)})", expand=False),
                decls,
            )

    def fill_decl_node(self, node: TreeNode[Decl]):
        d = node.data
        node.add_leaf(f"[i]loc:[/] {d.loc}")
        node.add_leaf(f"[i]usr:[/] {d.usr}")  # TODO: hide unless in "developer mode"
        if d.alt:
            node.add_leaf(f"[i]replacement:[/] {d.alt}")

        if d.other_mods:
            add_mod_loc_mapping_nodes(node, d.other_mods, "declared in other_mods", expand=True)

        add_mod_loc_mapping_nodes(node, d.direct_usages, "direct usages")

        if d.sem_children:
            add_mod_loc_mapping_nodes(node, d.transitive_usages, "direct and transitive usages")
            add_decl_nodes(
                node.add("semantic children"),
                d.sem_children,
            )

        if d.lex_children:
            add_decl_nodes(
                node.add("lexical but [b]not[/] semantic children"),
                d.lex_children,
            )

    def action_find_file(self):
        files = self.files
        tree = self

        def seek(file):
            for row in tree.root.children:
                if row.data.name == file.name:
                    tree.center_scroll = True
                    row.expand()
                    tree.move_cursor(row, animate=True)
                    tree.center_scroll = False
                    break

        self.app.search_commands(
            commands=[(f.name, partial(seek, f)) for f in files],
            placeholder="Search for files...",
        )


class ModularityApp(App):
    BINDINGS = [
        ("q", "quit", "Quit"),
        ("?", "toggle_help", "Toggle Help"),
        ("p", "toggle_preview", "Toggle Code Preview"),
    ]

    # def __init__(self, decls: list[Decl]):
    #     self.decls = decls
    #
    def on_mount(self):
        self.action_show_help_panel()

    def compose(self) -> ComposeResult:
        """Create child widgets for the app."""
        # yield Header()
        yield Footer()
        yield Horizontal(FilesTree(files))

    def action_toggle_help(self):
        if self.query("HelpPanel"):
            self.action_hide_help_panel()
        else:
            self.action_show_help_panel()

    def action_toggle_preview(self):
        if self.query("CodePreview"):
            self.query_exactly_one(CodePreview).remove()
        else:
            self.query_exactly_one(Horizontal).mount(CodePreview())


input = "merged_decls.json"
if len(sys.argv) > 1:
    input = sys.argv[1]
with open(input, "rb") as file:
    raw_decls = json.load(file)

for d in raw_decls:
    d["loc"] = Loc.parse(d["loc"])
    d["_raw_used_from"] = d["used_from"]
    del d["used_from"]
    if "other_mods" in d:
        for mod, locs in d["other_mods"].items():
            locs.sort()
            d["other_mods"][mod] = [Loc.parse(loc) for loc in locs]
    # For now these aren't used in the browser
    del d["vis_from"]
    del d["vis_from_non_ns"]

decls = sorted((Decl(**d) for d in raw_decls), key=lambda d: d.loc)
del raw_decls

decl_ix = {d.usr: d for d in decls}
files = dict[str, File]()


def getFile(d: Decl):
    name = d.loc.file
    if name in files:
        return files[name]
    else:
        file = File(name, d.mod)
        files[name] = file
        return file


top_level_usrs = {d.usr for d in decls}
for d in decls:
    getFile(d).all_decls.append(d)
    if d.sem_par in decl_ix:
        decl_ix[d.sem_par].sem_children.append(d)
        top_level_usrs.remove(d.usr)

        if decl_ix[d.sem_par].loc.file != d.loc.file:
            getFile(d).detached_decls.append(d)
            if decl_ix[d.sem_par].mod != d.mod:
                print(
                    f"warning: {d.display_name} defined in {d.mod}, but parent is in {decl_ix[d.sem_par].mod}"
                )

    if d.lex_par != d.sem_par and d.lex_par in decl_ix:
        decl_ix[d.lex_par].lex_children.append(d)
        # top_level_usrs.remove(d.usr)
        assert decl_ix[d.lex_par].loc.file == d.loc.file

for u in top_level_usrs:
    d = decl_ix[u]
    getFile(d).top_level_decls.append(d)

for f in files.values():
    f.all_decls.sort(key=lambda d: d.loc)
    f.top_level_decls.sort(key=lambda d: d.loc)
    f.detached_decls.sort(key=lambda d: d.loc)

files = {k: v for k, v in sorted(files.items(), key=lambda kv: kv[1].unknown_count, reverse=True)}
modules = {d.mod for d in decls}

if __name__ == "__main__" and "--parse-only" not in sys.argv:
    app = ModularityApp()
    app.run()

    # Don't waste time running GC on exit
    os._exit(0)

# cSpell:words usrs
