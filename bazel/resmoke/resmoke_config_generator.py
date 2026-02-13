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
    group_size: Annotated[
        int | None,
        typer.Option(
            help="Number of tests to run in each group (for test_kind: parallel_fsm_workload_test)"
        ),
    ] = None,
    group_count_multiplier: Annotated[
        str,
        typer.Option(
            help="Multiplier for the number of groups (for test_kind: parallel_fsm_workload_test)"
        ),
    ] = "",
):
    with open(test_list, "rt") as fh:
        tests = [line.rstrip("\n") for line in fh]

    with open(base_config, "rt") as fh:
        base_config_content = yaml.safe_load(fh)
        if "selector" in base_config_content:
            for x in [
                "roots",
                "exclude_files",
                "exclude_with_any_tags",
                "include_with_any_tags",
                "group_size",
                "group_count_multiplier",
            ]:
                base_config_content["selector"].pop(x, None)

    # Validate that group_size and group_count_multiplier are only used with parallel_fsm_workload_test
    if group_size is not None or group_count_multiplier != "":
        test_kind = base_config_content.get("test_kind")
        if test_kind != "parallel_fsm_workload_test":
            raise ValueError(
                f"group_size and group_count_multiplier can only be used with test_kind: parallel_fsm_workload_test, "
                f"but this config has test_kind: {test_kind}"
            )

    content = base_config_content
    content["selector"] = {}
    content["selector"]["roots"] = tests

    if exclude_with_any_tags != "":
        content["selector"]["exclude_with_any_tags"] = exclude_with_any_tags.split(",")
    if include_with_any_tags != "":
        content["selector"]["include_with_any_tags"] = include_with_any_tags.split(",")
    if group_size is not None:
        content["selector"]["group_size"] = group_size
    if group_count_multiplier != "":
        content["selector"]["group_count_multiplier"] = float(group_count_multiplier)

    with open(output, "wt") as fh:
        yaml.dump(content, fh)


if __name__ == "__main__":
    typer.run(main)
