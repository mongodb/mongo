#!/usr/bin/env python3
# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0
"""
Test cases for IDL Schema validation.

This file exists to verify that all of the IDL files validates against the IDL Schema.
"""

import os
import unittest

import yaml
from jsonschema import SchemaError, ValidationError, validate

# import package so that it works regardless of whether we run as a module or file
if __package__ is None:
    import sys

    sys.path.append(os.path.dirname(os.path.abspath(__file__)))
    import testcase
else:
    from . import testcase


class TestJSONSchema(testcase.IDLTestcase):
    """Test the IDL Generator."""

    @property
    def _base_dir(self):
        """Get the path to the project folder."""
        base_dir = os.path.dirname(
            os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        )
        return base_dir

    @property
    def _src_dir(self):
        """Get the path to the src folder."""
        return os.path.join(
            self._base_dir,
            "src",
        )

    @property
    def _idl_src_dir(self):
        """Get the path to the src/mongo/idl folder."""
        return os.path.join(self._src_dir, "mongo", "idl")

    @property
    def _idl_buildscripts_dir(self):
        """Get the path to the buildscripts/idl folder."""
        return os.path.join(self._base_dir, "buildscripts", "idl")

    def load_yaml_file(self, file_path):
        with open(file_path, "r", encoding="utf8") as f:
            return yaml.safe_load(f)

    def validate_yaml_file(self, file_path, schema):
        """Validate a JSON file against a given schema."""
        try:
            data = self.load_yaml_file(file_path)
            validate(instance=data, schema=schema)
        except (ValidationError, SchemaError) as e:
            print(f"{file_path}: Validation error - {e.message}")
            return False
        except Exception as e:
            print(f"{file_path}: Unexpected error - {e}")
            raise
        return True

    def test_validate_all_idl_files(self):
        """Validate all .idl files in a directory against a given schema."""
        idl_schema = self.load_yaml_file(os.path.join(self._idl_buildscripts_dir, "idl_schema.yml"))

        # We assert on this boolean after all the tests are checked.
        success = True

        for root, _, files in os.walk(self._base_dir):
            for file in files:
                if file.endswith(".idl"):
                    abs_file_path = os.path.join(root, file)
                    success = success and self.validate_yaml_file(abs_file_path, idl_schema)

        if not success:
            self.fail("JSON Schema validation failed for at least one file")


if __name__ == "__main__":
    unittest.main()
