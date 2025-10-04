#!/usr/bin/env python3
"""Compares IDL server parameters and configs between MongoDB server versions.

The comparison is computed by scanning though `base_version_dirs` and `incremented_version_dirs` looking for all configs and setParameters in each tree.
It then compares these looking for additions, removals, and deltas.  Finally it outputs a summary to the console.

This comparison does not currently support nested properties is as it does only simple string comparison on key:property pairs - see build_diff_fn as a means of extending the comparison capability in the future.
"""

import argparse
import io
import os
import pprint
import unittest
from enum import Enum

import yaml

_COMPARE_FIELDS_SERVER_PARAMETERS = ["default", "set_at", "validator", "test_only"]
_COMPARE_FIELDS_CONFIGS = ["arg_vartype", "requires", "hidden", "redact"]


class ComparisonType(str, Enum):
    CONFIGS = "configs"
    SERVER_PARAMETERS = "server_parameters"


class PropertyDiff:
    def __init__(self, base, inc):
        self.base = base
        self.inc = inc


class PropertiesDiffs:
    def __init__(self, removed, added, modified):
        self.removed = removed
        self.added = added
        self.modified = modified


def build_diff_fn(compare_fields: list) -> callable:
    def diff_fn(prop_base: dict, prop_inc: dict) -> dict:
        change_diffs = {}
        for field in compare_fields:
            if prop_base.get(field) != prop_inc.get(field):
                change_diffs[field] = PropertyDiff(
                    str(prop_base.get(field, "")), str(prop_inc.get(field, ""))
                )
        return change_diffs

    return diff_fn


class BuildBasePropertiesForComparisonHandler:
    """Interprets an .idl file representing a "base" version of configuration for comparison.

    As a base version, no comparison is required, only to build a list of configurations for
    future comparison.
    """

    def __init__(self, handler_type: ComparisonType):
        self.handler_type = handler_type
        self.properties = {}

    def handle(self, yaml_obj: dict, yaml_file_name: str) -> None:
        yaml_props = yaml_obj.get(self.handler_type)

        if yaml_props is not None:
            for prop, val in yaml_props.items():
                self.properties[prop, yaml_file_name] = val


class ComputeDiffsFromIncrementedVersionHandler:
    """Interprets an .idl file representing an "incremented" version containing changes from a "base" version.

    This handler performs comparison between the "incremented" state and a base state, and thus
    requires knowledge of a base dictionary of properties (base_properties) to execute.
    """

    def __init__(self, handler_type: ComparisonType, base_properties: dict, calc_diff_fn: callable):
        self.calc_diff = calc_diff_fn
        self.handler_type = handler_type
        self.properties_diff = PropertiesDiffs(base_properties, {}, {})

    def _compare_and_partition(self, yaml_props: dict, yaml_file_name: str) -> None:
        for yaml_key, yaml_val in yaml_props.items():
            compare_key = (yaml_key, yaml_file_name)

            # If the yaml file property does not exist in "removed" base version properties,
            # it must have been added in the incremented version
            if compare_key not in self.properties_diff.removed:
                self.properties_diff.added[compare_key] = yaml_val
                continue

            # Otherwise, we can remove it from 'removed' since it exists in both
            # version properties, and check if there is a diff
            # This will leave properties_diff.removed containing only entries that were
            # present in the base version properties, but not in the incremented version properties,
            # which means they were removed in the incremented version
            in_both_prop = self.properties_diff.removed.pop(compare_key)
            changed_properties = self.calc_diff(in_both_prop, yaml_val)

            if len(changed_properties) > 0:
                self.properties_diff.modified[compare_key] = changed_properties

    def handle(self, yaml_obj: dict, yaml_file_name: str) -> None:
        yaml_props = yaml_obj.get(self.handler_type.value)
        if yaml_props is not None:
            self._compare_and_partition(yaml_props, yaml_file_name)


