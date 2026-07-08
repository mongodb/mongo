#!/usr/bin/env python3
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.

import os
import re
from pathlib import Path
from wiredtiger import WiredTigerError
import wttest


class test_baseconfig(wttest.WiredTigerTestCase):
    """
    This file tests how the base config file is handled in different corruption scenarios.
    We expect the file to be renamed where possible, and an error message given in all cases.
    """

    def setUp(self):
        super().setUp()
        self.db_path = Path("test_baseconfig")
        self.db_path.mkdir()
        self.wiredtiger_open(str(self.db_path), "create").close()
        self.base_config_path = self.db_path / "WiredTiger.basecfg"
        self.bad_base_config_path = self.db_path / "WiredTiger.basecfg.bad"
        self.assertTrue(self.base_config_path.exists())
        self.assertFalse(self.bad_base_config_path.exists())

    def open_and_close_database(self, config=""):
        """Open the database and close it again."""
        self.wiredtiger_open(str(self.db_path), config).close()

    # The notification emitted when a corrupt base config is renamed out of the way.
    renamed_notification = (
        r"the bad base configuration file has been renamed from "
        r"'WiredTiger\.basecfg' to 'WiredTiger\.basecfg\.bad'"
    )

    def invalid_pattern(self, filename):
        """The error raised for an invalid base config."""
        return (
            r"the base configuration file '"
            + re.escape(filename)
            + r"' is invalid\. Either correct the file, or reopen with config_base=false"
        )

    def test_new_value_without_separator_is_renamed(self):
        """A raw tokenizer failure ("new value without a separator") causes a rename."""
        # Overwrite the first bytes of the file with garbage.
        with open(self.base_config_path, "r+", encoding="utf-8") as base_config:
            base_config.write("Bad!" * 4)

        self.assertRaisesWithMessage(
            WiredTigerError,
            lambda: self.wiredtiger_open(str(self.db_path), ""),
            "/New value starts without a separator/",
        )
        self.assertFalse(self.base_config_path.exists())
        self.assertTrue(self.bad_base_config_path.exists())

    def test_unknown_config_key_is_renamed(self):
        """An unrecognized key causes a rename."""
        # Seek to the end of the file and append garbage, producing a bare, unrecognized key.
        with open(self.base_config_path, "r+", encoding="utf-8") as base_config:
            base_config.seek(0, os.SEEK_END)
            base_config.write("Bad!" * 4)

        self.assertRaisesWithMessage(
            WiredTigerError,
            lambda: self.wiredtiger_open(str(self.db_path), ""),
            "/unknown configuration key/",
        )
        self.assertFalse(self.base_config_path.exists())
        self.assertTrue(self.bad_base_config_path.exists())

    def test_reopen_after_rename_succeeds(self):
        """A reopen after the rename succeeds from default settings."""
        # Seek to the end of the file and append garbage.
        with open(self.base_config_path, "r+", encoding="utf-8") as base_config:
            base_config.seek(0, os.SEEK_END)
            base_config.write("Bad!" * 4)

        self.assertRaisesWithMessage(
            WiredTigerError, lambda: self.wiredtiger_open(str(self.db_path), ""), "/.*/"
        )
        self.open_and_close_database()

    def test_config_base_false_skips_corrupt_file(self):
        """The advised recovery, config_base=false, opens without touching the corrupt file."""
        # Seek to the end of the file and append garbage.
        with open(self.base_config_path, "r+", encoding="utf-8") as base_config:
            base_config.seek(0, os.SEEK_END)
            base_config.write("Bad!" * 4)

        self.open_and_close_database("config_base=false")
        # The file is skipped, not parsed, so it is neither renamed nor removed.
        self.assertTrue(self.base_config_path.exists())
        self.assertFalse(self.bad_base_config_path.exists())

    def test_rename_is_reported(self):
        """When the file is renamed the user is notified"""
        # Seek to the end of the file and append garbage.
        with open(self.base_config_path, "r+", encoding="utf-8") as base_config:
            base_config.seek(0, os.SEEK_END)
            base_config.write("Bad!" * 4)

        # The rename is reported and the error names the renamed (.bad) file.
        with self.expectedStderrPattern(
            self.invalid_pattern("WiredTiger.basecfg.bad"),
            ignore_pat=wttest.WT_ERROR_LOG_PATTERN,
        ):
            self.assertRaises(
                WiredTigerError, lambda: self.wiredtiger_open(str(self.db_path), "")
            )
            self.assertIsNotNone(
                re.search(self.renamed_notification, self.readStderr())
            )

    def test_read_only_does_not_rename(self):
        """A read-only connection does not rename the file."""
        # Seek to the end of the file and append garbage.
        with open(self.base_config_path, "r+", encoding="utf-8") as base_config:
            base_config.seek(0, os.SEEK_END)
            base_config.write("Bad!" * 4)

        # Without a rename the error names the original file and no rename is reported.
        with self.expectedStderrPattern(
            self.invalid_pattern("WiredTiger.basecfg"),
            ignore_pat=wttest.WT_ERROR_LOG_PATTERN,
        ):
            self.assertRaises(
                WiredTigerError,
                lambda: self.wiredtiger_open(str(self.db_path), "readonly"),
            )
            self.assertIsNone(re.search(self.renamed_notification, self.readStderr()))

        self.assertTrue(self.base_config_path.exists())
        self.assertFalse(self.bad_base_config_path.exists())

    def test_unbalanced_bracket_is_renamed(self):
        """A raw syntax error causes a rename."""
        with open(self.base_config_path, "a", encoding="utf-8") as base_config:
            base_config.write("\nfoo=(bar\n")

        self.assertRaisesWithMessage(
            WiredTigerError,
            lambda: self.wiredtiger_open(str(self.db_path), ""),
            "/Unbalanced brackets/",
        )
        self.assertFalse(self.base_config_path.exists())
        self.assertTrue(self.bad_base_config_path.exists())

    def test_removal_is_tolerated(self):
        """A missing base config is treated the same as one that was never configured."""
        self.base_config_path.unlink()
        self.open_and_close_database()
        self.assertFalse(self.bad_base_config_path.exists())

    def test_truncate_middle_is_tolerated(self):
        """Truncating partway through the file does not fail the open."""
        size = self.base_config_path.stat().st_size
        with open(self.base_config_path, "r+", encoding="utf-8") as base_config:
            base_config.truncate(size // 2)

        self.open_and_close_database()
        self.assertFalse(self.bad_base_config_path.exists())

    def test_zero_at_start_is_tolerated(self):
        """Null bytes overwriting the start of the file, with real content surviving after
        them, does not fail the open."""
        # Overwrite only the first half of the file with null bytes, leaving the second half
        # intact.
        size = self.base_config_path.stat().st_size
        with open(self.base_config_path, "r+", encoding="utf-8") as base_config:
            base_config.write("\0" * (size // 2))

        self.open_and_close_database()
        self.assertFalse(self.bad_base_config_path.exists())

    def test_zero_truncated_is_tolerated(self):
        """A file replaced entirely with null bytes does not fail the open."""
        with open(self.base_config_path, "r+", encoding="utf-8") as base_config:
            base_config.truncate(0)
            base_config.write("\0" * 4096)

        self.open_and_close_database()
        self.assertFalse(self.bad_base_config_path.exists())

    def test_zero_at_end_is_tolerated(self):
        """Null bytes appended to the end of the file do not fail the open."""
        with open(self.base_config_path, "r+", encoding="utf-8") as base_config:
            base_config.seek(0, os.SEEK_END)
            base_config.write("\0" * 4096)

        self.open_and_close_database()
        self.assertFalse(self.bad_base_config_path.exists())

    def test_garbage_middle_is_tolerated(self):
        """Garbage in the middle of the file does not fail the open."""
        size = self.base_config_path.stat().st_size
        with open(self.base_config_path, "r+", encoding="utf-8") as base_config:
            base_config.seek(size // 2)
            base_config.write("Bad!" * 1024)

        self.open_and_close_database()
        self.assertFalse(self.bad_base_config_path.exists())

    def test_empty_basecfg_is_tolerated(self):
        """An empty base config is treated the same as a missing one."""
        with open(self.base_config_path, "r+", encoding="utf-8") as base_config:
            base_config.truncate(0)

        self.open_and_close_database()
        self.assertFalse(self.bad_base_config_path.exists())

    def test_unsupported_setting_is_not_renamed(self):
        """Unsupported settings do not trigger a rename."""
        with open(self.base_config_path, "a", encoding="utf-8") as base_config:
            base_config.write("\nchunk_cache=(enabled=true)\n")

        self.assertRaisesWithMessage(
            WiredTigerError,
            lambda: self.wiredtiger_open(str(self.db_path), ""),
            "/chunk cache has been deprecated/",
        )
        self.assertTrue(self.base_config_path.exists())
        self.assertFalse(self.bad_base_config_path.exists())

    def test_incompatible_version_is_not_renamed(self):
        """A base config from a newer release does not trigger a rename."""
        with open(self.base_config_path, "a", encoding="utf-8") as base_config:
            base_config.write("\nversion=(major=99,minor=0)\n")

        self.assertRaisesWithMessage(
            WiredTigerError,
            lambda: self.wiredtiger_open(str(self.db_path), ""),
            "/incompatible release/",
        )
        self.assertTrue(self.base_config_path.exists())
        self.assertFalse(self.bad_base_config_path.exists())

    def test_oversized_basecfg_is_not_renamed(self):
        """An oversized base config does not trigger a rename."""
        # Append enough content to push the file past the 100KB limit.
        with open(self.base_config_path, "a", encoding="utf-8") as base_config:
            base_config.write("#" + "x" * (101 * 1024) + "\n")

        self.assertRaisesWithMessage(
            WiredTigerError,
            lambda: self.wiredtiger_open(str(self.db_path), ""),
            "/Configuration file too big/",
        )
        self.assertTrue(self.base_config_path.exists())
        self.assertFalse(self.bad_base_config_path.exists())


if __name__ == "__main__":
    wttest.run()
