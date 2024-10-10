#!/usr/bin/env python3

import argparse
import copy
import json
import sys
import textwrap

from profilerlib import CallMetrics


def print_folded_visit(metrics, span_id: int, prefix: str):
    span = metrics.spans[span_id]
    if prefix is not None:
        print(f"{prefix} {span.exclusive_nanos}")

    for name, child_id in span.children.items():
        new_prefix = prefix + ";" + name if prefix is not None else name
        print_folded_visit(metrics, child_id, new_prefix)


def print_folded(metrics):
    # Print entire folded tree under the root
    print_folded_visit(metrics, 0, None)


def print_tsv(metrics):
    def print_tsv_line(arr):
        print("\t".join([str(x) for x in arr]))

    print_tsv_line(["id", "name", "parentId", "totalNanos", "netNanos", "exclusive_nanos", "count"])
    for s in metrics.spans.values():
        # Skip root span
        if s.id == 0:
            continue
        print_tsv_line(
            [s.id, s.name, s.parent_id, s.total_nanos, s.net_nanos, s.exclusive_nanos, s.count]
        )


def remove_empty_spans(metrics):
    # Keep only metrics that have non zero count. Make sure to also keep root span.
    metrics = copy.deepcopy(metrics)
    new_spans = {k: v for (k, v) in metrics.spans.items() if v.id == 0 or v.count != 0}

    for s in new_spans.values():
        s.children = {k: v for (k, v) in s.children.items() if v in new_spans}

    return CallMetrics(new_spans)


def main():
    parser = argparse.ArgumentParser(
        usage="usage: %(prog)s [options]",
        description="Formats the profiler output",
        formatter_class=argparse.RawTextHelpFormatter,
    )

    parser.add_argument(
        "-i",
        "--input",
        dest="input",
        default="-",
        help="I=input file name, or '-' for stdin. Defaults to stdin.",
    )

    parser.add_argument(
        "-n",
        "--normalize-count",
        dest="normalize_count",
        default=None,
        help=textwrap.dedent("""\
            normalizes the output by dividing the metrics by given factor:
              - a number: output will be scaled and divided by that number
              - a span name: output will be scaled and divided by the count value of that span
            """),
    )

    parser.add_argument(
        "-f",
        "--format",
        dest="format",
        default=None,
        choices=["tsv", "folded"],
        required=True,
        help=textwrap.dedent("""\
            produces output in a given format:
              - tsv: output will be formated as tsv
              - folded: output will be formatted as folded flamegraph profile
            """),
    )

    parser.add_argument(
        "-e",
        "--keep-empty",
        dest="keep_empty",
        action="store_true",
        default=False,
        help="will keep empty spans that have count of 0",
    )

    args = parser.parse_args()

    if args.input == "-":
        input_str = sys.stdin.read()
    else:
        with open(args.input, "r") as file:
            input_str = file.read()

    metrics = CallMetrics.from_json(json.loads(input_str))

    normalize_count = None
    if args.normalize_count is not None:
        if args.normalize_count.isnumeric():
            normalize_count = float(args.normalize_count)
        else:
            span_id = metrics.find_span(args.normalize_count)
            normalize_count = float(metrics.spans[span_id].count)
        new_metrics = CallMetrics.new_empty()
        new_metrics.add_weighted(metrics, 1.0 / normalize_count)
        metrics = new_metrics

    if not args.keep_empty:
        metrics = remove_empty_spans(metrics)

    if args.format == "folded":
        print_folded(metrics)
    elif args.format == "tsv":
        print_tsv(metrics)


if __name__ == "__main__":
    main()