def load_yaml(dirs: list, exclusions: list, idl_yaml_handlers: list) -> None:
    """Walks each path from top to bottom, applying each handler in idl_yaml_handlers to any .idl files encountered.

    If a directory encountered contains any string in exclusions, it is skipped and will not
    be included in the walk.
    """
    for directory in dirs:
        for dirpath, dirnames, filenames in os.walk(directory):
            for dirname in dirnames:
                for exclusion in exclusions:
                    if exclusion in dirpath + os.path.sep + dirname:
                        dirnames.remove(dirname)
                        break

            for name in filenames:
                if not name.endswith(".idl"):
                    continue

                with io.open(os.path.join(dirpath, name), "r", encoding="utf-8") as idl_yaml_stream:
                    idl_yaml = yaml.safe_load(idl_yaml_stream)
                    for handler in idl_yaml_handlers:
                        handler.handle(idl_yaml, name)


def get_properties_diffs(
    mode: ComparisonType, base_version_dirs: list, inc_version_dirs: list, exclude: list
) -> PropertiesDiffs:
    """Returns a PropertiesDiffs object containing the changes between properties in base_version_dirs and inc_version_dirs."""

    compare_fields = []
    if mode == ComparisonType.SERVER_PARAMETERS:
        compare_fields = _COMPARE_FIELDS_SERVER_PARAMETERS
    elif mode == ComparisonType.CONFIGS:
        compare_fields = _COMPARE_FIELDS_CONFIGS
    else:
        raise Exception(f"Unknown option {mode}")

    diff_fn = build_diff_fn(compare_fields)

    base_handler = BuildBasePropertiesForComparisonHandler(mode)
    load_yaml(base_version_dirs, exclude, [base_handler])

    increment_handler = ComputeDiffsFromIncrementedVersionHandler(
        mode, base_handler.properties, diff_fn
    )
    load_yaml(inc_version_dirs, exclude, [increment_handler])

    return increment_handler.properties_diff


def output_diffs(mode: ComparisonType, diff: PropertiesDiffs) -> None:
    pp = pprint.PrettyPrinter()

    mode_format = ""
    if mode == ComparisonType.CONFIGS:
        mode_format = "config"
    elif mode == ComparisonType.SERVER_PARAMETERS:
        mode_format = "server parameter"
    else:
        raise Exception(f"Unknown option {mode}")

    for sp, val in diff.added.items():
        if not val.get("test_only"):
            print(f"Added {mode_format} {str(sp)}")
            pp.pprint(val)
            print()

    for sp, val in diff.removed.items():
        if not val.get("test_only"):
            print(f"Removed {mode_format} {str(sp)}")
            pp.pprint(val)
            print()

    for sp, val in diff.modified.items():
        if not val.get("test_only"):
            print(f"Modified {mode_format} {str(sp)}")
            for property_name, delta in val.items():
                print(f"<{property_name}> changed from [{delta.base}] to [{delta.inc}]")
            print()


def main():
    arg_parser = argparse.ArgumentParser(prog="Core Server IDL Parameter/Config Diff")

    arg_parser.add_argument(
        "mode", choices=[ComparisonType.SERVER_PARAMETERS.value, ComparisonType.CONFIGS.value]
    )
    arg_parser.add_argument(
        "-b",
        "--base_version_dirs",
        help="A colon-separated list of paths to the base version for comparison",
        required=True,
    )

    arg_parser.add_argument(
        "-i",
        "--incremented_version_dirs",
        help="A colon-separated list of paths to the incremented version for comparison",
        required=True,
    )
    arg_parser.add_argument(
        "-e",
        "--exclude_dirs",
        help="A colon-separated list of directory path strings to exclude from comparison, "
        + "e.g. a path /foo/bar/dir will be excluded by an argument of any of foo/bar/dir, bar/dir,"
        + "foo, or bar, or dir ",
        required=False,
    )

    args = arg_parser.parse_args()

    incremented_version_dirs = str.split(args.incremented_version_dirs, ":")
    base_version_dirs = str.split(args.base_version_dirs, ":")
    exclude = set(args.exclude_dirs.split(":")) if args.exclude_dirs else set()
    mode = ComparisonType(args.mode)

    diffs = get_properties_diffs(mode, base_version_dirs, incremented_version_dirs, exclude)
    output_diffs(mode, diffs)


