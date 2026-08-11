#!/usr/bin/env python3
"""Script for symbolizing multithread MongoDB stack traces.

Accepts mongod multithread stacktrace lines. These are produced by hitting mongod with SIGUSR2.
Assembles json documents which are fed to the mongosymb library. See mongosymb.py.
"""

import argparse
import json
import sys

import mongosymb


def main():
    """Execute Main program."""

    parent_parser = mongosymb.make_argument_parser(add_help=False)
    parser = argparse.ArgumentParser(parents=[parent_parser], description=__doc__, add_help=True)
    options = parser.parse_args()

    # Remember the prologue between lines,
    # Prologue defines the library ids referred to by each record line.
    prologue = None

    for line in sys.stdin:
        try:
            doc = json.JSONDecoder().raw_decode(line)[0]
            if "attr" not in doc:
                continue
            attr = doc["attr"]

            if "prologue" in attr:
                prologue = attr["prologue"]

            # "threadRecord" is an older name for "record".
            # Accept either name.
            for record_field_name in ("record", "threadRecord"):
                if record_field_name not in attr:
                    continue
                thread_record = attr[record_field_name]
                merged = {**thread_record, **prologue}

                output_fn = None
                if options.output_format == "json":
                    output_fn = json.dump
                if options.output_format == "classic":
                    output_fn = mongosymb.classic_output

                resolver = None
                if options.debug_file_resolver == "path":
                    resolver = mongosymb.PathDbgFileResolver(options.path_to_executable)
                elif options.debug_file_resolver == "s3":
                    resolver = mongosymb.S3BuildidDbgFileResolver(
                        options.s3_cache_dir, options.s3_bucket
                    )

                frames = mongosymb.symbolize_frames(merged, resolver, **vars(options))
                print(
                    "\nthread {{name='{}', tid={}}}:".format(
                        thread_record["name"], thread_record["tid"]
                    )
                )

                output_fn(frames, sys.stdout, indent=2)

        except json.JSONDecodeError:
            print("failed to parse line: `{}`".format(line), file=sys.stderr)


if __name__ == "__main__":
    main()
    sys.exit(0)
