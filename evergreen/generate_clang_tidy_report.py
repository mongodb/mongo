import os
import re
import sys

from buildscripts.simple_report import make_report, put_report, try_combine_reports

failures = []

for root, _, files in os.walk("bazel-bin"):
    for name in files:
        if name.endswith(".clang-tidy.status"):
            with open(os.path.join(root, name)) as f:
                if f.read().strip() == "1":
                    tokens = name.split(".")
                    log_file = os.path.join(root, ".".join(tokens[:-1]) + ".log")
                    with open(log_file) as log:
                        content = log.read()

                    # exmaple
                    # bazel-bin/src/mongo/shell/bazel_clang_tidy_src/mongo/shell/mongo_main.cpp.mongo_main_with_debug.clang-tidy.status
                    # target is:
                    # mongo_main.cpp
                    source_file = ".".join(tokens[:-3])

                    failures.append(
                        [
                            os.path.join(
                                re.sub("^.*/bazel_clang_tidy_src/", "src/", root, 1), source_file
                            ),
                            content,
                        ]
                    )

for failure in failures:
    report = make_report(failure[0], failure[1], 1)
    try_combine_reports(report)
    put_report(report)
if failures:
    sys.exit(1)