if __name__ == "__main__":
    main()


#########################################################################################
#  python3 -m unittest buildscripts/config_diff.py
#########################################################################################
class TestBuildBasePropertiesForComparisonHandler(unittest.TestCase):
    def test_yaml_obj_filters_comparison_types_correctly(self):
        filename = "test.yml"
        document = """
            global:
              cpp_namespace: "mongo"

            server_parameters:
              changeStreamOptions:
                description: "Cluster server parameter for change stream options"
                set_at: cluster
                omit_in_ftdc: false
                cpp_class:
                  name: ChangeStreamOptionsParameter
                  override_set: true
                  override_validate: true

            configs:
              "net.compression.compressors":
                description: 'Comma-separated list of compressors to use for network messages'
                source: [ cli, ini, yaml ]
                arg_vartype: String
                short_name: networkMessageCompressors
                default: 'snappy,zstd,zlib' 
        """
        yaml_obj = yaml.load(document, Loader=yaml.FullLoader)

        fixture = BuildBasePropertiesForComparisonHandler(ComparisonType.SERVER_PARAMETERS)
        fixture.handle(yaml_obj, filename)

        # should filter out configs, but parse server parameters
        self.assertIsNone(fixture.properties.get(("net.compression.compressors", filename)))
        self.assertIsNotNone(fixture.properties[("changeStreamOptions", filename)])

        fixture = BuildBasePropertiesForComparisonHandler(ComparisonType.CONFIGS)
        fixture.handle(yaml_obj, filename)

        # should filter out server parameters, but parse configs
        self.assertIsNone(fixture.properties.get(("changeStreamOptions", filename)))
        self.assertIsNotNone(fixture.properties.get(("net.compression.compressors", filename)))

    def test_empty_yaml_obj_does_nothing(self):
        filename = "test.yml"
        document = """
            global:
              cpp_namespace: "mongo"
        """

        yaml_obj = yaml.load(document, Loader=yaml.FullLoader)

        fixture = BuildBasePropertiesForComparisonHandler(ComparisonType.SERVER_PARAMETERS)
        fixture.handle(yaml_obj, filename)
        self.assertTrue(len(fixture.properties) == 0)

        fixture = BuildBasePropertiesForComparisonHandler(ComparisonType.CONFIGS)
        fixture.handle(yaml_obj, filename)
        self.assertTrue(len(fixture.properties) == 0)


