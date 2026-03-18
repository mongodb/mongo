import unittest
from unittest.mock import Mock

from buildscripts.resmokelib.hang_analyzer.extractor import filter_core_dumps, get_dwarf_version


class TestExtractor(unittest.TestCase):
    def test_find_dwarf_version(self):
        """
        Test that DWARF version 5 is identified based on "format = DWARF32, version = 0x0005", and
        that the compile unit for the system library with DWARF version 2 is ignored.
        """

        # This is an excerpt from `llvm-dwarfdump -r 0 dist-test/bin/mongod.debug`
        dwarf_dump = """
dist-test/bin/mongod.debug:     file format elf64-x86-64

.debug_info contents:
0x00000000: Compile Unit: length = 0x00000041, format = DWARF32, version = 0x0002, abbr_offset = 0x0000, addr_size = 0x08 (next unit at 0x00000045)

0x0000000b: DW_TAG_compile_unit
              DW_AT_stmt_list   (0x00000000)
              DW_AT_low_pc      (0x00000000042e7200)
              DW_AT_high_pc     (0x00000000042e722b)
              DW_AT_name        ("../sysdeps/x86_64/start.S")
              DW_AT_comp_dir    ("/home/abuild/rpmbuild/BUILD/glibc-2.31/csu")
              DW_AT_producer    ("GNU AS 2.43.1")
              DW_AT_language    (DW_LANG_Mips_Assembler)
0x00000045: Compile Unit: length = 0x0000003b, format = DWARF32, version = 0x0004, abbr_offset = 0x0021, addr_size = 0x08 (next unit at 0x00000084)

0x00000050: DW_TAG_compile_unit
              DW_AT_producer    ("GNU C11 7.5.0 -mtune=generic -march=x86-64 -g -g -O2 -std=gnu11 -fgnu89-inline -fmessage-length=0 -funwind-tables -fasynchronous-unwind-tables -fstack-clash-protection -fmerge-all-constants -frounding-math -fstack-protector-strong -fpatchable-function-entry=16,14 -fmath-errno -fno-stack-protector -ftls-model=initial-exec -fPIE")
              DW_AT_language    (DW_LANG_C99)
              DW_AT_name        ("init.c")
              DW_AT_comp_dir    ("/home/abuild/rpmbuild/BUILD/glibc-2.31/csu")
              DW_AT_stmt_list   (0x00000059)
0x00000084: Compile Unit: length = 0x0000001e, format = DWARF32, version = 0x0002, abbr_offset = 0x0054, addr_size = 0x08 (next unit at 0x000000a6)

0x0000008f: DW_TAG_compile_unit
              DW_AT_stmt_list   (0x00000080)
              DW_AT_ranges      (0x00000000
                 [0x000000000680fe04, 0x000000000680fe16)
                 [0x000000000680fe1c, 0x000000000680fe20))
              DW_AT_name        ("../sysdeps/x86_64/crti.S")
              DW_AT_comp_dir    ("/home/abuild/rpmbuild/BUILD/glibc-2.31/csu")
              DW_AT_producer    ("GNU AS 2.43.1")
              DW_AT_language    (DW_LANG_Mips_Assembler)
0x000000a6: Compile Unit: length = 0x0000445e, format = DWARF32, version = 0x0005, unit_type = DW_UT_compile, abbr_offset = 0x0066, addr_size = 0x08 (next unit at 0x00004508)

0x000000b2: DW_TAG_compile_unit
              DW_AT_producer    ("MongoDB clang version 19.1.7 (https://x-access-token:ghs_abc123@github.com/10gen/toolchain-builder.git 868b3c714c67f4f245623d2f772d0fac95d936a2)")
              DW_AT_language    (DW_LANG_C_plus_plus_14)
              DW_AT_name        ("src/mongo/db/mongod.cpp")
              DW_AT_str_offsets_base    (0x00000008)
              DW_AT_stmt_list   (0x000000e2)
              DW_AT_comp_dir    (".")
              DW_AT_low_pc      (0x0000000000000000)
              DW_AT_ranges      (indexed (0x0) rangelist = 0x00000010
                 [0x0000000003b0ddb0, 0x0000000003b0ddc0)
                 [0x0000000003b0ddc0, 0x0000000003b0ddc9)
                 [0x0000000003b0b6ad, 0x0000000003b0b6d4)
                 [0x0000000003b0b6e0, 0x0000000003b0b71f))
              DW_AT_addr_base   (0x00000008)
              DW_AT_rnglists_base       (0x0000000c)
              DW_AT_loclists_base       (0x0000000c)
"""

        self.assertEqual(get_dwarf_version(dwarf_dump), 5)


