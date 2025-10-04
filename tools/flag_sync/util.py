import json
import os
import subprocess
import tempfile

import requests

S3_BUCKET = "mdb-build-public"


def get_flags(namespace: str):
    S3_URL = f"https://mdb-build-public.s3.us-east-1.amazonaws.com/flag_sync/{namespace}.json"
    r = requests.get(S3_URL)
    if r.status_code != 200:
        raise Exception("Namespace doesn't exist.")
    try:
        return json.loads(r.text)
    except:
        raise Exception("Couldn't parse flag json.")


def validate_bazel_flag(line: str):
    workspace_root = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    with tempfile.NamedTemporaryFile(mode="w+") as tf:
        tf.write(line)
        tf.flush()
        cmd = ["bazel", f"--bazelrc={tf.name}", "build", "//tools/flag_sync:client"]
        p = subprocess.run(cmd, capture_output=True, cwd=workspace_root)
        if p.returncode != 0:
            return False
    return True
