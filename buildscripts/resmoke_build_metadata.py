#!/usr/bin/python

"""
Sample utility to build test metadata JSON (i.e. tags) from test files that contain them.

CURRENTLY IN ACTIVE DEVELOPMENT
If you are not a developer, you probably want to look at smoke.py
"""

import re

import smoke
import smoke_config

USAGE = \
    """resmoke_build_metadata.py <YAML/JSON CONFIG>

Generates test metadata based on information in test files themselves.  All options are specified \
as YAML or JSON - the configuration is the "tests" subset of the configuration for a resmoke.py
test run.

NOTE: YAML can only be used if the PyYaml library is available on your system.  Only JSON is
supported on the command line.

For example:
    resmoke_build_metadata.py './jstests/disk/*.js'

results in:

    Metadata extraction configuration:
    ---
    tests:
      roots:
      - ./jstests/disk/*.js
    ...
    
Named sets of options are available in the "smoke_config" module, including:

    --jscore
    --sharding
    --replicasets
    --disk
    
For example:
    resmoke.py --jscore
    resmoke.py --sharding
    
""" + smoke.json_options.JSONOptionParser.DEFAULT_USAGE


def main():

    parser = smoke.json_options.JSONOptionParser(usage=USAGE,
                                                 configfile_args=smoke_config.get_named_configs())

    values, args, json_root = parser.parse_json_args()

    if "tests" in json_root:
        json_root = {"tests": json_root["tests"]}

    # Assume remaining arguments are test roots
    if args:
        json_root = smoke.json_options.json_update_path(json_root, "tests.roots", args)

    print "Metadata extraction configuration:"
    print smoke.json_options.json_dump(json_root)

    if not "tests" in json_root or json_root["tests"] is None:
        raise Exception("No tests specified.")

    def re_compile_all(re_patterns):
        if isinstance(re_patterns, basestring):
            re_patterns = [re_patterns]
        return [re.compile(pattern) for pattern in re_patterns]

    def build_test_metadata(roots=["./"],
                            include_files=[],
                            include_files_except=[],
                            exclude_files=[],
                            exclude_files_except=[],
                            **kwargs):

        if len(kwargs) > 0:
            raise optparse.OptionValueError(
                "Unrecognized options for building test metadata: %s" % kwargs)

        file_regex_query = smoke.suites.RegexQuery(re_compile_all(include_files),
                                                   re_compile_all(
                                                       include_files_except),
                                                   re_compile_all(
                                                       exclude_files),
                                                   re_compile_all(exclude_files_except))

        tests = smoke.tests.build_tests(roots, file_regex_query, extract_metadata=True)

        print "Writing test metadata for %s tests..." % len(tests)
        smoke.tests.write_metadata(tests, json_only=True)
        print "Test metadata written."

    build_test_metadata(**json_root["tests"])

if __name__ == "__main__":
    main()
