#!/usr/bin/env python3
import json
import os
import subprocess

import pymongo
import typer
import yaml
from bson.datetime_ms import DatetimeMS

try:
    from yaml import CLoader as Loader
except ImportError:
    raise RuntimeError("Why no cYaml?")
    # from yaml import Loader, Dumper


def main(
    uri: str,
    dropColls: bool = typer.Option(False, help="drop collections before inserts"),
    dropTs: bool = typer.Option(False, help="drop git commit base timestamps before inserts"),
):
    if dropColls:
        print("--dropColls was specified. This will drop the collections before inserting.")

    if dropTs:
        print(
            "--dropTs was specified. This will delete all documents at the commit base timestamp before inserting."
        )

    if (dropColls or dropTs) and (input("Type Y to confirm: ").lower().strip() not in ("y", "yes")):
        print("aborting")
        raise typer.Exit(1)

    try:
        print("Connecting to mongodb...")
        conn: pymongo.MongoClient = pymongo.MongoClient(uri)
        conn["test"].command("ping")  # test connection early
    except Exception as e:
        print("Failed to connect to mongo. Check the ip whitelist!")
        url = "wtfismyip.com/text"
        print(f"Attempting to determine your public IP from {url}:")
        os.system(f"curl -4 {url}")
        print()
        print("Detailed error from mongo:", e)
        raise typer.Exit(1)

    commit_date_str = subprocess.check_output(
        "git show --no-patch --format=%ct $(git merge-base HEAD origin/master)",
        shell=True,
    )
    commit_date = DatetimeMS(1000 * int(commit_date_str))
    print(f"commit date: {commit_date.as_datetime()}")

    def timestamp(decls):
        for d in decls:
            d["ts"] = commit_date

    with open("merged_decls.json") as f:
        decls = json.load(f)
        timestamp(decls)

    if dropColls:
        conn["modules"]["decls"].drop()
    elif dropTs:
        conn["modules"]["decls"].delete_many({"ts": commit_date})
    conn["modules"]["decls"].insert_many(decls)
    print("uploaded decls")

    with open("unowned.yaml") as f:
        decls = yaml.load(f, Loader=Loader)
        timestamp(decls)

    if dropColls:
        conn["modules"]["unowned"].drop()
    elif dropTs:
        conn["modules"]["unowned"].delete_many({"ts": commit_date})
    conn["modules"]["unowned"].insert_many(decls)
    print("uploaded unowned")


if __name__ == "__main__":
    typer.run(main)
