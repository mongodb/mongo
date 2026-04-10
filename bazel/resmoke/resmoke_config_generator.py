from pathlib import Path

import typer
import yaml
from typing_extensions import Annotated


def main(
    output: Annotated[Path, typer.Option()],
    test_list: Annotated[Path, typer.Option()],
    base_config: Annotated[Path, typer.Option()],
):
    with open(test_list, "rt") as fh:
        tests = [line.rstrip("\n") for line in fh if line.strip()]

    with open(base_config, "rt") as fh:
        content = yaml.safe_load(fh)

    # Replace only roots (and from_target) with the resolved file list.
    # All other selector fields (exclude_files, exclude_with_any_tags, etc.)
    # are preserved from the original YAML.
    if "selector" not in content:
        content["selector"] = {}
    content["selector"].pop("roots", None)
    content["selector"]["roots"] = tests

    with open(output, "wt") as fh:
        yaml.dump(content, fh)


app = typer.Typer(pretty_exceptions_show_locals=False)
app.command()(main)

if __name__ == "__main__":
    app()
