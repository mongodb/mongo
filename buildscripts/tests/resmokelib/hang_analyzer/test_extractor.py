import unittest

from buildscripts.resmokelib.hang_analyzer.extractor import get_dwarf_version


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


if __name__ == "__main__":
    unittest.main()
