"""
Prints a single line cotaining the historic average test runtimes for a Evergreen task:
STABLE_TEST_RUNTIMES [{"test_name": "1.js", "avg_duration_pass": 2.3}, {"test_name": "2.js", "avg_duration_pass": 1.0}, ...

This format is understood by bazel as a "stable" status key for the --workspace_status_command.
https://bazel.build/docs/user-manual#workspace-status-command

Usage:
python get_historic_runtimes.py --project=mongodb-mongo-master --task=my_task_0-linux --build-variant=my_variant
"""

import io
import json
import re

import boto3
import botocore
import typer
from typing_extensions import Annotated

STATS_BUCKET = "mongo-test-stats"

app = typer.Typer()


@app.command()
def main(
    project: Annotated[str, typer.Option()],
    build_variant: Annotated[str, typer.Option()],
    task: Annotated[str, typer.Option()],
):
    if not project or not build_variant or not task:
        return

    # Extracts the base task name, i.e. auth_0-linux_enterprise -> auth
    match = re.search(r"(.+)_\d+-(linux|mac|windows)*", task)
    if match:
        task = match.group(1)

    key = "/".join([project, build_variant, task])

    buffer = io.BytesIO()
    s3 = boto3.client("s3", config=botocore.client.Config(signature_version=botocore.UNSIGNED))
    try:
        s3.download_fileobj(STATS_BUCKET, key, buffer)
    except botocore.exceptions.ClientError as e:
        if e.response["Error"]["Code"] == "404" or e.response["Error"]["Code"] == "403":
            # If there are no historic test stats for this task, return.
            return
        raise

    buffer.seek(0)

    stats = json.load(buffer)
    for s in stats:
        for field in ["num_pass", "num_fail", "max_duration_pass"]:
            s.pop(field, None)

    # The STABLE_ prefix is required for the key to represent a "stable" key.
    print(f"STABLE_TEST_RUNTIMES {json.dumps(stats)}")


if __name__ == "__main__":
    app()
