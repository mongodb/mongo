#!/usr/bin/env python3
"""Validate etc/coverity.yml against the bundled Coverity configuration schema."""

import json
import os
import sys

import jsonschema
import yaml

CONFIG_PATH = "etc/coverity.yml"
SCHEMA_PATH = "etc/coverity_config_schema.json"

# commit.connect.url is required by the schema but intentionally absent from the
# static config — it is injected at runtime by the devprod_coverity module.
_EXPECTED_ERRORS = {("commit", "connect"): "url"}


def main() -> int:
    if "BUILD_WORKING_DIRECTORY" in os.environ:
        os.chdir(os.environ["BUILD_WORKING_DIRECTORY"])

    with open(CONFIG_PATH) as f:
        config = yaml.safe_load(f)
    with open(SCHEMA_PATH) as f:
        schema = json.load(f)

    errors = [
        e
        for e in jsonschema.Draft202012Validator(schema).iter_errors(config)
        if not (
            tuple(e.absolute_path) in _EXPECTED_ERRORS
            and _EXPECTED_ERRORS[tuple(e.absolute_path)] in e.message
        )
    ]

    if errors:
        for e in errors:
            print(f"{CONFIG_PATH}: schema error at {list(e.absolute_path)}: {e.message}")
        return 1

    print(f"{CONFIG_PATH} is valid.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