class TestFilterCoreDumpsHelper(unittest.TestCase):
    """Unit tests for the filter_core_dumps helper function."""

    def _artifact(self, filename):
        return Mock(url=f"https://s3.example.com/artifacts/{filename}")

    def test_no_filtering_when_no_boring_pids(self):
        """Test that all cores are returned when no boring PIDs provided."""
        artifacts = [
            self._artifact("dump_mongod.123.core.gz"),
            self._artifact("dump_mongos.456.core.gz"),
        ]
        logger = Mock()

        result = filter_core_dumps(artifacts, None, 50, logger)

        self.assertEqual(result, artifacts)

    def test_filters_out_boring_pids(self):
        """Test that cores with boring PIDs are filtered out."""
        a1 = self._artifact("dump_mongod.123.core.gz")  # boring
        a2 = self._artifact("dump_mongos.456.core.gz")  # interesting
        a3 = self._artifact("dump_mongod.789.core.gz")  # boring
        boring_pids = {"123", "789"}
        logger = Mock()

        result = filter_core_dumps([a1, a2, a3], boring_pids, 50, logger)

        self.assertEqual(len(result), 1)
        self.assertIn(a2, result)

    def test_applies_cap(self):
        """Test that maximum cap is applied."""
        artifacts = [self._artifact(f"dump_mongod.{i}.core.gz") for i in range(100)]
        logger = Mock()

        result = filter_core_dumps(artifacts, None, 20, logger)

        self.assertEqual(len(result), 20)

    def test_filter_then_cap(self):
        """Test that filtering happens before capping."""
        interesting = [self._artifact(f"dump_mongod.{i}.core.gz") for i in range(10)]
        boring = [self._artifact(f"dump_mongos.{i + 100}.core.gz") for i in range(10)]
        boring_pids = {str(i + 100) for i in range(10)}
        logger = Mock()

        result = filter_core_dumps(interesting + boring, boring_pids, 5, logger)

        self.assertEqual(len(result), 5)
        for artifact in result:
            self.assertIn(artifact, interesting)

    def test_unparseable_filenames_treated_as_interesting(self):
        """Test that cores with unparseable names are kept."""
        a1 = self._artifact("dump_mongod.123.core.gz")  # boring, parseable
        a2 = self._artifact("weird_name.core.gz")  # unparseable -> interesting
        a3 = self._artifact("another.file.core.gz")  # unparseable -> interesting
        boring_pids = {"123"}
        logger = Mock()

        result = filter_core_dumps([a1, a2, a3], boring_pids, 50, logger)

        self.assertEqual(len(result), 2)
        self.assertIn(a2, result)
        self.assertIn(a3, result)

    def test_empty_result_after_filtering(self):
        """Test handling when all cores are filtered out."""
        a1 = self._artifact("dump_mongod.123.core.gz")
        a2 = self._artifact("dump_mongos.456.core.gz")
        boring_pids = {"123", "456"}
        logger = Mock()

        result = filter_core_dumps([a1, a2], boring_pids, 50, logger)

        self.assertEqual(len(result), 0)


if __name__ == "__main__":
    unittest.main()
