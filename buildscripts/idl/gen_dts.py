# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0
"""
Generate Typescript declaration files from IDL source files.

This generates three files:
 - src/mongo/shell/structs_gen.d.ts
 - src/mongo/shell/commands_gen.d.ts
 - src/mongo/shell/enums_gen.d.ts

These Typescript declarations will be picked up by code editors to provide
recursive type inference and autocomplete when writing jstests. Furthermore,
it will also present documentation of commands and their parameters. Note that
the documentation is not provided when hovering over a property, but during
autocomplete.

Example usage:

```js
const conn = new Mongo();
// ...
const res = conn.runCommand({ find: "foo", batchSize: 2 });
//                                         ^ documentation provided here
assert.eq(2, res.cursor.firstBatch.length);
//                                 ^ type inferred to be `number`
```

If your editor is unable to infer types (such as when declaring a function),
they can be manually provided via JSDOC type hints.

```js
/**
 * @param {Mongo} conn
 * @param {Commands["find"]["req"]} cmd
 * @returns {Commands["find"]["res"]} // (not necessary, will be inferred)
 */
function runFindCommand(conn, cmd) {
    return conn.adminCommand(cmd);
}

/** @param {Mongo} conn */
function findDocument(conn, collName, id) {
    return runFindCommand(conn, {
        find: collName,
        filter: {_id: id},
        includeQueryStatsMetrics: true,
    });
}

/**
 * @param {CursorMetrics} m
 */
function checkRuntime(m, threshold) {
    assert(m.cpuNanos < threshold);
}

checkRuntime(findDocument(conn, "foo", 42).cursor.metrics, 1000 * 1000);
```

"""

import contextlib
import io
import os
import shutil
import sys
from pathlib import Path
from types import FunctionType
from typing import Optional, TypedDict

# Permit imports from "buildscripts".
sys.path.append(os.path.normpath(os.path.join(os.path.abspath(__file__), "../../..")))

from buildscripts.idl.idl import common, parser, syntax
from buildscripts.idl.idl.compiler import CompilerImportResolver


class Declarations(TypedDict):
    structs: set[str]
    commands: set[str]
    enums: set[str]


def object_to_dts(obj: common.SourceLocation):
    def indented(text: str, indent: int):
        return text.replace("\n", "\n" + "\t" * indent)

    def field_is_optional(obj: syntax.Field) -> bool:
        return (
            obj.optional
            or (obj.default is not None)
            or (
                isinstance(obj.type, syntax.FieldTypeSingle)
                and obj.type.type_name.startswith("optional")
            )
        )

    if isinstance(obj, syntax.FieldType):
        if isinstance(obj, syntax.FieldTypeSingle):
            return obj.type_name
        if isinstance(obj, syntax.FieldTypeArray):
            return f"({object_to_dts(obj.element_type)})[]"
        if isinstance(obj, syntax.FieldTypeVariant):
            return "|".join(object_to_dts(t) for t in obj.variant)
        raise ValueError("Unexpected field type: " + type(obj).__name__)

    if isinstance(obj, syntax.Field):
        description = (" " + (obj.description).replace("\n", "\n * ")) if obj.description else ""
        out = f"\n/** (field){description} */\n"
        out += f"{obj.name}{'?' if field_is_optional(obj) else ''}: {object_to_dts(obj.type)};"
        return out

    if type(obj) == syntax.Struct:
        out = f"/** {obj.description} */\n" if obj.description else ""
        out += f"type {obj.name} = {{\n"
        fields = obj.fields or []
        out += "\n".join(indented(object_to_dts(f), 1) for f in fields) + "\n};"
        return out

    if type(obj) == syntax.Command:

        def get_description_with_fields():
            out = f"(command) {obj.description or ''}"
            if not obj.fields:
                return out
            out += "\n(Fields: "
            fields = " ".join(
                [
                    f"`{f.name}{'?' if field_is_optional(f) else ''}`"
                    for f in sorted(obj.fields, key=field_is_optional)
                ]
            )
            return out + fields + ")"

        description = (obj.description or "").replace("\n", "\n\t * ")
        description_with_fields = indented(get_description_with_fields(), 2)
        reply_type = obj.reply_type or "object"

        namespace = (
            1
            if obj.namespace == "ignored"
            else object_to_dts(obj.type)
            if obj.namespace == "type"
            else "NamespaceString"
        )

        out = "interface Commands {\n\n"
        out += f"\t/** {description} */\n" if description else ""
        out += f"\t{obj.name}: Command<{{\n\n"
        out += f"\t\t/** {description_with_fields} */\n"
        out += f"\t\t{obj.name}: {namespace};\n"
        out += "\n".join(indented(object_to_dts(f), 2) for f in (obj.fields or [])) + "\n\n"
        out += f"\t}}, {reply_type}>\n"
        out += "};"

        return out

    if isinstance(obj, syntax.Enum):
        return f"type {obj.name} = " + " | ".join(f'"{v.value}"' for v in obj.values) + ";"

    raise ValueError("Unexpected object type: " + type(obj).__name__)


