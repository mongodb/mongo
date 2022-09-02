"""Unit tests for buildscripts/resmokelib/run/list_tags.py."""
import unittest

from buildscripts.resmokelib.run import list_tags


def _get_suite(tags_blocks):
    block = ""
    for tags_block in tags_blocks:
        block = f"{block}  {tags_block}" + "\n"
    return (("test_kind: js_test\n"
             "\n"
             "selector:\n"
             "  roots: []\n") + block + ("\n"
                                         "executor:\n"
                                         "  config: {}\n"
                                         "  fixture:\n"
                                         "    class: MongoDFixture\n"
                                         "    mongod_options:\n"
                                         "      set_parameters:\n"
                                         "        enableTestCommands: 1\n"))


class TestParseTagsBlocks(unittest.TestCase):
    def test_no_tags_blocks(self):
        suite = _get_suite([])
        result = list_tags.parse_tags_blocks(suite)
        self.assertCountEqual([], result)

    def test_empty_tags_block(self):
        tags_blocks = ["exclude_with_any_tags:\n"]
        suite = _get_suite(tags_blocks)
        result = list_tags.parse_tags_blocks(suite)
        self.assertCountEqual(tags_blocks, result)

    def test_two_tags_blocks(self):
        tags_blocks = [("exclude_with_any_tags:\n"
                        "  - dummy_tag_1\n"
                        "  - dummy_tag_2\n"
                        "  - dummy_tag_3"),
                       ("include_with_any_tags:\n"
                        "  - dummy_tag_4\n"
                        "  - dummy_tag_5\n"
                        "  - dummy_tag_6")]
        suite = _get_suite(tags_blocks)
        result = list_tags.parse_tags_blocks(suite)
        self.assertCountEqual(tags_blocks, result)

    def test_tags_block_with_tags_and_above_comments(self):
        tags_blocks = [("exclude_with_any_tags:\n"
                        "  # comment\n"
                        "  - dummy_tag_1\n"
                        "  # comment line 1\n"
                        "  # comment line 2\n"
                        "  - dummy_tag_2\n"
                        "  #################\n"
                        "  # fancy comment #\n"
                        "  #################\n"
                        "  - dummy_tag_3")]
        suite = _get_suite(tags_blocks)
        result = list_tags.parse_tags_blocks(suite)
        self.assertCountEqual(tags_blocks, result)

    def test_tags_block_with_tags_and_inline_comments(self):
        tags_blocks = [("exclude_with_any_tags:\n"
                        "  - dummy_tag_1  # inline comment\n"
                        "  - dummy_tag_2  # another one\n"
                        "  - dummy_tag_3  # and another one")]
        suite = _get_suite(tags_blocks)
        result = list_tags.parse_tags_blocks(suite)
        self.assertCountEqual(tags_blocks, result)

    def test_tags_block_with_tags_and_both_comments(self):
        tags_blocks = [("exclude_with_any_tags:\n"
                        "  # above comment\n"
                        "  - dummy_tag_1  # inline comment\n"
                        "  # above comment line 1\n"
                        "  # above comment line 2\n"
                        "  - dummy_tag_2  # another one inline\n"
                        "  #######################\n"
                        "  # above fancy comment #\n"
                        "  #######################\n"
                        "  - dummy_tag_3  # and another one inline")]
        suite = _get_suite(tags_blocks)
        result = list_tags.parse_tags_blocks(suite)
        self.assertCountEqual(tags_blocks, result)


