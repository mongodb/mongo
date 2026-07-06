"""Unit tests for buildscripts/resmokelib/owners_generation.py."""

import os
import tempfile
import unittest

import yaml

from buildscripts.resmokelib.owners_generation import SuiteOwnersGenerator


class TestWriteOwnersYml(unittest.TestCase):
    def _write_and_load(self, entries):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "OWNERS.yml")
            SuiteOwnersGenerator._write_owners_yml(path, entries)
            with open(path) as f:
                text = f.read()
            return text, yaml.safe_load(text)

    def test_uses_latest_format_version(self):
        _, data = self._write_and_load([("/core.yml", ["10gen/query-execution"])])
        self.assertEqual(data["version"], "2.0.0")

    def test_empty_entries_emit_empty_filters(self):
        text, data = self._write_and_load([])
        self.assertEqual(data["filters"], [])
        # The file must still carry the auto-generated header.
        self.assertIn("AUTO-GENERATED", text)

    def test_entries_round_trip_into_parser_shape(self):
        entries = [
            ("/core.yml", ["10gen/query-execution"]),
            ("/sharding.yml", ["10gen/server-catalog-and-routing", "alice"]),
        ]
        _, data = self._write_and_load(entries)
        filters = data["filters"]
        self.assertEqual(len(filters), 2)
        for (pattern, approvers), _filter in zip(entries, filters):
            # Each filter is the OWNERS shape the codeowners parser expects:
            # the pattern as a key plus a sibling ``approvers`` list.
            self.assertEqual(_filter["approvers"], approvers)
            self.assertIn(pattern, _filter)


if __name__ == "__main__":
    unittest.main()
