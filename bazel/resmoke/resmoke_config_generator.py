from pathlib import Path

import typer
import yaml
from typing_extensions import Annotated


def main(
    output: Annotated[Path, typer.Option()],
    test_list: Annotated[Path, typer.Option()],
    base_config: Annotated[Path, typer.Option()],
    exclude_with_any_tags: Annotated[str, typer.Option()] = "",
    include_with_any_tags: Annotated[str, typer.Option()] = "",
):
    with open(test_list, "rt") as fh:
        tests = [line.rstrip("\n") for line in fh]

    with open(base_config, "rt") as fh:
        base_config_content = yaml.safe_load(fh)
        if "selector" in base_config_content:
            for x in ["roots", "exclude_files", "exclude_with_any_tags", "include_with_any_tags"]:
                base_config_content["selector"].pop(x, None)

    content = base_config_content
    if "selector" not in content:
        content["selector"] = {}
    content["selector"]["roots"] = tests

    if exclude_with_any_tags != "":
        content["selector"]["exclude_with_any_tags"] = exclude_with_any_tags.split(",")
    if include_with_any_tags != "":
        content["selector"]["include_with_any_tags"] = include_with_any_tags.split(",")

    with open(output, "wt") as fh:
        yaml.dump(content, fh)


if __name__ == "__main__":
    typer.run(main)
