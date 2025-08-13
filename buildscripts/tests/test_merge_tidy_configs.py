import os
import pathlib
import tempfile
import unittest

import yaml
from bazel.merge_tidy_configs import (
    deep_merge_dicts,
    filter_and_sort_config_paths,
    is_ancestor_directory,
    load_yaml,
    merge_check_options_into_config,
    merge_checks_into_config,
    split_checks_to_list,
)


class TestClangTidyMergeHelpers(unittest.TestCase):  
  
    def test_split_checks_to_list_from_str(self):  
        self.assertEqual(  
            split_checks_to_list("foo, bar ,baz"),  
            ["foo", "bar", "baz"]  
        )  
  
    def test_split_checks_to_list_from_list(self):  
        self.assertEqual(  
            split_checks_to_list(["a, b", "c"]),  
            ["a", "b", "c"]  
        )  
  
    def test_merge_checks_into_config(self):  
        base = {"Checks": "a,b"}  
        incoming = {"Checks": "c,d"}  
        merge_checks_into_config(base, incoming)  
        self.assertEqual(base["Checks"], "a,b,c,d")  
  
    def test_merge_check_options_into_config(self):  
        base = {"CheckOptions": [{"key": "A", "value": "1"}]}  
        incoming = {"CheckOptions": [{"key": "B", "value": "2"}]}  
        merge_check_options_into_config(base, incoming)  
        self.assertEqual(  
            base["CheckOptions"],  
            [{"key": "A", "value": "1"}, {"key": "B", "value": "2"}]  
        )  
  
    def test_merge_check_options_override(self):  
        base = {"CheckOptions": [{"key": "A", "value": "1"}]}  
        incoming = {"CheckOptions": [{"key": "A", "value": "2"}]}  
        merge_check_options_into_config(base, incoming)  
        self.assertEqual(base["CheckOptions"], [{"key": "A", "value": "2"}])  
  
    def test_deep_merge_dicts(self):  
        base = {"Outer": {"Inner": 1}, "Keep": True}  
        override = {"Outer": {"Added": 2}, "New": False}  
        merged = deep_merge_dicts(base, override)  
        self.assertEqual(  
            merged,  
            {"Outer": {"Inner": 1, "Added": 2}, "Keep": True, "New": False}  
        )  
  
    def test_is_ancestor_directory_true(self):  
        tmpdir = pathlib.Path(tempfile.mkdtemp())  
        child = tmpdir / "subdir"  
        child.mkdir()  
        self.assertTrue(is_ancestor_directory(tmpdir, child))  
  
    def test_is_ancestor_directory_false(self):  
        tmp1 = pathlib.Path(tempfile.mkdtemp())  
        tmp2 = pathlib.Path(tempfile.mkdtemp())  
        self.assertFalse(is_ancestor_directory(tmp1, tmp2))  
  
    def test_filter_and_sort_config_paths_no_scope(self):  
        files = ["/tmp/file1", "/tmp/file2"]  
        res = filter_and_sort_config_paths(files, None)  
        self.assertEqual([pathlib.Path("/tmp/file1"), pathlib.Path("/tmp/file2")], res)  
  
    def test_filter_and_sort_config_paths_with_scope(self):  
        tmpdir = pathlib.Path(tempfile.mkdtemp())  
        (tmpdir / "a").mkdir()  
        cfg_root = tmpdir / "root.yaml"  
        cfg_child = tmpdir / "a" / "child.yaml"  
        cfg_root.write_text("Checks: a")  
        cfg_child.write_text("Checks: b")  
    
        old_cwd = pathlib.Path.cwd()  
        try:  
            # Simulate repo root being tmpdir  
            os.chdir(tmpdir)  
            res = filter_and_sort_config_paths([cfg_root, cfg_child], "a")  
        finally:  
            os.chdir(old_cwd)  
    
        self.assertEqual([p.name for p in res], ["root.yaml", "child.yaml"])  
    
    def test_load_yaml_empty_file(self):  
        tmpfile = pathlib.Path(tempfile.mktemp())  
        tmpfile.write_text("")  
        self.assertEqual(load_yaml(tmpfile), {})  
  
    def test_load_yaml_valid_yaml(self):  
        tmpfile = pathlib.Path(tempfile.mktemp())  
        yaml.safe_dump({"a": 1}, open(tmpfile, "w"))  
        self.assertEqual(load_yaml(tmpfile), {"a": 1})  
  
  
if __name__ == "__main__":  
    unittest.main()  
