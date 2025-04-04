import argparse
import re

import yaml

parser = argparse.ArgumentParser()
parser.add_argument("--variant_name")
parser.add_argument("--task_name")
args = parser.parse_args()

with open("etc/evergreen_yml_components/configuration.yml") as f:
    yml = yaml.safe_load(f)
    default_timeout = yml["exec_timeout_secs"]

override_timeout = None
with open("etc/evergreen_timeouts.yml") as f:
    yml = yaml.safe_load(f)
    if args.variant_name in yml["overrides"]:
        for task in yml["overrides"][args.variant_name]:
            if re.search(task["task"], args.task_name):
                override_timeout = task["exec_timeout"] * 60
                break

with open("override_task_timeout.yml", "w") as f:
    if override_timeout:
        print(
            f"Overriding timeout for {args.variant_name}:{args.task_name} of {override_timeout} seconds."
        )
        f.write(f"override_task_timeout: {override_timeout}")
    else:
        print(
            f"Using default timeout for {args.variant_name}:{args.task_name} of {override_timeout} seconds."
        )
        f.write(f"override_task_timeout: {default_timeout}")
