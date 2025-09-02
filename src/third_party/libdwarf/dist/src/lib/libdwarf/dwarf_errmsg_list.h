/*
Copyright (C) 2000-2005 Silicon Graphics, Inc. All Rights Reserved.
Portions Copyright (C) 2008-2023 David Anderson.  All Rights Reserved.

  This program is free software; you can redistribute it
  and/or modify it under the terms of version 2.1 of the
  GNU Lesser General Public License as published by the Free
  Software Foundation.

  This program is distributed in the hope that it would be
  useful, but WITHOUT ANY WARRANTY; without even the implied
  warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
  PURPOSE.

  Further, this software is distributed without any warranty
  that it is free of the rightful claim of any third person
  regarding infringement or the like.  Any license provided
  herein, whether implied or otherwise, applies only to this
  software file.  Patent licenses, if any, provided herein
  do not apply to combinations of this program with other
  software, or any other product whatsoever.

  You should have received a copy of the GNU Lesser General
  Public License along with this program; if not, write the
  Free Software Foundation, Inc., 51 Franklin Street - Fifth
  Floor, Boston MA 02110-1301, USA.

*/
#ifndef DWARF_ERRMSG_LIST_H
#define DWARF_ERRMSG_LIST_H

/* Array to hold string representation of errors. Any time a
   define is added to the list in libdwarf.h, a string should be
   added to this Array

   Errors in the list (missing a comma, for example)
   happen too often. Making this a separate little file
   simplifies testing for missing-commas/extra-strings.

   Using an array table instead of
   pointers saves DW_DLE_LAST+1 relocations at runtime
   for libdwarf as DSO (libdwarf.so).
*/
#define DW_MAX_MSG_LEN 111
static const char _dwarf_errmsgs[DW_DLE_LAST+1][DW_MAX_MSG_LEN] = {
{"DW_DLE_No error (0)\n"},
{"DW_DLE_VMM (1) dwarf format/library version mismatch"},
{"DW_DLE_MAP (2) memory map failure"},
{"DW_DLE_LEE (3) libelf error"},
{"DW_DLE_NDS (4) no debug section"},
{"DW_DLE_NLS (5) no line section "},
{"DW_DLE_ID (6) invalid descriptor for query "},
{"DW_DLE_IOF (7) I/O failure "},
{"DW_DLE_MAF (8) memory allocation failure "},
{"DW_DLE_IA (9) invalid argument "},
{"DW_DLE_MDE (10) mangled debugging entry:libelf detected error"},
{"DW_DLE_MLE (11) mangled line number entry "},
{"DW_DLE_FNO (12) file not open "},
{"DW_DLE_FNR (13) file not a regular file "},
{"DW_DLE_FWA (14) file open with wrong access "},
{"DW_DLE_NOB (15) not an object file "},
{"DW_DLE_MOF (16) mangled object file header "},
{"DW_DLE_EOLL (17) end of location list entries "},
{"DW_DLE_NOLL (18) no location list section "},
{"DW_DLE_BADOFF (19) Invalid offset "},
{"DW_DLE_EOS (20) end of section  "},
{"DW_DLE_ATRUNC (21) abbreviations section appears truncated"},
{"DW_DLE_BADBITC (22)  Address size passed to dwarf bad"},

{"DW_DLE_DBG_ALLOC (23) Unable to malloc a Dwarf_Debug structure"},
{"DW_DLE_FSTAT_ERROR (24) The file fd passed to dwarf_init "
    "cannot be fstat()ed"},
{"DW_DLE_FSTAT_MODE_ERROR (25) The file mode bits say not "
    "a normal file"},
{"DW_DLE_INIT_ACCESS_WRONG (26) A call to dwarf_init failed,"
    " this error impossible as of July 2021"},
{"DW_DLE_ELF_BEGIN_ERROR (27) a call to "
    "elf_begin(... ELF_C_READ_MMAP... ) failed"},
{"DW_DLE_ELF_GETEHDR_ERROR (28) a call to "
    "elf32_getehdr() or elf64_getehdr() failed"},
{"DW_DLE_ELF_GETSHDR_ERROR (29) a call to "
    "elf32_getshdr() or elf64_getshdr() failed"},
{"DW_DLE_ELF_STRPTR_ERROR (30) a call to "
    "elf_strptr() failed trying to get a section name"},
{"DW_DLE_DEBUG_INFO_DUPLICATE (31)  Only one .debug_info  "
    "section is allowed"},
{"DW_DLE_DEBUG_INFO_NULL (32) .debug_info section present but "
    "elf_getdata() failed"},
{"DW_DLE_DEBUG_ABBREV_DUPLICATE (33) Only one .debug_abbrev  "
    "section is allowed"},
{"DW_DLE_DEBUG_ABBREV_NULL (34) .debug_abbrev section present but "
    "elf_getdata() failed"},
{"DW_DLE_DEBUG_ARANGES_DUPLICATE (35) Only one .debug_aranges  "
    "section is allowed"},
{"DW_DLE_DEBUG_ARANGES_NULL (36) .debug_aranges section present but "
    "elf_getdata() failed"},
{"DW_DLE_DEBUG_LINE_DUPLICATE (37) Only one .debug_line  "
    "section is allowed"},
{"DW_DLE_DEBUG_LINE_NULL (38) .debug_line section 0-size. Corrupt."},
{"DW_DLE_DEBUG_LOC_DUPLICATE (39) Only one .debug_loc  "
    "section is allowed"},
{"DW_DLE_DEBUG_LOC_NULL (40) .debug_loc section present but "
    "elf_getdata() failed"},
{"DW_DLE_DEBUG_MACINFO_DUPLICATE (41) Only one .debug_macinfo  "
    "section is allowed"},
{"DW_DLE_DEBUG_MACINFO_NULL (42) .debug_macinfo section present but "
    "elf_getdata() failed"},
{"DW_DLE_DEBUG_PUBNAMES_DUPLICATE (43) Only one .debug_pubnames  "
    "section is allowed"},
{"DW_DLE_DEBUG_PUBNAMES_NULL (44) .debug_pubnames section "
    "present but elf_getdata() failed"},
{"DW_DLE_DEBUG_STR_DUPLICATE (45)  Only one .debug_str  "
    "section is allowed"},
{"DW_DLE_DEBUG_STR_NULL (46) .debug_str section present but "
    "elf_getdata() failed"},
{"DW_DLE_CU_LENGTH_ERROR (47) Corrupted DWARF or corrupted object"},
{"DW_DLE_VERSION_STAMP_ERROR (48) Corrupted DWARF "
    "or corrupted object"},
{"DW_DLE_ABBREV_OFFSET_ERROR (49) Corrupted DWARF or "
    "corrupted object"},
{"DW_DLE_ADDRESS_SIZE_ERROR (50) size too large"},
{"DW_DLE_DEBUG_INFO_PTR_NULL (51)"},
{"DW_DLE_DIE_NULL (52)"},
{"DW_DLE_STRING_OFFSET_BAD (53) Corrupted DWARF or corrupted object"},
{"DW_DLE_DEBUG_LINE_LENGTH_BAD (54)"},
{"DW_DLE_LINE_PROLOG_LENGTH_BAD (55)"},
{"DW_DLE_LINE_NUM_OPERANDS_BAD (56)"},
{"DW_DLE_LINE_SET_ADDR_ERROR (57)"},
{"DW_DLE_LINE_EXT_OPCODE_BAD (58)"},
{"DW_DLE_DWARF_LINE_NULL (59)"},
{"DW_DLE_INCL_DIR_NUM_BAD (60)"},
{"DW_DLE_LINE_FILE_NUM_BAD (61)"},
{"DW_DLE_ALLOC_FAIL (62) Out of memory or corrupted object"},
{"DW_DLE_NO_CALLBACK_FUNC (63)"},
{"DW_DLE_SECT_ALLOC (64)"},
{"DW_DLE_FILE_ENTRY_ALLOC (65)"},
{"DW_DLE_LINE_ALLOC (66)"},
{"DW_DLE_FPGM_ALLOC (67)"},
{"DW_DLE_INCDIR_ALLOC (68)"},
{"DW_DLE_STRING_ALLOC (69)"},
{"DW_DLE_CHUNK_ALLOC (70)"},
{"DW_DLE_BYTEOFF_ERR (71)"},
{"DW_DLE_CIE_ALLOC (72)"},
{"DW_DLE_FDE_ALLOC (73)"},
{"DW_DLE_REGNO_OVFL (74)"},
{"DW_DLE_CIE_OFFS_ALLOC (75)"},
{"DW_DLE_WRONG_ADDRESS (76)"},
{"DW_DLE_EXTRA_NEIGHBORS (77)"},
{"DW_DLE_WRONG_TAG (78)"},
{"DW_DLE_DIE_ALLOC (79)"},
{"DW_DLE_PARENT_EXISTS (80)"},
{"DW_DLE_DBG_NULL (81)"},
{"DW_DLE_DEBUGLINE_ERROR (82)"},
{"DW_DLE_DEBUGFRAME_ERROR (83)"},
{"DW_DLE_DEBUGINFO_ERROR (84)"},
{"DW_DLE_ATTR_ALLOC (85)"},
{"DW_DLE_ABBREV_ALLOC (86)"},
{"DW_DLE_OFFSET_UFLW (87)"},
{"DW_DLE_ELF_SECT_ERR (88)"},
{"DW_DLE_DEBUG_FRAME_LENGTH_BAD (89)"},
{"DW_DLE_FRAME_VERSION_BAD (90)"},
{"DW_DLE_CIE_RET_ADDR_REG_ERROR (91)"},
{"DW_DLE_FDE_NULL (92)"},
{"DW_DLE_FDE_DBG_NULL (93)"},
{"DW_DLE_CIE_NULL (94)"},
{"DW_DLE_CIE_DBG_NULL (95)"},
{"DW_DLE_FRAME_TABLE_COL_BAD (96)"},
{"DW_DLE_PC_NOT_IN_FDE_RANGE (97)"},
{"DW_DLE_CIE_INSTR_EXEC_ERROR (98)"},
{"DW_DLE_FRAME_INSTR_EXEC_ERROR (99)"},
{"DW_DLE_FDE_PTR_NULL (100)"},
{"DW_DLE_RET_OP_LIST_NULL (101)"},
{"DW_DLE_LINE_CONTEXT_NULL (102)"},
{"DW_DLE_DBG_NO_CU_CONTEXT (103)"},
{"DW_DLE_DIE_NO_CU_CONTEXT (104)"},
{"DW_DLE_FIRST_DIE_NOT_CU (105)"},
{"DW_DLE_NEXT_DIE_PTR_NULL (106)"},
{"DW_DLE_DEBUG_FRAME_DUPLICATE  (107) Only one .debug_frame  "
    "section is allowed"},
{"DW_DLE_DEBUG_FRAME_NULL (108) .debug_frame section present but "
    "elf_getdata() failed"},
{"DW_DLE_ABBREV_DECODE_ERROR (109)"},
{"DW_DLE_DWARF_ABBREV_NULL (110)"},
{"DW_DLE_ATTR_NULL (111)"},
{"DW_DLE_DIE_BAD (112)"},
{"DW_DLE_DIE_ABBREV_BAD (113)"},
{"DW_DLE_ATTR_FORM_BAD (114)"},
{"DW_DLE_ATTR_NO_CU_CONTEXT (115)"},
{"DW_DLE_ATTR_FORM_SIZE_BAD (116)"},
{"DW_DLE_ATTR_DBG_NULL (117)"},
{"DW_DLE_BAD_REF_FORM (118)"},
{"DW_DLE_ATTR_FORM_OFFSET_BAD (119)"},
{"DW_DLE_LINE_OFFSET_BAD (120)"},
{"DW_DLE_DEBUG_STR_OFFSET_BAD (121)"},
{"DW_DLE_STRING_PTR_NULL (122)"},
{"DW_DLE_PUBNAMES_VERSION_ERROR (123)"},
{"DW_DLE_PUBNAMES_LENGTH_BAD (124)"},
{"DW_DLE_GLOBAL_NULL (125)"},
{"DW_DLE_GLOBAL_CONTEXT_NULL (126)"},
{"DW_DLE_DIR_INDEX_BAD (127)"},
{"DW_DLE_LOC_EXPR_BAD (128)"},
{"DW_DLE_DIE_LOC_EXPR_BAD (129)"},
{"DW_DLE_ADDR_ALLOC (130)"},
{"DW_DLE_OFFSET_BAD (131)"},
{"DW_DLE_MAKE_CU_CONTEXT_FAIL (132)"},
{"DW_DLE_REL_ALLOC (133)"},
{"DW_DLE_ARANGE_OFFSET_BAD (134)"},
{"DW_DLE_SEGMENT_SIZE_BAD (135) Size of a segment selector "
    "should usually be less than 8 (bytes)."},
{"DW_DLE_ARANGE_LENGTH_BAD (136)"},
{"DW_DLE_ARANGE_DECODE_ERROR (137)"},
{"DW_DLE_ARANGES_NULL (138)"},
{"DW_DLE_ARANGE_NULL (139)"},
{"DW_DLE_NO_FILE_NAME (140)"},
{"DW_DLE_NO_COMP_DIR (141)"},
{"DW_DLE_CU_ADDRESS_SIZE_BAD (142)"},
{"DW_DLE_INPUT_ATTR_BAD (143)"},
{"DW_DLE_EXPR_NULL (144)"},
{"DW_DLE_BAD_EXPR_OPCODE (145)"},
{"DW_DLE_EXPR_LENGTH_BAD (146)"},
{"DW_DLE_MULTIPLE_RELOC_IN_EXPR (147)"},
{"DW_DLE_ELF_GETIDENT_ERROR (148)"},
{"DW_DLE_NO_AT_MIPS_FDE (149)"},
{"DW_DLE_NO_CIE_FOR_FDE (150)"},
{"DW_DLE_DIE_ABBREV_LIST_NULL (151) No abbrev exists for "
    "the requested abbrev code"},
{"DW_DLE_DEBUG_FUNCNAMES_DUPLICATE (152)"},
{"DW_DLE_DEBUG_FUNCNAMES_NULL (153) .debug_funcnames section "
    "present but elf_getdata() bad"},
{"DW_DLE_DEBUG_FUNCNAMES_VERSION_ERROR (154)"},
{"DW_DLE_DEBUG_FUNCNAMES_LENGTH_BAD (155)"},
{"DW_DLE_FUNC_NULL (156)"},
{"DW_DLE_FUNC_CONTEXT_NULL (157)"},
{"DW_DLE_DEBUG_TYPENAMES_DUPLICATE (158)"},
{"DW_DLE_DEBUG_TYPENAMES_NULL (159) .debug_typenames section "
    "present but elf_getdata() failed"},
{"DW_DLE_DEBUG_TYPENAMES_VERSION_ERROR (160)"},
{"DW_DLE_DEBUG_TYPENAMES_LENGTH_BAD (161)"},
{"DW_DLE_TYPE_NULL (162)"},
{"DW_DLE_TYPE_CONTEXT_NULL (163)"},
{"DW_DLE_DEBUG_VARNAMES_DUPLICATE (164)"},
{"DW_DLE_DEBUG_VARNAMES_NULL (165) .debug_varnames section present "
    "but elf_getdata() failed"},
{"DW_DLE_DEBUG_VARNAMES_VERSION_ERROR (166)"},
{"DW_DLE_DEBUG_VARNAMES_LENGTH_BAD (167)"},
{"DW_DLE_VAR_NULL (168)"},
{"DW_DLE_VAR_CONTEXT_NULL (169)"},
{"DW_DLE_DEBUG_WEAKNAMES_DUPLICATE (170)"},
{"DW_DLE_DEBUG_WEAKNAMES_NULL (171) .debug_weaknames section "
    "present but elf_getdata() failed"},

{"DW_DLE_DEBUG_WEAKNAMES_VERSION_ERROR (172)"},
{"DW_DLE_DEBUG_WEAKNAMES_LENGTH_BAD (173)"},
{"DW_DLE_WEAK_NULL (174)"},
{"DW_DLE_WEAK_CONTEXT_NULL (175)"},
{"DW_DLE_LOCDESC_COUNT_WRONG (176)"},
{"DW_DLE_MACINFO_STRING_NULL (177)"},
{"DW_DLE_MACINFO_STRING_EMPTY (178)"},
{"DW_DLE_MACINFO_INTERNAL_ERROR_SPACE (179)"},
{"DW_DLE_MACINFO_MALLOC_FAIL (180)"},
{"DW_DLE_DEBUGMACINFO_ERROR (181)"},
{"DW_DLE_DEBUG_MACRO_LENGTH_BAD (182) in .debug_macinfo"},
{"DW_DLE_DEBUG_MACRO_MAX_BAD (183) in .debug_macinfo"},
{"DW_DLE_DEBUG_MACRO_INTERNAL_ERR (184) in .debug_macinfo"},
{"DW_DLE_DEBUG_MACRO_MALLOC_SPACE (185) in .debug_macinfo"},
{"DW_DLE_DEBUG_MACRO_INCONSISTENT (186) in .debug_macinfo"},
{"DW_DLE_DF_NO_CIE_AUGMENTATION(187)"},
{"DW_DLE_DF_REG_NUM_TOO_HIGH(188)"},
{"DW_DLE_DF_MAKE_INSTR_NO_INIT(189)"},
{"DW_DLE_DF_NEW_LOC_LESS_OLD_LOC(190)"},
{"DW_DLE_DF_POP_EMPTY_STACK(191)"},
{"DW_DLE_DF_ALLOC_FAIL(192)"},
{"DW_DLE_DF_FRAME_DECODING_ERROR(193)"},
{"DW_DLE_DEBUG_LOC_SECTION_SHORT(194)"},
{"DW_DLE_FRAME_AUGMENTATION_UNKNOWN(195)"},
{"DW_DLE_PUBTYPE_CONTEXT(196)"},
{"DW_DLE_DEBUG_PUBTYPES_LENGTH_BAD(197)"},
{"DW_DLE_DEBUG_PUBTYPES_VERSION_ERROR(198)"},
{"DW_DLE_DEBUG_PUBTYPES_DUPLICATE(199)"},
{"DW_DLE_FRAME_CIE_DECODE_ERROR(200)"},
{"DW_DLE_FRAME_REGISTER_UNREPRESENTABLE(201)"},
{"DW_DLE_FRAME_REGISTER_COUNT_MISMATCH(202)"},
{"DW_DLE_LINK_LOOP(203)"},
{"DW_DLE_STRP_OFFSET_BAD(204)"},
{"DW_DLE_DEBUG_RANGES_DUPLICATE(205)"},
{"DW_DLE_DEBUG_RANGES_OFFSET_BAD(206)"},
{"DW_DLE_DEBUG_RANGES_MISSING_END(207)"},
{"DW_DLE_DEBUG_RANGES_OUT_OF_MEM(208)"},
{"DW_DLE_DEBUG_SYMTAB_ERR(209)"},
{"DW_DLE_DEBUG_STRTAB_ERR(210)"},
{"DW_DLE_RELOC_MISMATCH_INDEX(211)"},
{"DW_DLE_RELOC_MISMATCH_RELOC_INDEX(212)"},
{"DW_DLE_RELOC_MISMATCH_STRTAB_INDEX(213)"},
{"DW_DLE_RELOC_SECTION_MISMATCH(214)"},
{"DW_DLE_RELOC_SECTION_MISSING_INDEX(215)"},
{"DW_DLE_RELOC_SECTION_LENGTH_ODD(216)"},
{"DW_DLE_RELOC_SECTION_PTR_NULL(217)"},
{"DW_DLE_RELOC_SECTION_MALLOC_FAIL(218)"},
{"DW_DLE_NO_ELF64_SUPPORT(219)"},
{"DW_DLE_MISSING_ELF64_SUPPORT(220)"},
{"DW_DLE_ORPHAN_FDE(221)"},
{"DW_DLE_DUPLICATE_INST_BLOCK(222)"},
{"DW_DLE_BAD_REF_SIG8_FORM(223)"},
{"DW_DLE_ATTR_EXPRLOC_FORM_BAD(224)"},
{"DW_DLE_FORM_SEC_OFFSET_LENGTH_BAD(225)"},
{"DW_DLE_NOT_REF_FORM(226)"},
{"DW_DLE_DEBUG_FRAME_LENGTH_NOT_MULTIPLE(227)"},
{"DW_DLE_REF_SIG8_NOT_HANDLED (228)"},
{"DW_DLE_DEBUG_FRAME_POSSIBLE_ADDRESS_BOTCH (229)"},
{"DW_DLE_LOC_BAD_TERMINATION (230) location operator "
    "in expression missing data"},
{"DW_DLE_SYMTAB_SECTION_LENGTH_ODD (231) so doing "
    "relocations seems unsafe"},
{"DW_DLE_RELOC_SECTION_SYMBOL_INDEX_BAD (232) so doing a "
    "relocation seems unsafe"},
{"DW_DLE_RELOC_SECTION_RELOC_TARGET_SIZE_UNKNOWN (233) so "
    "doing a relocation is unsafe"},
{"DW_DLE_SYMTAB_SECTION_ENTRYSIZE_ZERO(234)"},
{"DW_DLE_LINE_NUMBER_HEADER_ERROR (235), a line number "
    "program header seems incomplete"},
{"DW_DLE_DEBUG_TYPES_NULL (236)"},
{"DW_DLE_DEBUG_TYPES_DUPLICATE (237)"},
{"DW_DLE_DEBUG_TYPES_ONLY_DWARF4 (238) DW4 and DW5 have types CUs"},
{"DW_DLE_DEBUG_TYPEOFFSET_BAD (239)"},
{"DW_DLE_GNU_OPCODE_ERROR (240)"},
{"DW_DLE_DEBUGPUBTYPES_ERROR (241), could not create "
    "pubtypes section"},
{"DW_DLE_AT_FIXUP_NULL (242)"},
{"DW_DLE_AT_FIXUP_DUP (243)"},
{"DW_DLE_BAD_ABINAME (244)"},
{"DW_DLE_TOO_MANY_DEBUG(245), too many .debug_* sections "
    "present somehow"},
{"DW_DLE_DEBUG_STR_OFFSETS_DUPLICATE(246)"},
{"DW_DLE_SECTION_DUPLICATION(247)"},
{"DW_DLE_SECTION_ERROR(248)"},
{"DW_DLE_DEBUG_ADDR_DUPLICATE(249)"},
{"DW_DLE_DEBUG_CU_UNAVAILABLE_FOR_FORM(250)"},
{"DW_DLE_DEBUG_FORM_HANDLING_INCOMPLETE(251)"},
{"DW_DLE_NEXT_DIE_PAST_END(252)"},
{"DW_DLE_NEXT_DIE_WRONG_FORM(253)"},
{"DW_DLE_NEXT_DIE_NO_ABBREV_LIST(254)"},
{"DW_DLE_NESTED_FORM_INDIRECT_ERROR(255)"},
{"DW_DLE_CU_DIE_NO_ABBREV_LIST(256)"},
{"DW_DLE_MISSING_NEEDED_DEBUG_ADDR_SECTION(257)"},
{"DW_DLE_ATTR_FORM_NOT_ADDR_INDEX(258)"},
{"DW_DLE_ATTR_FORM_NOT_STR_INDEX(259)"},
{"DW_DLE_DUPLICATE_GDB_INDEX(260)"},
{"DW_DLE_ERRONEOUS_GDB_INDEX_SECTION(261) The section is too small"},
{"DW_DLE_GDB_INDEX_COUNT_ERROR(262)"},
{"DW_DLE_GDB_INDEX_COUNT_ADDR_ERROR(263)"},
{"DW_DLE_GDB_INDEX_CUVEC_ERROR(264)"},
{"DW_DLE_GDB_INDEX_INDEX_ERROR(265)"},
{"DW_DLE_DUPLICATE_CU_INDEX(266)"},
{"DW_DLE_DUPLICATE_TU_INDEX(267)"},
{"DW_DLE_XU_TYPE_ARG_ERROR(268) XU means dwarf_cu_ or "
    "tu_ index section"},
{"DW_DLE_XU_IMPOSSIBLE_ERROR(269) XU means dwarf_cu_ or "
    "tu_ index section"},
{"DW_DLE_XU_NAME_COL_ERROR(270) XU means dwarf_cu_ or "
    "tu_ index section"},
{"DW_DLE_XU_HASH_ROW_ERROR(271) XU means dwarf_cu_ or "
    "tu_ index section"},
{"DW_DLE_XU_HASH_INDEX_ERROR(272) XU means dwarf_cu_ or "
    "tu_ index section"},
{"DW_DLE_FAILSAFE_ERRVAL(273)"},
{"DW_DLE_ARANGE_ERROR(274) producer problem in object generation"},
{"DW_DLE_PUBNAMES_ERROR(275) producer problem in object generation"},
{"DW_DLE_FUNCNAMES_ERROR(276) producer problem in object generation"},
{"DW_DLE_TYPENAMES_ERROR(277) producer problem in object generation"},
{"DW_DLE_VARNAMES_ERROR(278) producer problem in object generation"},
{"DW_DLE_WEAKNAMES_ERROR(279) producer problem in object generation"},
{"DW_DLE_RELOCS_ERROR(280) producer problem in object generation"},
{"DW_DLE_DW_DLE_ATTR_OUTSIDE_SECTION(281)"},
{"DW_DLE_FISSION_INDEX_WRONG(282)"},
{"DW_DLE_FISSION_VERSION_ERROR(283)"},
{"DW_DLE_NEXT_DIE_LOW_ERROR(284) corrupted DIE tree"},
{"DW_DLE_CU_UT_TYPE_ERROR(285) bad DW_UT_* value, corrupt DWARF5"},
{"DW_DLE_NO_SUCH_SIGNATURE_FOUND(286) CU signature not in the index"},
{"DW_DLE_SIGNATURE_SECTION_NUMBER_WRONG(287) "
    "libdwarf software error"},
{"DW_DLE_ATTR_FORM_NOT_DATA8(288) wanted an 8 byte signature"},
{"DW_DLE_SIG_TYPE_WRONG_STRING (289) expected tu or cu"},
{"DW_DLE_MISSING_REQUIRED_TU_OFFSET_HASH(290) is a "
    "broken dwp package file"},
{"DW_DLE_MISSING_REQUIRED_CU_OFFSET_HASH(291) is a "
    "broken dwp package file"},
{"DW_DLE_DWP_MISSING_DWO_ID(292)"},
{"DW_DLE_DWP_SIBLING_ERROR(293)"},
{"DW_DLE_DEBUG_FISSION_INCOMPLETE(294)"},
{"DW_DLE_FISSION_SECNUM_ERR(295) internal libdwarf error"},
{"DW_DLE_DEBUG_MACRO_DUPLICATE(296)"},
{"DW_DLE_DEBUG_NAMES_DUPLICATE(297)"},
{"DW_DLE_DEBUG_LINE_STR_DUPLICATE(298)"},
{"DW_DLE_DEBUG_SUP_DUPLICATE(299)"},
{"DW_DLE_NO_SIGNATURE_TO_LOOKUP(300)"},
{"DW_DLE_NO_TIED_ADDR_AVAILABLE(301)"},
{"DW_DLE_NO_TIED_SIG_AVAILABLE(302)"},
{"DW_DLE_STRING_NOT_TERMINATED(303) section data may be corrupted"},
{"DW_DLE_BAD_LINE_TABLE_OPERATION(304) two-level line table botch"},
{"DW_DLE_LINE_CONTEXT_BOTCH(305) call is wrong or memory corruption"},
{"DW_DLE_LINE_CONTEXT_INDEX_WRONG(306)"},
{"DW_DLE_NO_TIED_STRING_AVAILABLE(307) tied file does not "
    "have the string"},
{"DW_DLE_NO_TIED_FILE_AVAILABLE(308) see dwarf_set_tied_dbg()"},
{"DW_DLE_CU_TYPE_MISSING(309) libdwarf bug or data corruption"},
{"DW_DLE_LLE_CODE_UNKNOWN (310) libdwarf bug or data corruption"},
{"DW_DLE_LOCLIST_INTERFACE_ERROR (311) interface cannot do "
    "location or DW_OP*"},
{"DW_DLE_LOCLIST_INDEX_ERROR (312)"},
{"DW_DLE_INTERFACE_NOT_SUPPORTED (313)"},
{"DW_DLE_ZDEBUG_REQUIRES_ZLIB (314) Unable to decompress .zdebug "
    "as zlib missing"},
{"DW_DLE_ZDEBUG_INPUT_FORMAT_ODD(315)"},
{"DW_DLE_ZLIB_BUF_ERROR (316) Z_BUF_ERROR buffer size small"},
{"DW_DLE_ZLIB_DATA_ERROR (317) Z_DATA_ERROR compressed "
    "data corrupted"},
{"DW_DLE_MACRO_OFFSET_BAD (318)"},
{"DW_DLE_MACRO_OPCODE_BAD (319)"},
{"DW_DLE_MACRO_OPCODE_FORM_BAD (320)"},
{"DW_DLE_UNKNOWN_FORM (321) Possibly corrupt DWARF data"},
{"DW_DLE_BAD_MACRO_HEADER_POINTER(322)"},
{"DW_DLE_BAD_MACRO_INDEX(323)"},
{"DW_DLE_MACRO_OP_UNHANDLED(324) Possibly an implementation "
    "extension"},
{"DW_DLE_MACRO_PAST_END(325)"},
{"DW_DLE_LINE_STRP_OFFSET_BAD(326)"},
{"DW_DLE_STRING_FORM_IMPROPER(327) An internal libdwarf logic error"},
{"DW_DLE_ELF_FLAGS_NOT_AVAILABLE(328) elf/non-elf object confusion?"},
{"DW_DLE_LEB_IMPROPER (329) Runs off end of section or CU"},
{"DW_DLE_DEBUG_LINE_RANGE_ZERO (330) Corrupted line section"},
{"DW_DLE_READ_LITTLEENDIAN_ERROR (331) Corrupted dwarfdata "
    "littleendian host"},
{"DW_DLE_READ_BIGENDIAN_ERROR (332) Corrupted dwarf data "
    "bigendian host"},
{"DW_DLE_RELOC_INVALID (333) relocation corruption"},
{"DW_DLE_INFO_HEADER_ERROR(334) Corrupt dwarf"},
{"DW_DLE_ARANGES_HEADER_ERROR(335) Corrupt dwarf"},
{"DW_DLE_LINE_OFFSET_WRONG_FORM(336) Corrupt dwarf"},
{"DW_DLE_FORM_BLOCK_LENGTH_ERROR(337) Corrupt dwarf"},
{"DW_DLE_ZLIB_SECTION_SHORT (338) Corrupt dwarf"},
{"DW_DLE_CIE_INSTR_PTR_ERROR (339)"},
{"DW_DLE_FDE_INSTR_PTR_ERROR (340)"},
{"DW_DLE_FISSION_ADDITION_ERROR (341) Corrupt dwarf"},
{"DW_DLE_HEADER_LEN_BIGGER_THAN_SECSIZE (342) Corrupt dwarf"},
{"DW_DLE_LOCEXPR_OFF_SECTION_END (343) Corrupt dwarf"},
{"DW_DLE_POINTER_SECTION_UNKNOWN (344)"},
{"DW_DLE_ERRONEOUS_XU_INDEX_SECTION(345) XU means cu_ or tu_ index"},
{"DW_DLE_DIRECTORY_FORMAT_COUNT_VS_DIRECTORIES_MISMATCH(346) "
    "Inconsistent line table, corrupted."},
{"DW_DLE_COMPRESSED_EMPTY_SECTION(347) corrupt section data"},
{"DW_DLE_SIZE_WRAPAROUND(348) Impossible string length"},
{"DW_DLE_ILLOGICAL_TSEARCH(349) Impossible situation. "
    "Corrupted data?"},
{"DW_DLE_BAD_STRING_FORM(350) Not a currently allowed form"},
{"DW_DLE_DEBUGSTR_ERROR(351) problem generating .debug_str section"},
{"DW_DLE_DEBUGSTR_UNEXPECTED_REL(352) string relocation "
    "will be wrong."},
{"DW_DLE_DISCR_ARRAY_ERROR(353) Internal error in "
    "dwarf_discr_list()"},
{"DW_DLE_LEB_OUT_ERROR(354) Insufficient buffer to turn "
    "integer to leb"},
{"DW_DLE_SIBLING_LIST_IMPROPER(355) Runs off end of section. "
    "Corrupt dwarf"},
{"DW_DLE_LOCLIST_OFFSET_BAD(356) Corrupt dwarf"},
{"DW_DLE_LINE_TABLE_BAD(357) Corrupt line table"},
{"DW_DLE_DEBUG_LOClISTS_DUPLICATE(358)"},
{"DW_DLE_DEBUG_RNGLISTS_DUPLICATE(359)"},
{"DW_DLE_ABBREV_OFF_END(360)"},
{"DW_DLE_FORM_STRING_BAD_STRING(361) string runs off end of data"},
{"DW_DLE_AUGMENTATION_STRING_OFF_END(362) augmentation runs off "
    "of its section"},
{"DW_DLE_STRING_OFF_END_PUBNAMES_LIKE(363) one of the global "
    "sections, string bad"},
{"DW_DLE_LINE_STRING_BAD(364)  runs off end of line data"},
{"DW_DLE_DEFINE_FILE_STRING_BAD(365) runs off end of section"},
{"DW_DLE_MACRO_STRING_BAD(366) DWARF5 macro def/undef string "
    "runs off section data"},
{"DW_DLE_MACINFO_STRING_BAD(367) DWARF2..4 macro def/undef "
    "string runs off section data"},
{"DW_DLE_ZLIB_UNCOMPRESS_ERROR(368) Surely an invalid "
    "uncompress length"},
{"DW_DLE_IMPROPER_DWO_ID(369)"},
{"DW_DLE_GROUPNUMBER_ERROR(370) An error determining default "
    "target group number"},
{"DW_DLE_ADDRESS_SIZE_ZERO(371)"},
{"DW_DLE_DEBUG_NAMES_HEADER_ERROR(372)"},
{"DW_DLE_DEBUG_NAMES_AUG_STRING_ERROR(373) corrupt dwarf"},
{"DW_DLE_DEBUG_NAMES_PAD_NON_ZERO(374) corrupt dwarf"},
{"DW_DLE_DEBUG_NAMES_OFF_END(375) corrupt dwarf"},
{"DW_DLE_DEBUG_NAMES_ABBREV_OVERFLOW(376) Surprising "
    "overrun of fixed size array"},
{"DW_DLE_DEBUG_NAMES_ABBREV_CORRUPTION(377)"},
{"DW_DLE_DEBUG_NAMES_NULL_POINTER(378) null argument"},
{"DW_DLE_DEBUG_NAMES_BAD_INDEX_ARG(379) index outside valid range"},
{"DW_DLE_DEBUG_NAMES_ENTRYPOOL_OFFSET(380) offset outside entrypool"},
{"DW_DLE_DEBUG_NAMES_UNHANDLED_FORM(381) Might be corrupt "
    "dwarf or incomplete DWARF support"},
{"DW_DLE_LNCT_CODE_UNKNOWN(382)"},
{"DW_DLE_LNCT_FORM_CODE_NOT_HANDLED(383) Might be bad form "
    "or just not implemented"},
{"DW_DLE_LINE_HEADER_LENGTH_BOTCH(384) Internal libdwarf error"},
{"DW_DLE_STRING_HASHTAB_IDENTITY_ERROR(385) Internal libdwarf error"},
{"DW_DLE_UNIT_TYPE_NOT_HANDLED(386) Possibly incomplete "
    "dwarf5 support"},
{"DW_DLE_GROUP_MAP_ALLOC(387) Out of malloc space"},
{"DW_DLE_GROUP_MAP_DUPLICATE(388) Each section # should appear once"},
{"DW_DLE_GROUP_COUNT_ERROR(389) An inconsistency in map entry count"},
{"DW_DLE_GROUP_INTERNAL_ERROR(390) libdwarf data corruption"},
{"DW_DLE_GROUP_LOAD_ERROR(391) corrupt data?"},
{"DW_DLE_GROUP_LOAD_READ_ERROR(392)"},
{"DW_DLE_AUG_DATA_LENGTH_BAD(393) Data does not fit in section"},
{"DW_DLE_ABBREV_MISSING(394) Unable to find abbrev for DIE"},
{"DW_DLE_NO_TAG_FOR_DIE(395)"},
{"DW_DLE_LOWPC_WRONG_CLASS(396) found in dwarf_lowpc()"},
{"DW_DLE_HIGHPC_WRONG_FORM(397) found in dwarf_highpc()"},
{"DW_DLE_STR_OFFSETS_BASE_WRONG_FORM(398)"},
{"DW_DLE_DATA16_OUTSIDE_SECTION(399)"},
{"DW_DLE_LNCT_MD5_WRONG_FORM(400)"},
{"DW_DLE_LINE_HEADER_CORRUPT(401) possible data corruption"},
{"DW_DLE_STR_OFFSETS_NULLARGUMENT(402) improper call"},
{"DW_DLE_STR_OFFSETS_NULL_DBG(403) improper call"},
{"DW_DLE_STR_OFFSETS_NO_MAGIC(404) improper call"},
{"DLE_STR_OFFSETS_ARRAY_SIZE(405) Not a multiple of entry size"},
{"DW_DLE_STR_OFFSETS_VERSION_WRONG(406) Must be 5 "},
{"DW_DLE_STR_OFFSETS_ARRAY_INDEX_WRONG(407) Requested outside bound"},
{"DW_DLE_STR_OFFSETS_EXTRA_BYTES(408) .debug_str_offsets "
    "section problem"},
{"DW_DLE_DUP_ATTR_ON_DIE(409) Compiler error, object improper DWARF"},
{"DW_DLE_SECTION_NAME_BIG(410) Caller provided insufficient "
    "room for section name"},
{"DW_DLE_FILE_UNAVAILABLE(411). Unable find/read object file"},
{"DW_DLE_FILE_WRONG_TYPE(412). Not an object type we recognize."},
{"DW_DLE_SIBLING_OFFSET_WRONG(413). Corrupt dwarf."},
{"DW_DLE_OPEN_FAIL(414) Unable to open, possibly a bad filename"},
{"DW_DLE_OFFSET_SIZE(415) Offset size is neither 32 nor 64"},
{"DW_DLE_MACH_O_SEGOFFSET_BAD(416) corrupt object"},
{"DW_DLE_FILE_OFFSET_BAD(417) corrupt object"},
{"DW_DLE_SEEK_ERROR(418). Seek failed, corrupt object"},
{"DW_DLE_READ_ERROR(419). Read failed, corrupt object"},
{"DW_DLE_ELF_CLASS_BAD(420) Corrupt object."},
{"DW_DLE_ELF_ENDIAN_BAD(421) Corrupt object."},
{"DW_DLE_ELF_VERSION_BAD(422) Corrupt object."},
{"DW_DLE_FILE_TOO_SMALL(423) File is too small to be an "
    "object file."},
{"DW_DLE_PATH_SIZE_TOO_SMALL(424) buffer passed to "
    "dwarf_object_detector_path is too small."},
{"DW_DLE_BAD_TYPE_SIZE(425) At compile time the build "
    "configured itself improperly."},
{"DW_DLE_PE_SIZE_SMALL(426) File too small to be valid PE object."},
{"DW_DLE_PE_OFFSET_BAD(427) Calculated offset too large. "
    "Corrupt object."},
{"DW_DLE_PE_STRING_TOO_LONG(428) Increase size for call."},
{"DW_DLE_IMAGE_FILE_UNKNOWN_TYPE(429) a PE object has an "
    "unknown machine type, not 0x14c, 0x200 or 0x8664"},
{"DLE_LINE_TABLE_LINENO_ERROR(430) Negative line number "
    "impossible. Corrupted line table."},
{"DW_DLE_PRODUCER_CODE_NOT_AVAILABLE(431) Without elf.h "
    "the producer code is not available."},
{"DW_DLE_NO_ELF_SUPPORT(432) libdwarf was compiled without "
    "Elf object support."},
{"DW_DLE_NO_STREAM_RELOC_SUPPORT(433) no elf.h so cannot "
    "generate STREAM relocations"},
{"DW_DLE_RETURN_EMPTY_PUBNAMES_ERROR(434) Flag value passed "
    "in not allowed."},
{"DW_DLE_SECTION_SIZE_ERROR(435) Corrupt Elf. Section size: "
    "> file size or not a multiple of entry size"},
{"DW_DLE_INTERNAL_NULL_POINTER(436) Internal libdwarf "
    "call:null pointer"},
{"DW_DLE_SECTION_STRING_OFFSET_BAD(437) Corrupt Elf, an "
    "offset to section name is invalid"},
{"DW_DLE_SECTION_INDEX_BAD(438) Corrupt Elf, a section "
    "index is incorrect"},
{"DW_DLE_INTEGER_TOO_SMALL(439) Build does not allow reading Elf64"},
{"DW_DLE_ELF_SECTION_LINK_ERROR(440) Corrupt Elf, section "
    "links in error"},
{"DW_DLE_ELF_SECTION_GROUP_ERROR(441) Corrupt Elf, section "
    "group information problem"},
{"DW_DLE_ELF_SECTION_COUNT_MISMATCH(442) Corrupt Elf or "
    "libdwarf bug."},
{"DW_DLE_ELF_STRING_SECTION_MISSING(443) Corrupt Elf, "
    "string section wrong type"},
{"DW_DLE_SEEK_OFF_END(444) Corrupt Elf. Seek past the end "
    "not allowed"},
{"DW_DLE_READ_OFF_END(445) Corrupt Elf. A read would read past "
    "end of object"},
{"DW_DLE_ELF_SECTION_ERROR(446) Section offset or size is too large. "
    "Corrupt elf object."},
{"DW_DLE_ELF_STRING_SECTION_ERROR(447) String section missing. "
    "Corrupt Elf"},
{"DW_DLE_MIXING_SPLIT_DWARF_VERSIONS(448) DWARF5 header "
    "signature and DWARF4 DW_AT_[GNU]_dwo_id both present"},
{"DW_DLE_TAG_CORRUPT(449) DW_TAG outside allowed range. "
    "Corrupt DWARF."},
{"DW_DLE_FORM_CORRUPT(450) DW_FORM unknown, too large a value. "
    "Corrupt DWARF?"},
{"DW_DLE_ATTR_CORRUPT(451) DW_AT outside allowed range. "
    "Corrupt DWARF."},
{"DW_DLE_ABBREV_ATTR_DUPLICATION(452) Abbreviation list corruption."},
{"DW_DLE_DWP_SIGNATURE_MISMATCH(453) Impossible signature "
    "mismatch. Corrupted Dwarf?"},
{"DW_DLE_CU_UT_TYPE_VALUE(454) Internal libdwarf data corruption"},
{"DW_DLE_DUPLICATE_GNU_DEBUGLINK(455) Duplicated section "
    ".gnu_debuglink"},
{"DW_DLE_CORRUPT_GNU_DEBUGLINK(456) Section length wrong"},
{"DW_DLE_CORRUPT_NOTE_GNU_DEBUGID(457) Data corruption in "
    ".note.gnu.debugid section"},
{"DW_DLE_CORRUPT_GNU_DEBUGID_SIZE(458) Section .note.gnu.debugid "
    "size incorrect"},
{"DW_DLE_CORRUPT_GNU_DEBUGID_STRING(459) Section .note.gnu.debugid "
    "owner string not terminated properly"},
{"DW_DLE_HEX_STRING_ERROR(460).  dwarf_producer_init() "
    "extras string has a bad hex string"},
{"DW_DLE_DECIMAL_STRING_ERROR(461) dwarf_producer_init() extras "
    "string has a bad decimal string"},
{"DW_DLE_PRO_INIT_EXTRAS_UNKNOWN(462) dwarf_producer_init() extras "
    "string has an unknown string"},
{"DW_DLE_PRO_INIT_EXTRAS_ERR(463) dwarf_producer_init() extras "
    "string has an unexpected space character"},
{"DW_DLE_NULL_ARGS_DWARF_ADD_PATH(464) obsolete error code"},
{"DW_DLE_DWARF_INIT_DBG_NULL(465) a dwarf_init*() call "
    "the return-dbg argument is null"},
{"DW_DLE_ELF_RELOC_SECTION_ERROR(466) A relocation section header "
    "link field is incorrect."},
{"DW_DLE_USER_DECLARED_ERROR(467) library user created this."},
{"DW_DLE_RNGLISTS_ERROR(468) Corrupt dwarf. Bad .debug_rnglists "
    "data."},
{"DW_DLE_LOCLISTS_ERROR(469) Corrupt dwarf. Bad .debug_loclists "
    "data."},
{"DW_DLE_SECTION_SIZE_OR_OFFSET_LARGE(470) corrupt section header."},
{"DW_DLE_GDBINDEX_STRING_ERROR(471) .gdb_index section string error"},
{"DW_DLE_GNU_PUBNAMES_ERROR(472) A problem with .debug_gnu_pubnames"},
{"DW_DLE_GNU_PUBTYPES_ERROR(473) A problem with .debug_gnu_pubtypes"},
{"DW_DLE_DUPLICATE_GNU_DEBUG_PUBNAMES(474) Duplicated section "
    ".debug_gnu_pubnames"},
{"DW_DLE_DUPLICATE_GNU_DEBUG_PUBTYPES(475) Duplicated section "
    ".debug_gnu_pubtypes"},
{"DW_DLE_DEBUG_SUP_STRING_ERROR(476) String in .debug_sup head "
    "runs off the end of the section."},
{"DW_DLE_DEBUG_SUP_ERROR(477). .debug_sup data corruption"},
{"DW_DLE_LOCATION_ERROR(478). A location processing libdwarf error"},
{"DW_DLE_DEBUGLINK_PATH_SHORT(479) Buffer provided for GNU "
    "debuglink is too small"},
{"DW_DLE_SIGNATURE_MISMATCH(480) DWARF4 extension dwo_id and "
    "dwarf5signature present but they do not match!"},
{"DW_DLE_MACRO_VERSION_ERROR(481) Unknown DWARF5 macro version."
    " Corrupt data."},
{"DW_DLE_NEGATIVE_SIZE(482) A size < 0 "
    "(from DW_FORM_implicit_const) is not appropriate"},
{"DW_DLE_UDATA_VALUE_NEGATIVE(483) Reading a negative value from "
    "dwarf_formudata() is not allowed."},
{"DW_DLE_DEBUG_NAMES_ERROR(484) Error reading .debug_names"},
{"DW_DLE_CFA_INSTRUCTION_ERROR(485) Error accessing "
    "frame instructions"},
{"DW_DLE_MACHO_CORRUPT_HEADER(486) Incorrect header content."
    " Corrupt DWARF"},
{"DW_DLE_MACHO_CORRUPT_COMMAND(487) Incorrect Macho Command "
    "data.  Corrupt DWARF"},
{"DW_DLE_MACHO_CORRUPT_SECTIONDETAILS(488) Incorrect Macho "
    "section data. Corrupt DWARF"},
{"DW_DLE_RELOCATION_SECTION_SIZE_ERROR(489) Corrupt Elf. "
    "Reloc section size impossible."},
{"DW_DLE_SYMBOL_SECTION_SIZE_ERROR(490) Corrupt Elf. "
    "Symbols section size bad"},
{"DW_DLE_PE_SECTION_SIZE_ERROR(491) Corrupt PE object. "
    "Section size too large."},
{"DW_DLE_DEBUG_ADDR_ERROR(492) Problem reading .debug_addr"},
{"DW_DLE_NO_SECT_STRINGS(493) Section strings section "
    "number from header "
    "is incorrect. Unusable object"},
{"DW_DLE_TOO_FEW_SECTIONS(494) Sections incomplete, corrupted. "
    "Unusable object"},
{"DW_DLE_BUILD_ID_DESCRIPTION_SIZE(495) .note.gnu.build-id section"
    " corrupt.  Unusable object"},
{"DW_DLE_BAD_SECTION_FLAGS(496) Some section flags are incorrect."
    " Unusable object"},
{"DW_DLE_IMPROPER_SECTION_ZERO(497) Section zero header contents "
    "incorrect. See Elf ABI.  Unsafe object."},
{"DW_DLE_INVALID_NULL_ARGUMENT(498) Argument must be a valid "
    "pointer and non-null"},
{"DW_DLE_LINE_INDEX_WRONG(499) An index into a line table is "
    "not valid. Corrupt data."},
{"DW_DLE_LINE_COUNT_WRONG(500) A count in a line table is "
    "not valid. Corrupt data."},
{"DW_DLE_ARITHMETIC_OVERFLOW(501) Arithmetic overflow. "
    " Corrupt Dwarf." },
{"DW_DLE_UNIVERSAL_BINARY_ERROR(502) Error reading Mach-O "
    "uninversal binary head. Corrupt Mach-O object." },
{"DW_DLE_UNIV_BIN_OFFSET_SIZE_ERROR(503) Offset/size from "
    "a Mach-O universal binary has an impossible value"},
{"DW_DLE_PE_SECTION_SIZE_HEURISTIC_FAIL(504) Section size fails "
    "a heuristic sanity check"},
{"DW_DLE_LLE_ERROR(505) Generic .debug_loclists read error"},
{"DW_DLE_RLE_ERROR(506) Generic .debug_rnglists read error"},
{"DW_DLE_MACHO_SEGMENT_COUNT_HEURISTIC_FAIL(507) "
    "MachO object seems corrupt"},
{"DW_DLE_DUPLICATE_NOTE_GNU_BUILD_ID(508) Duplicated section "},
{"DW_DLE_SYSCONF_VALUE_UNUSABLE(509) sysconf() return is < 200 "
    "or greater than 100million"}
};
#endif /* DWARF_ERRMSG_LIST_H */