class TestComputeDiffsFromIncrementedVersionHandler(unittest.TestCase):
    def setUp(self):
        self.parameter_diff_function = build_diff_fn(_COMPARE_FIELDS_SERVER_PARAMETERS)
        self.config_diff_function = build_diff_fn(_COMPARE_FIELDS_CONFIGS)

    def test_yaml_obj_filtered_correctly(self):
        filename = "inc.yml"
        document = """
            server_parameters:
              testOptions:
                description: "Cluster server parameter for change stream options"
                set_at: cluster
                omit_in_ftdc: false
                cpp_class:
                  name: ChangeStreamOptionsParameter
                  override_set: true
                  override_validate: true
              helloMorld:
                description: "yep"
                set_at: allthetime
                cpp_class:
                  name: ChangeStreamOptionsParameter
                  override_set: true
                  override_validate: true

            configs:
              "asdf":
                description: 'Comma-separated list of compressors to use for network messages'
                source: [ cli, ini, yaml ]
                arg_vartype: String
                short_name: networkMessageCompressors
                default: 'snappy,zstd,zlib' 
              "qwer":
                description: 'ok'
                source: [ cli, yaml ]
                arg_vartype: String
                short_name: networkMessageCompressors
                default: 'zlib' 
        """

        inc_yaml_obj = yaml.load(document, Loader=yaml.FullLoader)

        inc_fixture = ComputeDiffsFromIncrementedVersionHandler(
            ComparisonType.CONFIGS, {}, self.config_diff_function
        )
        inc_fixture.handle(inc_yaml_obj, filename)

        properties_diffs = inc_fixture.properties_diff
        self.assertIsNotNone(properties_diffs.added.get(("asdf", filename)))
        self.assertIsNotNone(properties_diffs.added.get(("qwer", filename)))

        self.assertIsNone(properties_diffs.added.get(("testOptions", filename)))
        self.assertIsNone(properties_diffs.added.get(("helloMorld", filename)))

        inc_fixture = ComputeDiffsFromIncrementedVersionHandler(
            ComparisonType.SERVER_PARAMETERS, {}, self.parameter_diff_function
        )
        inc_fixture.handle(inc_yaml_obj, filename)

        properties_diffs = inc_fixture.properties_diff

        self.assertIsNone(properties_diffs.added.get(("asdf", filename)))
        self.assertIsNone(properties_diffs.added.get(("qwer", filename)))

        self.assertIsNotNone(properties_diffs.added.get(("testOptions", filename)))
        self.assertIsNotNone(properties_diffs.added.get(("helloMorld", filename)))

    def test_added_works_correctly(self):
        filename = "test.yaml"
        document = """
            server_parameters:
              testOptions:
                description: "Cluster server parameter for change stream options"
                set_at: cluster
                omit_in_ftdc: false
                cpp_class:
                  name: ChangeStreamOptionsParameter
                  override_set: true
                  override_validate: true

            configs:
              "asdf":
                description: 'Comma-separated list of compressors to use for network messages'
                source: [ cli, ini, yaml ]
                arg_vartype: String
                short_name: networkMessageCompressors
                default: 'snappy,zstd,zlib' 
        """

        inc_yaml_obj = yaml.load(document, Loader=yaml.FullLoader)

        inc_fixture = ComputeDiffsFromIncrementedVersionHandler(
            ComparisonType.CONFIGS, {}, self.config_diff_function
        )
        inc_fixture.handle(inc_yaml_obj, filename)

        properties_diffs = inc_fixture.properties_diff

        self.assertIsNotNone(properties_diffs.added.get(("asdf", filename)))
        self.assertTrue(len(properties_diffs.added) == 1)
        self.assertTrue(len(properties_diffs.removed) == 0)
        self.assertTrue(len(properties_diffs.modified) == 0)

        inc_fixture = ComputeDiffsFromIncrementedVersionHandler(
            ComparisonType.SERVER_PARAMETERS, {}, self.parameter_diff_function
        )
        inc_fixture.handle(inc_yaml_obj, filename)

        properties_diffs = inc_fixture.properties_diff
        self.assertTrue(len(properties_diffs.added) == 1)
        self.assertIsNotNone(properties_diffs.added.get(("testOptions", filename)))
        self.assertTrue(len(properties_diffs.removed) == 0)
        self.assertTrue(len(properties_diffs.modified) == 0)

    def test_removed_works_correctly(self):
        filename = "test.yaml"
        document = """
            server_parameters:
            configs:
        """

        def get_base_data():
            return {("ok", "test.yaml"): {"yes": "no"}, ("also_ok", "blah.yaml"): {"no": "yes"}}

        inc_yaml_obj = yaml.load(document, Loader=yaml.FullLoader)

        inc_fixture = ComputeDiffsFromIncrementedVersionHandler(
            ComparisonType.CONFIGS, get_base_data(), self.config_diff_function
        )
        inc_fixture.handle(inc_yaml_obj, filename)

        properties_diffs = inc_fixture.properties_diff

        self.assertIsNotNone(properties_diffs.removed.get(("ok", filename)))
        self.assertIsNotNone(properties_diffs.removed.get(("also_ok", "blah.yaml")))

        self.assertTrue(len(properties_diffs.removed) == 2)
        self.assertTrue(len(properties_diffs.added) == 0)
        self.assertTrue(len(properties_diffs.modified) == 0)

        inc_fixture = ComputeDiffsFromIncrementedVersionHandler(
            ComparisonType.SERVER_PARAMETERS, get_base_data(), self.parameter_diff_function
        )
        inc_fixture.handle(inc_yaml_obj, filename)

        properties_diffs = inc_fixture.properties_diff

        self.assertIsNotNone(properties_diffs.removed.get(("ok", filename)))
        self.assertIsNotNone(properties_diffs.removed.get(("also_ok", "blah.yaml")))

        self.assertTrue(len(properties_diffs.removed) == 2)
        self.assertTrue(len(properties_diffs.added) == 0)
        self.assertTrue(len(properties_diffs.modified) == 0)

    def test_empty_modified_works_correctly(self):
        filename = "test.yaml"
        document = """
            server_parameters:
              testOptions:
                description: "Cluster server parameter for change stream options"
                set_at: cluster
                omit_in_ftdc: false
                cpp_class:
                  name: ChangeStreamOptionsParameter
                  override_set: true
                  override_validate: true

              testParameter:
                description: "Some parameter"
                set_at: cluster
                omit_in_ftdc: false
                cpp_class:
                  name: ChangeStreamOptionsParameter
                  override_set: true
                  override_validate: true

            configs:
              "asdf":
                description: 'Comma-separated list of compressors to use for network messages'
                source: [ cli, ini, yaml ]
                arg_vartype: String
                short_name: networkMessageCompressors
                default: 'snappy,zstd,zlib' 

              "zxcv":
                description: 'Comma-separated list of compressors to use for network messages'
                source: [ cli, ini, yaml ]
                arg_vartype: String
                short_name: networkMessageCompressors
                default: 'snappy,zstd,zlib' 
        """
        inc_yaml_obj = yaml.load(document, Loader=yaml.FullLoader)

        inc_fixture = ComputeDiffsFromIncrementedVersionHandler(
            ComparisonType.CONFIGS, {}, build_diff_fn(["default"])
        )
        inc_fixture.handle(inc_yaml_obj, filename)

        properties_diffs = inc_fixture.properties_diff

        self.assertIsNotNone(properties_diffs.added.get(("asdf", filename)))
        self.assertTrue(len(properties_diffs.removed) == 0)
        self.assertTrue(len(properties_diffs.modified) == 0)

        inc_fixture = ComputeDiffsFromIncrementedVersionHandler(
            ComparisonType.SERVER_PARAMETERS, {}, build_diff_fn(["set_at"])
        )
        inc_fixture.handle(inc_yaml_obj, filename)

        properties_diffs = inc_fixture.properties_diff

        self.assertIsNotNone(properties_diffs.added.get(("testOptions", filename)))
        self.assertTrue(len(properties_diffs.removed) == 0)
        self.assertTrue(len(properties_diffs.modified) == 0)

    def test_not_modified_between_yamls_reports_correctly(self):
        filename = "test.yaml"
        document = """
            server_parameters:
              testOptions:
                description: "Cluster server parameter for change stream options"
                set_at: cluster
                omit_in_ftdc: false
                cpp_class:
                  name: ChangeStreamOptionsParameter
                  override_set: true
                  override_validate: true

              testParameter:
                description: "Some parameter"
                set_at: cluster
                omit_in_ftdc: false
                cpp_class:
                  name: ChangeStreamOptionsParameter
                  override_set: true
                  override_validate: true

            configs:
              "asdf":
                description: 'Comma-separated list of compressors to use for network messages'
                source: [ cli, ini, yaml ]
                arg_vartype: String
                short_name: networkMessageCompressors
                default: 'snappy,zstd,zlib' 

              "zxcv":
                description: 'Comma-separated list of compressors to use for network messages'
                source: [ cli, ini, yaml ]
                arg_vartype: String
                short_name: networkMessageCompressors
                default: 'snappy,zstd,zlib' 
        """

        document_inc = document

        document_yaml = yaml.load(document, Loader=yaml.FullLoader)
        document_inc_yaml = yaml.load(document_inc, Loader=yaml.FullLoader)

        diff_fn = build_diff_fn(_COMPARE_FIELDS_CONFIGS)

        config_base_properties_handler = BuildBasePropertiesForComparisonHandler(
            ComparisonType.CONFIGS
        )
        config_base_properties_handler.handle(document_yaml, filename)

        config_inc_properties_handler = ComputeDiffsFromIncrementedVersionHandler(
            ComparisonType.CONFIGS, config_base_properties_handler.properties, diff_fn
        )
        config_inc_properties_handler.handle(document_inc_yaml, filename)

        property_diff = config_inc_properties_handler.properties_diff

        self.assertEqual(0, len(property_diff.modified))

        diff_fn = build_diff_fn(_COMPARE_FIELDS_SERVER_PARAMETERS)

        sp_base_properties_handler = BuildBasePropertiesForComparisonHandler(
            ComparisonType.SERVER_PARAMETERS
        )
        sp_base_properties_handler.handle(document_yaml, filename)

        sp_inc_properties_handler = ComputeDiffsFromIncrementedVersionHandler(
            ComparisonType.SERVER_PARAMETERS, sp_base_properties_handler.properties, diff_fn
        )
        sp_inc_properties_handler.handle(document_inc_yaml, filename)

        property_diff = sp_inc_properties_handler.properties_diff

        self.assertEqual(0, len(property_diff.modified))

    def test_modified_between_yamls_reports_correctly(self):
        filename = "test.yaml"
        document = """
            server_parameters:
              testOptions:
                description: "Cluster server parameter for change stream options"
                set_at: cluster
                omit_in_ftdc: false
                cpp_class:
                  name: ChangeStreamOptionsParameter
                  override_set: true
                  override_validate: true

              testParameter:
                description: "Some parameter"
                set_at: cluster
                omit_in_ftdc: false
                cpp_class:
                  name: ChangeStreamOptionsParameter
                  override_set: true
                  override_validate: true

            configs:
              "asdf":
                description: 'Comma-separated list of compressors to use for network messages'
                source: [ cli, ini, yaml ]
                arg_vartype: String
                short_name: networkMessageCompressors
                default: 'snappy,zstd,zlib' 

              "zxcv":
                description: 'Comma-separated list of compressors to use for network messages'
                source: [ cli, ini, yaml ]
                arg_vartype: String
                short_name: networkMessageCompressors
                default: 'snappy,zstd,zlib' 
        """

        document_inc = """
            server_parameters:
              testOptions:
                description: "Cluster server parameter for change stream options"
                set_at: runtime
                cpp_class:
                  name: ChangeStreamOptionsParameter
                  override_set: true
                  override_validate: true

              testParameter:
                description: "Some parameter"
                set_at: cluster
                omit_in_ftdc: false
                cpp_class:
                  name: ChangeStreamOptionsParameter
                  override_set: true
                  override_validate: true

            configs:
              "asdf":
                description: 'Comma-separated list of compressors to use for network messages'
                source: [ cli, ini, yaml ]
                arg_vartype: int
                short_name: networkMessageCompressors
                default: 'snappy,zstd,zlib' 

              "zxcv":
                description: 'Comma-separated list of compressors to use for network messages'
                source: [ cli, ini, yaml ]
                arg_vartype: String
                short_name: networkMessageCompressors
                default: 'snappy,zstd,zlib' 
        """

        document_yaml = yaml.load(document, Loader=yaml.FullLoader)
        document_inc_yaml = yaml.load(document_inc, Loader=yaml.FullLoader)

        diff_fn = build_diff_fn(_COMPARE_FIELDS_CONFIGS)

        config_base_properties_handler = BuildBasePropertiesForComparisonHandler(
            ComparisonType.CONFIGS
        )
        config_base_properties_handler.handle(document_yaml, filename)

        config_inc_properties_handler = ComputeDiffsFromIncrementedVersionHandler(
            ComparisonType.CONFIGS, config_base_properties_handler.properties, diff_fn
        )
        config_inc_properties_handler.handle(document_inc_yaml, filename)

        property_diff = config_inc_properties_handler.properties_diff

        self.assertEqual(
            property_diff.modified.get(("asdf", filename)).get("arg_vartype").base, "String"
        )
        self.assertEqual(
            property_diff.modified.get(("asdf", filename)).get("arg_vartype").inc, "int"
        )
        self.assertIsNone(property_diff.modified.get(("zxcv", filename)))

        diff_fn = build_diff_fn(_COMPARE_FIELDS_SERVER_PARAMETERS)

        sp_base_properties_handler = BuildBasePropertiesForComparisonHandler(
            ComparisonType.SERVER_PARAMETERS
        )
        sp_base_properties_handler.handle(document_yaml, filename)

        sp_inc_properties_handler = ComputeDiffsFromIncrementedVersionHandler(
            ComparisonType.SERVER_PARAMETERS, sp_base_properties_handler.properties, diff_fn
        )
        sp_inc_properties_handler.handle(document_inc_yaml, filename)

        property_diff = sp_inc_properties_handler.properties_diff

        self.assertEqual(
            property_diff.modified.get(("testOptions", filename)).get("set_at").base, "cluster"
        )
        self.assertEqual(
            property_diff.modified.get(("testOptions", filename)).get("set_at").inc, "runtime"
        )
        self.assertIsNone(property_diff.modified.get(("testParameter", filename)))