def gen_dts_from_file(
    input_file: str, existing_decls: Optional[Declarations] = None
) -> dict[str, int]:
    if not os.path.exists(input_file):
        logging.error('File "%s" not found', args.input_file)
        return {}

    with io.open(input_file, encoding="utf-8") as f:
        parsed_doc = parser.parse(f, input_file, CompilerImportResolver(["src"]))

    return gen_dts(parsed_doc, existing_decls=existing_decls)


def gen_dts(
    parsed_doc: syntax.IDLParsedSpec,
    open_func: FunctionType = io.open,
    exists_func: FunctionType = os.path.exists,
    ignore_imported=False,
    existing_decls: Optional[Declarations] = None,
) -> dict[str, int]:
    if parsed_doc.errors:
        parsed_doc.errors.dump_errors()

    symbols = parsed_doc.spec.symbols
    declarations = existing_decls or {"structs": set(), "enums": set(), "commands": set()}
    base = "src/mongo/shell/%s_gen.d.ts"

    decl_counts = {key: 0 for key in declarations}

    for symbol_type in declarations:
        file_path = base % symbol_type

        # Create the _gen.d.ts file if it doesn't exist.
        if not exists_func(file_path):
            with open_func(file_path, "w") as file:
                file.write("")

    for symbol_type in declarations:
        for symbol in symbols.__getattribute__(symbol_type):
            # Skip any symbols that are already declared.
            if symbol.name in declarations[symbol_type]:
                continue

            if ignore_imported and symbol.imported:
                continue

            declarations[symbol_type].add(symbol.name)
            decl_counts[symbol_type] += 1
            # Create a Typescript definition from the symbol and write it to the file.
            text = object_to_dts(symbol)
            with open_func(base % symbol_type, "a") as f:
                f.write(text + "\n\n")

    return decl_counts


def gen_dts_files():
    COLS = shutil.get_terminal_size().columns
    MAX_FILENAME_LEN = COLS // 3

    def print_progress(file, additions: dict[str, int]) -> None:
        file = str(file)
        file = file if len(file) < MAX_FILENAME_LEN else f"{file[:MAX_FILENAME_LEN - 3]}..."
        info = f"Parsed {file}: ".ljust(MAX_FILENAME_LEN + len("Parsed  : ") + 1)
        info += " / ".join(f"{count:>3} {group}" for (group, count) in additions.items())
        info += " " * (COLS - len(info) - 5)
        print(info, end="\r")

    os.chdir(
        os.environ.get(
            "BUILD_WORKSPACE_DIRECTORY", (os.path.join(os.path.abspath(__file__), "../../../../"))
        )
    )

    # Remove the existing _gen.d.ts files.
    with contextlib.suppress(FileNotFoundError):
        os.remove("src/mongo/shell/structs_gen.d.ts")
        os.remove("src/mongo/shell/commands_gen.d.ts")
        os.remove("src/mongo/shell/enums_gen.d.ts")

    declarations = {"structs": set(), "enums": set(), "commands": set()}
    # Parse all IDL files in the src directory.
    for file in [*Path("src").rglob("*.idl")]:
        with open(file, "r", encoding="utf-8") as f:
            try:
                print_progress(file, gen_dts_from_file(file, declarations))
            except Exception as e:
                print(" " * COLS, end="\r")
                print(str(e))

    print(" " * COLS, end="\r")

    # Print completion message
    print("\ngen_dts complete. Files created:")
    for group in ("structs", "commands", "enums"):
        file = f"src/mongo/shell/{group}_gen.d.ts"
        byte_count = os.path.getsize(file)
        with open(file, "r", encoding="utf-8") as f:
            lines = f.readlines()
            line_count = len(lines)
        decl_count = len(declarations[group])
        print(
            f"{str(file):>35} | {int(byte_count / 1000):>4}KB | {line_count:>4} lines | {decl_count:>4} declarations"
        )


def main():
    """Run the main function."""
    os.chdir(os.environ.get("BUILD_WORKSPACE_DIRECTORY", "."))
    gen_dts_files()


if __name__ == "__main__":
    main()
