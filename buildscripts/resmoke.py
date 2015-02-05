#!/usr/bin/python

"""
Command line test utility for MongoDB tests of all kinds.

CURRENTLY IN ACTIVE DEVELOPMENT
If you are not a developer, you probably want to use smoke.py
"""

import logging
import logging.config
import optparse
import os
import re
import urllib

import smoke
import smoke_config

USAGE = \
    """resmoke.py <YAML/JSON CONFIG>

All options are specified as YAML or JSON - the configuration can be loaded via a file, as a named
configuration in the "smoke_config" module, piped as stdin, or specified on the command line as
options via the --set, --unset, and --push operators.

NOTE: YAML can only be used if the PyYaml library is available on your system.  Only JSON is
supported on the command line.

For example:
    resmoke.py './jstests/disk/*.js'

results in:

    Test Configuration:
    ---
    tests:
      roots:
      - ./jstests/disk/*.js
    suite:
      ...
    executor:
      fixtures:
        ...
      testers:
        ...
    logging:
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

DEFAULT_LOGGER_CONFIG = {}


def get_local_logger_filenames(logging_root):
    """Helper to extract filenames from the logging config for helpful reporting to the user."""

    filenames = []
    if "handlers" not in logging_root:
        return filenames

    for handler_name, handler_info in logging_root["handlers"].iteritems():
        if "filename" in handler_info:
            logger_filename = handler_info["filename"]
            filenames.append("file://%s" %
                             urllib.pathname2url(os.path.abspath(logger_filename)))

    return filenames


def main():

    named_configs = smoke_config.get_named_configs()

    parser = smoke.json_options.JSONOptionParser(usage=USAGE,
                                                 configfile_args=named_configs)

    help = \
        """Just outputs the configured JSON options."""

    parser.add_option('--dump-options', default=False, dest='dump_options', action="store_true",
                      help=help)

    help = \
        """Outputs all the tests found with metadata."""

    parser.add_option('--dump-tests', default=False, dest='dump_tests', action="store_true",
                      help=help)

    help = \
        """Outputs the tests in the suite."""

    parser.add_option('--dump-suite', default=False, dest='dump_suite', action="store_true",
                      help=help)

    values, args, json_root = parser.parse_json_args()

    # Assume remaining arguments are test roots
    if args:
        json_root = smoke.json_options.json_update_path(json_root, "tests.roots", args)

    # Assume all files in suite if not specified
    if "suite" not in json_root or json_root["suite"] is None:
        json_root["suite"] = {}

    # Assume default_logging if no other logging specified
    if "logging" not in json_root or json_root["logging"] is None:
        default_logging = \
            smoke.json_options.json_file_load(named_configs["log_default"])
        json_root["logging"] = default_logging["logging"]

    if "executor" not in json_root or json_root["executor"] is None:
        default_executor = \
            smoke.json_options.json_file_load(named_configs["executor_default"])
        json_root["executor"] = default_executor["executor"]

    if not values.dump_options:
        print "Test Configuration: \n---"

    for key in ["tests", "suite", "executor", "logging"]:
        if key in json_root:
            print smoke.json_options.json_dump({key: json_root[key]}),
    print

    if values.dump_options:
        return

    def validate_config(tests=None, suite=None, executor=None, logging=None, **kwargs):

        if len(kwargs) > 0:
            raise optparse.OptionValueError(
                "Unrecognized test options: %s" % kwargs)

        if not all([tests is not None, executor is not None]):
            raise optparse.OptionValueError(
                "Test options must contain \"tests\" and \"executor\".")

    validate_config(**json_root)
    logging.config.dictConfig(json_root["logging"])

    def re_compile_all(re_patterns):
        if isinstance(re_patterns, basestring):
            re_patterns = [re_patterns]
        return [re.compile(pattern) for pattern in re_patterns]

    def build_tests(roots=["./"],
                    include_files=[],
                    include_files_except=[],
                    exclude_files=[],
                    exclude_files_except=[],
                    extract_metadata=True,
                    **kwargs):

        if len(kwargs) > 0:
            raise optparse.OptionValueError(
                "Unrecognized options for tests: %s" % kwargs)

        file_regex_query = smoke.suites.RegexQuery(re_compile_all(include_files),
                                                   re_compile_all(
                                                       include_files_except),
                                                   re_compile_all(
                                                       exclude_files),
                                                   re_compile_all(exclude_files_except))

        if isinstance(roots, basestring):
            roots = [roots]

        return smoke.tests.build_tests(roots, file_regex_query, extract_metadata)

    tests = build_tests(**json_root["tests"])

    if values.dump_tests:
        print "Tests:\n%s" % tests

    def build_suite(tests,
                    include_tags=[],
                    include_tags_except=[],
                    exclude_tags=[],
                    exclude_tags_except=[],
                    **kwargs):

        if len(kwargs) > 0:
            raise optparse.OptionValueError(
                "Unrecognized options for suite: %s" % kwargs)

        tag_regex_query = smoke.suites.RegexQuery(re_compile_all(include_tags),
                                                  re_compile_all(
                                                      include_tags_except),
                                                  re_compile_all(exclude_tags),
                                                  re_compile_all(exclude_tags_except))

        return smoke.suites.build_suite(tests, tag_regex_query)

    suite = build_suite(tests, **json_root["suite"])
    suite.sort(key=lambda test: test.uri)

    if values.dump_suite:
        print "Suite:\n%s" % suite

    print "Running %s tests in suite (out of %s tests found)..." % (len(tests), len(suite))

    local_logger_filenames = get_local_logger_filenames(json_root["logging"])
    if local_logger_filenames:
        print "\nOutput from tests redirected to:\n\t%s\n" % \
            "\n\t".join(local_logger_filenames)

    try:
        smoke.executor.exec_suite(suite, logging.getLogger("executor"), **json_root["executor"])
    finally:
        if local_logger_filenames:
            print "\nOutput from tests was redirected to:\n\t%s\n" % \
                "\n\t".join(local_logger_filenames)

if __name__ == "__main__":
    main()