class TestPropertiesDiffFunction(unittest.TestCase):
    def test_empty_returns_empty(self):
        fn = build_diff_fn([])
        diffs = fn({"same": "different"}, {"same": "different"})
        self.assertTrue(len(diffs) == 0)

    def test_return_one_diff(self):
        fn = build_diff_fn(["same"])
        diffs = fn({"same": "dofferent"}, {"same": "different"})
        self.assertTrue(len(diffs) == 1)
        self.assertTrue(diffs["same"].base == "dofferent")
        self.assertTrue(diffs["same"].inc == "different")

    def test_return_two_diffs(self):
        fn = build_diff_fn(["same", "a"])
        diffs = fn({"same": "dofferent", "a": "1"}, {"same": "different", "a": "2"})
        self.assertTrue(len(diffs) == 2)
        self.assertTrue(diffs["same"].base == "dofferent")
        self.assertTrue(diffs["same"].inc == "different")
        self.assertTrue(diffs["a"].base == "1")
        self.assertTrue(diffs["a"].inc == "2")

    def test_only_base_returns_diff(self):
        fn = build_diff_fn(["a"])
        diffs = fn({"a": "1"}, {})
        self.assertTrue(len(diffs) == 1)
        self.assertTrue(diffs["a"].base == "1")
        self.assertTrue(diffs["a"].inc == "")

    def test_only_inc_returns_diff(self):
        fn = build_diff_fn(["a"])
        diffs = fn({}, {"a": "1"})
        self.assertTrue(len(diffs) == 1)
        self.assertTrue(diffs["a"].inc == "1")
        self.assertTrue(diffs["a"].base == "")

    def test_nonincluided_field_is_not_diff(self):
        fn = build_diff_fn(["b"])
        diffs = fn({}, {"a": "1"})
        self.assertTrue(len(diffs) == 0)

    def test_base_version_nexist_added_none(self):
        fn = build_diff_fn(["a"])
        diffs = fn({}, {"a": "None"})
        self.assertTrue(diffs["a"].inc == "None")
        self.assertTrue(diffs["a"].base == "")
        self.assertTrue(len(diffs) == 1)


class TestCLIFunctions(unittest.TestCase):
    def test_unknown_comparison_type_throws(self):
        with self.assertRaises(Exception):
            output_diffs(None, PropertiesDiffs({}, {}, {}))

        with self.assertRaises(Exception):
            get_properties_diffs(None, [], [], [])
