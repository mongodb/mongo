import os
import re
import sys

from buildscripts.simple_report import make_report, put_report, try_combine_reports
from buildscripts.util.read_config import read_config_file

expansions = read_config_file("../expansions.yml")
clang_tidy = expansions.get("run_for_clang_tidy", None)

if clang_tidy:
    failures = []
    for root, _, files in os.walk("bazel-bin"):
        for name in files:
            if name.endswith(".clang-tidy.status") and "mongo_tidy_checks/tests/" not in root:
                with open(os.path.join(root, name)) as f:
                    if f.read().strip() == "1":
                        tokens = name.split(".")
                        log_file = os.path.join(root, ".".join(tokens[:-1]) + ".log")
                        with open(log_file) as log:
                            content = log.read()

                        # exmaple
                        # bazel-bin/src/mongo/shell/bazel_clang_tidy_src/mongo/shell/mongo_main.cpp.mongo_main_with_debug.clang-tidy.status
                        # source_file is:
                        # mongo_main.cpp
                        # target is mongo_main_with_debug
                        filename = os.path.basename(log_file)
                        parts = filename.split(".")
                        if len(parts) < 5:
                            raise ValueError(f"Unexpected status file format: {filename}")
                        source_file = ".".join(parts[:2])
                        target_name = parts[2]
                        label_path = re.sub(r"/bazel_clang_tidy_src/.*$", "", log_file)
                        label_path = re.sub(r"^.*bazel-bin/", "//", label_path)
                        label_path = os.path.dirname(label_path)
                        target = f"{label_path}:{target_name}"
                        content += f"Run 'bazel build --config=clang-tidy --keep_going {target}' to reproduce this error"

                        failures.append(
                            [
                                os.path.join(
                                    re.sub("^.*/bazel_clang_tidy_src/", "src/", root, 1),
                                    source_file,
                                ),
                                content,
                            ]
                        )

    with open("bazel-invocation.txt", "w") as f:
        f.write("bazel build --config=clang-tidy //src/mongo/...")

    if failures:
        for failure in failures:
            report = make_report(failure[0], failure[1], 1)
            try_combine_reports(report)
            put_report(report)
        sys.exit(1)
    else:
        report = make_report("bazel build --config=clang-tidy //src/mongo/...", "", 0)
        try_combine_reports(report)
        put_report(report)
