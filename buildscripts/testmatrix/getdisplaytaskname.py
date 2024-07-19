"""
Get the display task name from the execution task and the variant.

Get an execution task name like this: multiversion_auth_0_enterprise-rhel-8-64-bit-dynamic-all-feature-flags-required
Into a display task name like this: multiversion_auth
"""

import sys

task_name = sys.argv[1]
variant_name = sys.argv[2]

task_name = task_name.replace("_" + variant_name, "")

if task_name[-1].isdigit() or task_name.endswith("_misc"):
    # Is a generated task.
    task_name = task_name.rsplit("_", maxsplit=1)[0]

print(task_name)
