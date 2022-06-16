"""Script to run suite in Antithesis from the workload container."""
import subprocess
from time import sleep
import pymongo

client = pymongo.MongoClient(host="mongos", port=27017, serverSelectionTimeoutMS=30000)

while True:
    payload = client.admin.command({"listShards": 1})
    if len(payload["shards"]) == 2:
        print("Sharded Cluster available.")
        break
    if len(payload["shards"]) < 2:
        print("Waiting for shards to be added to cluster.")
        sleep(5)
        continue
    if len(payload["shards"]) > 2:
        raise RuntimeError('More shards in cluster than expected.')

subprocess.run([
    "./buildscripts/resmoke.py", "run", "--suite",
    "antithesis_concurrency_sharded_with_stepdowns_and_balancer"
], check=True)
