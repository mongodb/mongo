import json

import typer
from typing_extensions import Annotated


def main(
    input_compdb: Annotated[str, typer.Option()],
    output_compdb: Annotated[str, typer.Option()],
    bazel_compdb: str = "",
    ninja: bool = False,
):
    compdb_list = []
    bazel_compdb_list = []
    bazel_files = []
    compdb_files = []

    def print_dupes(target_list, file):
        seen = set()
        dupes = []

        for x in target_list:
            if x in seen:
                dupes.append(x)
            else:
                seen.add(x)
        print(f"ERROR, found duplicate entries for {file}:\n{dupes}")

    def find_output_file(arg_list):
        output_file = None
        for i, arg in enumerate(arg_list):
            if arg == "-o" or arg == "--output":
                output_file = arg_list[i + 1]
                break
            elif arg.startswith("/Fo") or arg.startswith("-Fo"):
                output_file = arg[3:]
                break
            elif arg.startswith("--output="):
                output_file = arg[9:]
                break
        if output_file is None:
            raise Exception(f"Failed to find output arg in {arg_list}")
        return output_file

    def fix_mongo_toolchain_path(arg_list):
        return

    with open(input_compdb) as f:
        compdb_list = json.load(f)
        compdb_files = [f"{entry['file']}->{entry['output']}" for entry in compdb_list]

    if ninja:
        for command in compdb_list:
            if command["output"].endswith(".compdb"):
                command["output"] = command["output"][: -(len(".compdb"))]
            else:
                print(f"compdb entry does not contain '.compdb': {command['output']}")

    if bazel_compdb:
        with open(bazel_compdb) as f:
            bazel_compdb_list = json.load(f)
            bazel_compdb_adjusted = []
            for entry in bazel_compdb_list:
                output_file = find_output_file(entry["arguments"])

                quoted_args = []
                for arg in entry["arguments"]:
                    if arg.startswith('"') and arg.endswith('"'):
                        quoted_args.append(arg)
                        continue
                    if arg.startswith("'") and arg.endswith("'"):
                        quoted_args.append(arg)
                        continue
                    if " " in arg:
                        arg = '"' + arg + '"'
                        quoted_args.append(arg)
                    else:
                        quoted_args.append(arg)

                new_entry = {
                    "file": entry["file"],
                    "command": " ".join(quoted_args),
                    "directory": entry["directory"],
                    "output": output_file,
                }
                bazel_compdb_adjusted.append(new_entry)
            bazel_files = [f"{entry['file']}->{entry['output']}" for entry in bazel_compdb_adjusted]
            bazel_compdb_list = bazel_compdb_adjusted

        try:
            assert len(bazel_files) == len(set(bazel_files))
        except AssertionError as exc:
            print_dupes(bazel_files, bazel_compdb)
            raise exc

        try:
            assert len(compdb_files) == len(set(compdb_files))
        except AssertionError as exc:
            print_dupes(compdb_files, input_compdb)
            raise exc

        try:
            assert not bool(set(bazel_files) & set(compdb_files))
        except AssertionError as exc:
            print_dupes(compdb_files + bazel_files, f"{input_compdb} + {bazel_compdb}")
            raise exc

    with open(output_compdb, "w") as f:
        json.dump(compdb_list + bazel_compdb_list, f, indent=2)


if __name__ == "__main__":
    typer.run(main)