class TestSplitIntoTags(unittest.TestCase):
    def test_empty_block(self):
        result = list_tags.split_into_tags("")
        self.assertCountEqual([], result)

    def test_block_with_no_tags(self):
        block = "exclude_with_any_tags:\n"
        result = list_tags.split_into_tags(block)
        self.assertCountEqual([[""]], result)

    def test_block_with_tags_no_comments(self):
        block = ("exclude_with_any_tags:\n"
                 "  - dummy_tag_1\n"
                 "  - dummy_tag_2\n"
                 "  - dummy_tag_3")
        expected = [
            ["- dummy_tag_1"],
            ["- dummy_tag_2"],
            ["- dummy_tag_3"],
        ]
        result = list_tags.split_into_tags(block)
        self.assertCountEqual(expected, result)

    def test_block_with_tags_and_above_comments(self):
        block = ("exclude_with_any_tags:\n"
                 "  # comment\n"
                 "  - dummy_tag_1\n"
                 "  # comment line 1\n"
                 "  # comment line 2\n"
                 "  - dummy_tag_2\n"
                 "  #################\n"
                 "  # fancy comment #\n"
                 "  #################\n"
                 "  - dummy_tag_3")
        expected = [
            [
                "# comment",
                "- dummy_tag_1",
            ],
            [
                "# comment line 1",
                "# comment line 2",
                "- dummy_tag_2",
            ],
            [
                "#################",
                "# fancy comment #",
                "#################",
                "- dummy_tag_3",
            ],
        ]
        result = list_tags.split_into_tags(block)
        self.assertCountEqual(expected, result)

    def test_block_with_tags_and_inline_comments(self):
        block = (("exclude_with_any_tags:\n"
                  "  - dummy_tag_1  # inline comment\n"
                  "  - dummy_tag_2  # another one\n"
                  "  - dummy_tag_3  # and another one"))
        expected = [
            ["- dummy_tag_1  # inline comment"],
            ["- dummy_tag_2  # another one"],
            ["- dummy_tag_3  # and another one"],
        ]
        result = list_tags.split_into_tags(block)
        self.assertCountEqual(expected, result)

    def test_block_with_tags_and_both_comments(self):
        block = ("exclude_with_any_tags:\n"
                 "  # above comment\n"
                 "  - dummy_tag_1  # inline comment\n"
                 "  # above comment line 1\n"
                 "  # above comment line 2\n"
                 "  - dummy_tag_2  # another one inline\n"
                 "  #######################\n"
                 "  # above fancy comment #\n"
                 "  #######################\n"
                 "  - dummy_tag_3  # and another one inline")
        expected = [
            [
                "# above comment",
                "- dummy_tag_1  # inline comment",
            ],
            [
                "# above comment line 1",
                "# above comment line 2",
                "- dummy_tag_2  # another one inline",
            ],
            [
                "#######################",
                "# above fancy comment #",
                "#######################",
                "- dummy_tag_3  # and another one inline",
            ],
        ]
        result = list_tags.split_into_tags(block)
        self.assertCountEqual(expected, result)


class TestGetTagDoc(unittest.TestCase):
    def test_empty_tag_block(self):
        expected = ("", "")
        result = list_tags.get_tag_doc("")
        self.assertEqual(expected, result)

    def test_tag_no_comment(self):
        tag_block = ["- dummy_tag"]
        expected = ("dummy_tag", "")
        result = list_tags.get_tag_doc(tag_block)
        self.assertEqual(expected, result)

    def test_tag_with_above_comment(self):
        tag_block = [
            "# comment",
            "- dummy_tag",
        ]
        expected = ("dummy_tag", "comment")
        result = list_tags.get_tag_doc(tag_block)
        self.assertEqual(expected, result)

    def test_tag_with_multiline_above_comment(self):
        tag_block = [
            "# comment line 1",
            "# comment line 2",
            "- dummy_tag",
        ]
        expected = ("dummy_tag", ("comment line 1\ncomment line 2"))
        result = list_tags.get_tag_doc(tag_block)
        self.assertEqual(expected, result)

    def test_tag_with_inline_comment(self):
        tag_block = ["- dummy_tag  # comment"]
        expected = ("dummy_tag", "comment")
        result = list_tags.get_tag_doc(tag_block)
        self.assertEqual(expected, result)

    def test_tag_with_both_comment(self):
        tag_block = [
            "# above comment line 1",
            "# above comment line 2",
            "- dummy_tag  # inline comment",
        ]
        expected = ("dummy_tag", ("above comment line 1\n"
                                  "above comment line 2\n"
                                  "inline comment"))
        result = list_tags.get_tag_doc(tag_block)
        self.assertEqual(expected, result)
