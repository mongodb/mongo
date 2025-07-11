import io

import boto3
import typer

from tools.flag_sync import util

app = typer.Typer()


@app.command()
def create(namespace: str):
    s3 = boto3.client("s3")
    f = io.BytesIO(b"{}")
    s3.upload_fileobj(f, util.S3_BUCKET, f"flag_sync/{namespace}.json")
    print(f"Created namespace {namespace}")


@app.command()
def list():
    s3 = boto3.client("s3")
    res = s3.list_objects_v2(Bucket=util.S3_BUCKET, Prefix="flag_sync/")
    print("====Namespaces====")
    for f in res["Contents"]:
        print(f["Key"].replace(".json", "").split("/")[-1])
