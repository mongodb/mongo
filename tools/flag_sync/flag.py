import io
import json
from typing import Optional

import boto3
import typer

from tools.flag_sync import util

app = typer.Typer()


@app.command()
def create(
    namespace: str,
    name: str,
    value: str,
    enabled: Optional[bool] = True,
    validate: Optional[bool] = True,
):
    if validate:
        if not util.validate_bazel_flag(value):
            print("Failed to create flag. Flag failed bazel verification.")
            return
    s3 = boto3.client("s3")
    flags = util.get_flags(namespace)
    flags[name] = {"value": value, "enabled": enabled}
    print(f"Created flag in namespace {namespace}:")
    print_flag(name, value, enabled)
    flags_json = json.dumps(flags)
    f = io.BytesIO(flags_json.encode("utf-8"))
    s3.upload_fileobj(f, util.S3_BUCKET, f"flag_sync/{namespace}.json")


@app.command()
def get(namespace: str, name: Optional[str] = None):
    flags = util.get_flags(namespace)
    for cur_name, flag in flags.items():
        if name and cur_name != name:
            continue
        print_flag(cur_name, flag["value"], flag["enabled"])


@app.command()
def update(
    namespace: str,
    name: str,
    value: Optional[str] = None,
    enabled: Optional[bool] = None,
    validate: Optional[bool] = True,
):
    if validate and value:
        if not util.validate_bazel_flag(value):
            print("Failed to update flag. Flag failed bazel verification.")
            return
    s3 = boto3.client("s3")
    flags = util.get_flags(namespace)
    if name not in flags:
        print(f"Flag with name {name} does not exist.")
        return
    if value:
        flags[name]["value"] = value
        print(f"Updated {name} to {value} in namespace {namespace}")
    if enabled is not None:
        flags[name]["enabled"] = enabled
        print(f"Updated enabled status of {name} to {enabled} in namespace {namespace}")
    print(f"Updated flag in namespace {namespace}:")
    print_flag(name, flags[name]["value"], flags[name]["enabled"])
    flags_json = json.dumps(flags)
    f = io.BytesIO(flags_json.encode("utf-8"))
    s3.upload_fileobj(f, util.S3_BUCKET, f"flag_sync/{namespace}.json")


@app.command()
def toggle_on(namespace: str, name: str):
    update(namespace=namespace, name=name, enabled=True)


@app.command()
def toggle_off(namespace: str, name: str):
    update(namespace=namespace, name=name, enabled=False)


@app.command()
def delete(namespace: str, name: str):
    s3 = boto3.client("s3")
    flags = util.get_flags(namespace)
    if name not in flags:
        print(f"Flag with name {name} does not exist.")
        return
    del flags[name]
    print(f"Deleted flag {name} in namespace {namespace}")
    flags_json = json.dumps(flags)
    f = io.BytesIO(flags_json.encode("utf-8"))
    s3.upload_fileobj(f, util.S3_BUCKET, f"flag_sync/{namespace}.json")


def print_flag(name: str, value: str, enabled: bool):
    print(f"{name}[{'+' if enabled else '-'}]: {value}")
