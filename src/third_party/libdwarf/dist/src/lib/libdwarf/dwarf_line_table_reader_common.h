/*
   Copyright (C) 2000-2006 Silicon Graphics, Inc.  All Rights Reserved.
   Portions Copyright (C) 2007-2023 David Anderson. All Rights Reserved.
   Portions Copyright (C) 2010-2012 SN Systems Ltd. All Rights Reserved.
   Portions Copyright (C) 2015-2015 Google, Inc. All Rights Reserved.

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
   Public License along with this program; if not, write
   the Free Software Foundation, Inc., 51 Franklin Street -
   Fifth Floor, Boston MA 02110-1301, USA.

*/

/*  This is #included twice. Once for
    libdwarf callers and one for dwarfdump which prints
    the internals.

    This way we have just one blob of code that reads
    the table operations.  */

static unsigned char
dwarf_standard_opcode_operand_count[
STANDARD_OPERAND_COUNT_TWO_LEVEL] = {
    /* DWARF2 */
    0,
    1, 1, 1, 1,
    0, 0, 0,
    1,
    /* Following are new for DWARF3. */
    0, 0, 1,
    /* Experimental opcodes. */
    1, 2, 0,
};

/*  We have a normal standard opcode base, but
    an arm compiler emitted a non-standard table!
    This could lead to problems...
    ARM C/C++ Compiler, RVCT4.0 [Build 4
    00] seems to get the table wrong .  */
static unsigned char
dwarf_arm_standard_opcode_operand_count[
STANDARD_OPERAND_COUNT_DWARF3] = {
    /* DWARF2 */
    0,
    1, 1, 1, 1,
    0, 0, 0,
    0,  /* <<< --- this is wrong */
    /* Following are new for DWARF3. */
    0, 0, 1
};

/*  Rather like memcmp but identifies which value pair
    mismatches (the return value is non-zero if mismatch,
    zero if match)..
    This is used only in determining which kind of
    standard-opcode-table we have in the DWARF: std-original,
    or std-later.
    mismatch_entry returns the table index that mismatches.
    tabval returns the table byte value.
    lineval returns the value from the line table header.  */
static int
operandmismatch(unsigned char * table,unsigned table_length,
    unsigned char *linetable,
    unsigned check_count,
    unsigned * mismatch_entry, unsigned * tabval,unsigned *lineval)
{
    unsigned i = 0;

    /*  ASSERT: check_count is <= table_length, which
        is guaranteed by the caller. */
    for (i = 0; i<check_count; ++i) {
        if (i >= table_length) {
            *mismatch_entry = i;
            *lineval = linetable[i];
            *tabval = 0; /* No entry present. */
            /* A kind of mismatch */
            return  TRUE;
        }
        if (table[i] == linetable[i]) {
            continue;
        }
        *mismatch_entry = i;
        *tabval = table[i];
        *lineval = linetable[i];
        return  TRUE;
    }
    /* Matches. */
    return FALSE;
}

/*  Encapsulates DECODE_LEB128_UWORD_CK
    so the caller can free resources
    in case of problems. */
static int
read_uword_de(Dwarf_Small **lp,
    Dwarf_Unsigned *out_p,
    Dwarf_Debug dbg,
    Dwarf_Error *err,
    Dwarf_Small *lpend)
{
    Dwarf_Small *inptr = *lp;
    Dwarf_Unsigned out = 0;
    DECODE_LEB128_UWORD_CK(inptr,
        out,
        dbg,err,lpend);
    *lp = inptr;
    *out_p = out;
    return DW_DLV_OK;
}

/*  A bogus value read from a line table */
static void
IssueExpError(Dwarf_Debug dbg,
    Dwarf_Error *err,
    const char * msg,
    Dwarf_Unsigned val)
{
    dwarfstring m;
    char buf[200];

    dwarfstring_constructor_static(&m, buf,sizeof(buf));
    dwarfstring_append(&m , "ERROR: ");
    dwarfstring_append(&m ,(char *)msg);
    dwarfstring_append_printf_u(&m , " Bad value: 0x%x", val);
    _dwarf_error_string(dbg, err, DW_DLE_LINE_TABLE_BAD,
        dwarfstring_string(&m));
    dwarfstring_destructor(&m);
}

static int
read_sword_de(Dwarf_Small **lp,
    Dwarf_Signed *out_p,
    Dwarf_Debug dbg,
    Dwarf_Error *err,
    Dwarf_Small *lpend)
{
    Dwarf_Small *inptr = *lp;
    Dwarf_Signed out = 0;
    DECODE_LEB128_SWORD_CK(inptr,
        out,
        dbg,err,lpend);
    *lp = inptr;
    *out_p = out;
    return DW_DLV_OK;
}

/*  Common line table header reading code.
    Returns DW_DLV_OK, DW_DLV_ERROR.
    DW_DLV_NO_ENTRY cannot be returned, but callers should
    assume it is possible.

    The line_context area must be initialized properly before
    calling this.

    Has the side effect of allocating arrays which
    must be freed (see the Line_Table_Context which
    holds the pointers to space we allocate here).

    bogus_bytes_ptr and bogus_bytes are output values which
    let a print-program notify the user of some surprising bytes
    after a line table header and before the line table instructions.
    These can be ignored unless one is printing.
    And are ignored if NULL passed as the pointer.

    err_count_out may be NULL, in which case we
    make no attempt to count checking-type errors.
    Checking-type errors do not stop us, we just report them.

    See dw-linetableheader.txt for the ordering of text fields
    across the various dwarf versions. The code
    follows this ordering closely.

    Some of the arguments remaining are in line_context
    so can be deleted from the
    argument list (after a close look for correctness).  */
static int
_dwarf_read_line_table_header(Dwarf_Debug dbg,
    Dwarf_CU_Context cu_context,
    Dwarf_Small * section_start,
    Dwarf_Small * data_start,
    Dwarf_Unsigned section_length,
    Dwarf_Small ** updated_data_start_out,
    Dwarf_Line_Context  line_context,
    Dwarf_Small ** bogus_bytes_ptr,
    Dwarf_Unsigned *bogus_bytes,
    Dwarf_Error * err,
    int *err_count_out)
{
    Dwarf_Small *line_ptr = data_start;
    Dwarf_Small *starting_line_ptr = data_start;
    Dwarf_Unsigned total_length = 0;
    int local_length_size = 0;
    int local_extension_size = 0;
    Dwarf_Unsigned prologue_length = 0;
    Dwarf_Half version = 0;
    /*  Both of the next two should point *one past*
        the end of actual data of interest. */
    Dwarf_Small *section_end = section_start + section_length;
    Dwarf_Small *line_ptr_end = 0;
    Dwarf_Small *lp_begin = 0;
    int res = 0;
    Dwarf_Unsigned htmp = 0;

    if (bogus_bytes_ptr) *bogus_bytes_ptr = 0;
    if (bogus_bytes) *bogus_bytes= 0;

    line_context->lc_line_ptr_start = starting_line_ptr;
    /* READ_AREA_LENGTH updates line_ptr for consumed bytes */
    READ_AREA_LENGTH_CK(dbg, total_length, Dwarf_Unsigned,
        line_ptr, local_length_size, local_extension_size,
        err, section_length,section_end);
    line_ptr_end = line_ptr + total_length;
    line_context->lc_line_ptr_end = line_ptr_end;
    line_context->lc_length_field_length =
        (Dwarf_Half)(local_length_size + local_extension_size);
    line_context->lc_section_offset = starting_line_ptr -
        dbg->de_debug_line.dss_data;
    /*  ASSERT: line_context->lc_length_field_length == line_ptr
        -line_context->lc_line_ptr_start;
        The following test allows the == case too
        as that is normal for the last CUs line table. */
    if (line_ptr_end > section_end) {
        dwarfstring m;

        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,"DW_DLE_DEBUG_LINE_LENGTH_BAD "
            " the total length of this line table is too large at"
            " %" DW_PR_DUu  " bytes",total_length);
        _dwarf_error_string(dbg, err, DW_DLE_DEBUG_LINE_LENGTH_BAD,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    line_context->lc_total_length = total_length;

    /* READ_UNALIGNED_CK(dbg, version, Dwarf_Half,
        line_ptr, DWARF_HALF_SIZE,
        err,line_ptr_end); */
    res = _dwarf_read_unaligned_ck_wrapper(dbg,
        &htmp,line_ptr,DWARF_HALF_SIZE,line_ptr_end,err);
    if (res == DW_DLV_ERROR) {
        return res;
    }
    version = (Dwarf_Half)htmp;

    line_context->lc_version_number = version;
    line_ptr += DWARF_HALF_SIZE;
    if (version != DW_LINE_VERSION2 &&
        version != DW_LINE_VERSION3 &&
        version != DW_LINE_VERSION4 &&
        version != DW_LINE_VERSION5 &&
        version != EXPERIMENTAL_LINE_TABLES_VERSION) {
        _dwarf_error(dbg, err, DW_DLE_VERSION_STAMP_ERROR);
        return DW_DLV_ERROR;
    }
    if (version == DW_LINE_VERSION5) {
        if (line_ptr >= line_ptr_end) {
            _dwarf_error(dbg, err, DW_DLE_DEBUG_LINE_LENGTH_BAD);
            return DW_DLV_ERROR;
        }
        line_context->lc_address_size = *(unsigned char *) line_ptr;
        line_ptr = line_ptr + sizeof(Dwarf_Small);
        if (line_ptr >= line_ptr_end) {
            _dwarf_error(dbg, err, DW_DLE_DEBUG_LINE_LENGTH_BAD);
            return DW_DLV_ERROR;
        }
        line_context->lc_segment_selector_size =
            *(unsigned char *) line_ptr;
        line_ptr = line_ptr + sizeof(Dwarf_Small);
    } else {
        line_context->lc_address_size = cu_context->cc_address_size;
        line_context->lc_segment_selector_size =
            cu_context->cc_segment_selector_size;
    }

    READ_UNALIGNED_CK(dbg, prologue_length, Dwarf_Unsigned,
        line_ptr, local_length_size,
        err,line_ptr_end);
    if (prologue_length > total_length) {
        dwarfstring m;
        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            "DW_DLE_DEBUG_LINE_LENGTH_BAD "
            " the prologue length of "
            " this line table is too large at"
            " %" DW_PR_DUu  " bytes",prologue_length);
        _dwarf_error_string(dbg, err,
            DW_DLE_DEBUG_LINE_LENGTH_BAD,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    line_context->lc_prologue_length = prologue_length;
    line_ptr += local_length_size;
    line_context->lc_line_prologue_start = line_ptr;
    if (line_ptr >= line_ptr_end) {
        _dwarf_error(dbg, err, DW_DLE_DEBUG_LINE_LENGTH_BAD);
        return DW_DLV_ERROR;
    }
    line_context->lc_minimum_instruction_length =
        *(unsigned char *) line_ptr;
    line_ptr = line_ptr + sizeof(Dwarf_Small);

    if (version == DW_LINE_VERSION4 ||
        version == DW_LINE_VERSION5 ||
        version == EXPERIMENTAL_LINE_TABLES_VERSION) {
        if (line_ptr >= line_ptr_end) {
            _dwarf_error(dbg, err, DW_DLE_DEBUG_LINE_LENGTH_BAD);
            return DW_DLV_ERROR;
        }
        line_context->lc_maximum_ops_per_instruction =
            *(unsigned char *) line_ptr;
        line_ptr = line_ptr + sizeof(Dwarf_Small);
    }
    if (line_ptr >= line_ptr_end) {
        _dwarf_error(dbg, err, DW_DLE_DEBUG_LINE_LENGTH_BAD);
        return DW_DLV_ERROR;
    }
    line_context->lc_default_is_stmt = *(unsigned char *) line_ptr;
    line_ptr = line_ptr + sizeof(Dwarf_Small);

    if (line_ptr >= line_ptr_end) {
        _dwarf_error(dbg, err, DW_DLE_DEBUG_LINE_LENGTH_BAD);
        return DW_DLV_ERROR;
    }
    line_context->lc_line_base = *(signed char *) line_ptr;
    line_ptr = line_ptr + sizeof(Dwarf_Sbyte);
    if (line_ptr >= line_ptr_end) {
        _dwarf_error(dbg, err, DW_DLE_LINE_OFFSET_BAD);
        return DW_DLV_ERROR;
    }
    line_context->lc_line_range = *(unsigned char *) line_ptr;
    if (!line_context->lc_line_range) {
        _dwarf_error(dbg, err, DW_DLE_DEBUG_LINE_RANGE_ZERO);
        return DW_DLV_ERROR;
    }
    line_ptr = line_ptr + sizeof(Dwarf_Small);
    if (line_ptr >= line_ptr_end) {
        _dwarf_error(dbg, err, DW_DLE_LINE_OFFSET_BAD);
        return DW_DLV_ERROR;
    }
    line_context->lc_opcode_base = *(unsigned char *) line_ptr;
    line_ptr = line_ptr + sizeof(Dwarf_Small);
    /*  Set up the array of standard opcode lengths. */
    /*  We think this works ok even for cross-endian processing of
        objects.  It might be wrong, we might need to
        specially process the array of ubyte into host order.  */
    line_context->lc_opcode_length_table = line_ptr;

    /*  lc_opcode_base is one greater than the size of the array. */
    line_ptr += line_context->lc_opcode_base - 1;
    line_context->lc_std_op_count = line_context->lc_opcode_base -1;
    if (line_ptr >= line_ptr_end) {
        _dwarf_error(dbg, err, DW_DLE_LINE_OFFSET_BAD);
        return DW_DLV_ERROR;
    }
    {
        /*  Determine (as best we can) whether the
            lc_opcode_length_table holds 9 or 12 standard-conforming
            entries.  gcc4 upped to DWARF3's 12 without updating the
            version number.
            EXPERIMENTAL_LINE_TABLES_VERSION upped to 15.  */
        unsigned check_count = line_context->lc_std_op_count;
        unsigned tab_count =
            sizeof(dwarf_standard_opcode_operand_count);

        int operand_ck_fail = TRUE;
        if (line_context->lc_std_op_count > tab_count) {
            _dwarf_print_header_issue(dbg,
                "Too many standard operands in linetable header: ",
                data_start,
                line_context->lc_std_op_count,
                0,0,0,
                err_count_out);
            check_count = tab_count;
        }
        if ((line_context->lc_opcode_length_table + check_count) >=
            section_end) {
            _dwarf_error_string(dbg, err,
                DW_DLE_LINE_NUM_OPERANDS_BAD,
                "DW_DLE_LINE_NUM_OPERANDS_BAD: "
                "The .debug_line section is too short to be real "
                "as a standard opcode table does not fit");
            return DW_DLV_ERROR;
        }
        {
            unsigned entrynum = 0;
            unsigned tabv     = 0;
            unsigned linetabv     = 0;

            int mismatch = operandmismatch(
                dwarf_standard_opcode_operand_count,
                tab_count,
                line_context->lc_opcode_length_table,
                check_count,&entrynum,&tabv,&linetabv);
            if (mismatch) {
                if (err_count_out) {
                    _dwarf_print_header_issue(dbg,
                        "standard-operands did not match, checked",
                        data_start,
                        check_count,
                        entrynum,tabv,linetabv,err_count_out);
                }
                if (check_count >
                    sizeof(dwarf_arm_standard_opcode_operand_count)) {
                    check_count =
                        sizeof(
                        dwarf_arm_standard_opcode_operand_count);
                }
                mismatch = operandmismatch(
                    dwarf_arm_standard_opcode_operand_count,
                    sizeof(dwarf_arm_standard_opcode_operand_count),
                    line_context->lc_opcode_length_table,
                    check_count,&entrynum,&tabv,&linetabv);
                if (!mismatch && err_count_out) {
                    _dwarf_print_header_issue(dbg,
                        "arm (incorrect) operands in use: ",
                        data_start,
                        check_count,
                        entrynum,tabv,linetabv,err_count_out);
                }
            }
            if (!mismatch) {
                if (version == 2) {
                    if (line_context->lc_std_op_count ==
                        STANDARD_OPERAND_COUNT_DWARF3) {
                        _dwarf_print_header_issue(dbg,
                            "standard DWARF3 operands matched,"
                            " but is DWARF2 linetable: count",
                            data_start,
                            check_count,
                            0,0,0, err_count_out);
                    }
                }
                operand_ck_fail = FALSE;
            }
        }
        if (operand_ck_fail) {
            /* Here we are not sure what the lc_std_op_count is. */
            _dwarf_error_string(dbg, err,
                DW_DLE_LINE_NUM_OPERANDS_BAD,
                "DW_DLE_LINE_NUM_OPERANDS_BAD "
                "we cannot determine the size of the standard opcode"
                " table in the line table header");
            return DW_DLV_ERROR;
        }
    }
    /*  At this point we no longer need to check operand counts. */
    if (line_ptr >= line_ptr_end) {
        _dwarf_error_string(dbg, err, DW_DLE_LINE_OFFSET_BAD,
            "DW_DLE_LINE_OFFSET_BAD "
            "we run off the end of the .debug_line section "
            "reading a line table header");
        return DW_DLV_ERROR;
    }

    if (version < DW_LINE_VERSION5){
        Dwarf_Unsigned directories_count = 0;
        Dwarf_Unsigned directories_malloc = 5;
        line_context->lc_include_directories =
            malloc(sizeof(Dwarf_Small *) * directories_malloc);
        if (line_context->lc_include_directories == NULL) {
            _dwarf_error(dbg, err, DW_DLE_ALLOC_FAIL);
            return DW_DLV_ERROR;
        }
        memset(line_context->lc_include_directories, 0,
            sizeof(Dwarf_Small *) * directories_malloc);

        while ((*(char *) line_ptr) != '\0') {
            if (directories_count >= directories_malloc) {
                Dwarf_Unsigned expand = 2 * directories_malloc;
                Dwarf_Unsigned bytesalloc =
                    sizeof(Dwarf_Small *) * expand;
                Dwarf_Small **newdirs =
                    realloc(line_context->lc_include_directories,
                        bytesalloc);

                if (!newdirs) {
                    _dwarf_error_string(dbg, err, DW_DLE_ALLOC_FAIL,
                        "DW_DLE_ALLOC_FAIL "
                        "reallocating an array of include directories"
                        " in a line table");
                    return DW_DLV_ERROR;
                }
                /* Doubled size, zero out second half. */
                memset(newdirs + directories_malloc, 0,
                    sizeof(Dwarf_Small *) * directories_malloc);
                directories_malloc = expand;
                line_context->lc_include_directories = newdirs;
            }
            line_context->lc_include_directories[directories_count] =
                line_ptr;
            res = _dwarf_check_string_valid(dbg,
                data_start,line_ptr,line_ptr_end,
                DW_DLE_LINE_STRING_BAD,err);
            if (res != DW_DLV_OK) {
                return res;
            }
            line_ptr = line_ptr + strlen((char *) line_ptr) + 1;
            directories_count++;
            if (line_ptr >= line_ptr_end) {
                _dwarf_error(dbg, err,
                    DW_DLE_LINE_NUMBER_HEADER_ERROR);
                return DW_DLV_ERROR;
            }
        }
        line_ptr++;
        line_context->lc_include_directories_count =
            directories_count;
    } else if (version == EXPERIMENTAL_LINE_TABLES_VERSION) {
        /* Empty old style dir entry list. */
        line_ptr++;
    } else if (version == DW_LINE_VERSION5) {
        /* handled below */
    } else {
        /* No old style directory entries. */
    }
    /* Later tests will deal with the == case as required. */
    if (line_ptr > line_ptr_end) {
        _dwarf_error(dbg, err, DW_DLE_LINE_OFFSET_BAD);
        return DW_DLV_ERROR;
    }
    if (version < DW_LINE_VERSION5) {
        if (line_ptr >= line_ptr_end) {
            _dwarf_error(dbg, err, DW_DLE_LINE_NUMBER_HEADER_ERROR);
            return DW_DLV_ERROR;
        }
        while (*(char *) line_ptr != '\0') {
            Dwarf_Unsigned utmp;
            Dwarf_Unsigned dir_index = 0;
            Dwarf_Unsigned lastmod = 0;
            Dwarf_Unsigned file_length = 0;
            int resl = 0;
            Dwarf_File_Entry currfile  = 0;

            currfile = (Dwarf_File_Entry)
                malloc(sizeof(struct Dwarf_File_Entry_s));
            if (currfile == NULL) {
                _dwarf_error(dbg, err, DW_DLE_ALLOC_FAIL);
                return DW_DLV_ERROR;
            }
            memset(currfile,0,sizeof(struct Dwarf_File_Entry_s));
            /*  Insert early so in case of error we can find
                and free the record. */
            _dwarf_add_to_files_list(line_context,currfile);

            currfile->fi_file_name = line_ptr;
            resl = _dwarf_check_string_valid(dbg,
                data_start,line_ptr,line_ptr_end,
                DW_DLE_LINE_STRING_BAD,err);
            if (resl != DW_DLV_OK) {
                return resl;
            }
            line_ptr = line_ptr + strlen((char *) line_ptr) + 1;
            /*  DECODE_LEB128_UWORD_CK(line_ptr, utmp,dbg,
                err,line_ptr_end); */
            res =  read_uword_de(&line_ptr,&utmp,
                dbg,err,line_ptr_end);
            if (res == DW_DLV_ERROR) {
                return DW_DLV_ERROR;
            }
            dir_index = (Dwarf_Unsigned) utmp;
            if (dir_index >
                line_context->lc_include_directories_count) {
                _dwarf_error(dbg, err, DW_DLE_DIR_INDEX_BAD);
                return DW_DLV_ERROR;
            }
            currfile->fi_dir_index = dir_index;
            currfile->fi_dir_index_present = TRUE;

            /*DECODE_LEB128_UWORD_CK(line_ptr,lastmod,
                dbg,err, line_ptr_end); */
            res =  read_uword_de( &line_ptr,&lastmod,
                dbg,err,line_ptr_end);
            if (res == DW_DLV_ERROR) {
                return DW_DLV_ERROR;
            }

            currfile->fi_time_last_mod = lastmod;
            currfile->fi_time_last_mod_present = TRUE;

            DECODE_LEB128_UWORD_CK(line_ptr,file_length,
                dbg,err, line_ptr_end);
            currfile->fi_file_length = file_length;
            currfile->fi_file_length_present = TRUE;
            if (line_ptr >= line_ptr_end) {
                _dwarf_error(dbg, err,
                    DW_DLE_LINE_NUMBER_HEADER_ERROR);
                return DW_DLV_ERROR;
            }
        }
        /* Skip trailing nul byte */
        ++line_ptr;
    } else if (version == EXPERIMENTAL_LINE_TABLES_VERSION) {
        if (line_ptr >= line_ptr_end) {
            _dwarf_error(dbg, err, DW_DLE_LINE_NUMBER_HEADER_ERROR);
            return DW_DLV_ERROR;
        }
        if (*line_ptr != 0) {
            _dwarf_error(dbg, err, DW_DLE_LINE_NUMBER_HEADER_ERROR);
            return DW_DLV_ERROR;
        }
        line_ptr++;
    } else if (version == 5) {
        /* handled below */
    } else {
        /* No old style filenames entries. */
    }
    /* Later tests will deal with the == case as required. */
    if (line_ptr > line_ptr_end) {
        _dwarf_error(dbg, err, DW_DLE_LINE_OFFSET_BAD);
        return DW_DLV_ERROR;
    }

    if (version == EXPERIMENTAL_LINE_TABLES_VERSION) {
        static unsigned char expbytes[5] = {0,0xff,0xff,0x7f, 0x7f };
        Dwarf_Unsigned logicals_table_offset = 0;
        Dwarf_Unsigned actuals_table_offset = 0;
        unsigned i = 0;

        for ( ; i < 5; ++i) {
            if (line_ptr >= line_ptr_end) {
                _dwarf_error(dbg, err,
                    DW_DLE_LINE_NUMBER_HEADER_ERROR);
                return DW_DLV_ERROR;
            }
            if (*line_ptr != expbytes[i]) {
                _dwarf_error(dbg, err,
                    DW_DLE_LINE_NUMBER_HEADER_ERROR);
                return DW_DLV_ERROR;
            }
            line_ptr++;
        }
        READ_UNALIGNED_CK(dbg, logicals_table_offset, Dwarf_Unsigned,
            line_ptr, local_length_size,err,line_ptr_end);
        line_context->lc_logicals_table_offset =
            logicals_table_offset;
        line_ptr += local_length_size;
        READ_UNALIGNED_CK(dbg, actuals_table_offset, Dwarf_Unsigned,
            line_ptr, local_length_size,err,line_ptr_end);
        line_context->lc_actuals_table_offset = actuals_table_offset;
        line_ptr += local_length_size;
        /* Later tests will deal with the == case as required. */
        if (line_ptr > line_ptr_end) {
            _dwarf_error_string(dbg, err, DW_DLE_LINE_OFFSET_BAD,
                "DW_DLE_LINE_OFFSET_BAD "
                "The line table pointer points past end "
                "of line table.");
            return DW_DLV_ERROR;
        }
        if (actuals_table_offset > dbg->de_filesize) {
            _dwarf_error_string(dbg, err, DW_DLE_LINE_OFFSET_BAD,
                "DW_DLE_LINE_OFFSET_BAD "
                "The line table actuals offset is larger than "
                " the size of the object file. Corrupt DWARF");
            return DW_DLV_ERROR;
        }
        if ((line_ptr+actuals_table_offset) > line_ptr_end) {
            _dwarf_error_string(dbg, err, DW_DLE_LINE_OFFSET_BAD,
                "DW_DLE_LINE_OFFSET_BAD "
                "The line table actuals offset is too large "
                "to be real.");
            return DW_DLV_ERROR;
        }
    }

    if (version == DW_LINE_VERSION5 ||
        version == EXPERIMENTAL_LINE_TABLES_VERSION) {
        /* DWARF 5.  directory names.*/
        Dwarf_Unsigned directory_format_count = 0;
        struct Dwarf_Unsigned_Pair_s * format_values = 0;
        Dwarf_Unsigned directories_count = 0;
        Dwarf_Unsigned i = 0;
        Dwarf_Unsigned j = 0;
        int dres = 0;

        if (line_ptr >= line_ptr_end) {
            _dwarf_error(dbg, err, DW_DLE_LINE_NUMBER_HEADER_ERROR);
            return DW_DLV_ERROR;
        }
        directory_format_count = *(unsigned char *) line_ptr;
        line_context->lc_directory_entry_format_count =
            directory_format_count;
        line_ptr = line_ptr + sizeof(Dwarf_Small);
        if (directory_format_count > 0) {
            format_values = malloc(
                sizeof(struct Dwarf_Unsigned_Pair_s) *
                directory_format_count);
            if (format_values == NULL) {
                _dwarf_error(dbg, err, DW_DLE_ALLOC_FAIL);
                return DW_DLV_ERROR;
            }
            for (i = 0; i < directory_format_count; i++) {
                dres=read_uword_de(&line_ptr,
                    &format_values[i].up_first,
                    dbg,err,line_ptr_end);
                if (dres != DW_DLV_OK) {
                    free(format_values);
                    format_values = 0;
                    return dres;
                }
                dres=read_uword_de(&line_ptr,
                    &format_values[i].up_second,
                    dbg,err,line_ptr_end);
                if (dres != DW_DLV_OK) {
                    free(format_values);
                    format_values = 0;
                    return dres;
                }
                /*  FIXME: what would be appropriate tests
                    of this pair of values? */
            }
        }
        dres = read_uword_de(&line_ptr,&directories_count,
            dbg,err,line_ptr_end);
        if (dres != DW_DLV_OK) {
            free(format_values);
            format_values = 0;
            return dres;
        }
        if (directories_count > total_length) {
            dwarfstring m;

            free(format_values);
            format_values = 0;
            dwarfstring_constructor(&m);
            dwarfstring_append_printf_u(&m,
                "DW_DLE_DEBUG_LINE_LENGTH_BAD "
                " the directories count of "
                " this line table is too large at"
                " %" DW_PR_DUu  ,directories_count);
            _dwarf_error_string(dbg, err,
                DW_DLE_DEBUG_LINE_LENGTH_BAD,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
        }
        line_context->lc_include_directories =
            malloc(sizeof(Dwarf_Small *) * directories_count);
        if (line_context->lc_include_directories == NULL) {
            free(format_values);
            format_values = 0;
            _dwarf_error(dbg, err, DW_DLE_ALLOC_FAIL);
            return DW_DLV_ERROR;
        }
        if (directory_format_count == 0 &&
            directories_count > 0) {
            free(format_values);
            format_values = 0;
            _dwarf_error_string(dbg, err,
                DW_DLE_DIRECTORY_FORMAT_COUNT_VS_DIRECTORIES_MISMATCH,
                "DW_DLE_DIRECTORY_FORMAT_COUNT_"
                "VS_DIRECTORIES_MISMATCH"
                ": format count is zero yet directories count > 0");
            return DW_DLV_ERROR;
        }
        memset(line_context->lc_include_directories, 0,
            sizeof(Dwarf_Small *) * directories_count);

        for (i = 0; i < directories_count; i++) {
            for (j = 0; j < directory_format_count; j++) {
                Dwarf_Unsigned lntype =
                    format_values[j].up_first;
                Dwarf_Unsigned lnform =
                    format_values[j].up_second;
                if (line_ptr >= line_ptr_end) {
                    free(format_values);
                    format_values = 0;
                    _dwarf_error_string(dbg, err,
                        DW_DLE_LINE_NUMBER_HEADER_ERROR,
                        " Running off end of line table"
                        " reading directory path");
                    return DW_DLV_ERROR;
                }
                switch (lntype) {
                case DW_LNCT_path: {
                    char *inc_dir_ptr = 0;
                    res = _dwarf_decode_line_string_form(dbg,
                        lntype,lnform,
                        local_length_size,
                        &line_ptr,
                        line_ptr_end,
                        &inc_dir_ptr,
                        err);
                    if (res != DW_DLV_OK) {
                        free(format_values);
                        format_values = 0;
                        return res;
                    }
                    line_context->lc_include_directories[i] =
                        (unsigned char *)inc_dir_ptr;
                    break;
                }
                default:
                    free(format_values);
                    format_values = 0;
                    _dwarf_error(dbg, err,
                        DW_DLE_LINE_NUMBER_HEADER_ERROR);
                    return DW_DLV_ERROR;
                }
            }
            /* Later tests will deal with the == case as required. */
            if (line_ptr > line_ptr_end) {
                free(format_values);
                format_values = 0;
                _dwarf_error(dbg, err,
                    DW_DLE_LINE_NUMBER_HEADER_ERROR);
                return DW_DLV_ERROR;
            }
        }
        line_context->lc_directory_format_values =
            format_values;
        format_values = 0;
        line_context->lc_include_directories_count =
            directories_count;
    }
    if (version == DW_LINE_VERSION5 ||
        version == EXPERIMENTAL_LINE_TABLES_VERSION) {
        /* DWARF 5.  file names.*/
        struct Dwarf_Unsigned_Pair_s *
            filename_entry_pairs = 0;
        Dwarf_Unsigned filename_format_count = 0;
        Dwarf_Unsigned files_count = 0;
        Dwarf_Unsigned i = 0;
        Dwarf_Unsigned j = 0;
        int dres = 0;

        if (line_ptr >= line_ptr_end) {
            _dwarf_error(dbg, err,
                DW_DLE_LINE_NUMBER_HEADER_ERROR);
            return DW_DLV_ERROR;
        }
        filename_format_count = *(unsigned char *) line_ptr;
        line_context->lc_file_name_format_count =
            filename_format_count;
        line_ptr = line_ptr + sizeof(Dwarf_Small);
        filename_entry_pairs = malloc(
            sizeof(struct Dwarf_Unsigned_Pair_s) *
            filename_format_count);
        if (!filename_entry_pairs) {
            _dwarf_error(dbg, err, DW_DLE_ALLOC_FAIL);
            return DW_DLV_ERROR;
        }
        if (line_ptr >= line_ptr_end) {
            free(filename_entry_pairs);
            _dwarf_error_string(dbg, err,
                DW_DLE_LINE_NUMBER_HEADER_ERROR,
                "DW_DLE_LINE_NUMBER_HEADER_ERROR: "
                "reading filename format entries");
            return DW_DLV_ERROR;
        }
        for (i = 0; i < filename_format_count; i++) {
            dres=read_uword_de(&line_ptr,
                &filename_entry_pairs[i].up_first,
                dbg,err,line_ptr_end);
            if (dres != DW_DLV_OK) {
                free(filename_entry_pairs);
                return dres;
            }
            dres=read_uword_de(&line_ptr,
                &filename_entry_pairs[i].
                up_second, dbg,err,line_ptr_end);
            if (dres != DW_DLV_OK) {
                free(filename_entry_pairs);
                return dres;
            }
            /*  FIXME: what would be appropriate tests
                of this pair of values? */
        }
        /* DECODE_LEB128_UWORD_CK(line_ptr, files_count,
            dbg,err,line_ptr_end); */
        dres=read_uword_de(&line_ptr,&files_count,
            dbg,err,line_ptr_end);
        if (dres != DW_DLV_OK) {
            free(filename_entry_pairs);
            return dres;
        }
        if (files_count > total_length) {
            dwarfstring m;

            dwarfstring_constructor(&m);
            dwarfstring_append_printf_u(&m,
                "DW_DLE_DEBUG_LINE_LENGTH_BAD "
                " the files count of "
                " this line table is too large at"
                " %" DW_PR_DUu  " files",files_count);
            _dwarf_error_string(dbg, err,
                DW_DLE_DEBUG_LINE_LENGTH_BAD,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            free(filename_entry_pairs);
            return DW_DLV_ERROR;
        }

        for (i = 0; i < files_count; i++) {
            Dwarf_File_Entry curline = 0;
            curline = (Dwarf_File_Entry)
                malloc(sizeof(struct Dwarf_File_Entry_s));
            if (curline == NULL) {
                free(filename_entry_pairs);
                _dwarf_error_string(dbg, err, DW_DLE_ALLOC_FAIL,
                    "DW_DLE_ALLOC_FAIL: "
                    "Unable to malloc Dwarf_File_Entry_s");
                return DW_DLV_ERROR;
            }
            memset(curline,0,sizeof(*curline));
            _dwarf_add_to_files_list(line_context,curline);
            for (j = 0; j < filename_format_count; j++) {
                Dwarf_Unsigned dirindex = 0;
                Dwarf_Unsigned lntype =
                    filename_entry_pairs[j].up_first;
                Dwarf_Unsigned lnform =
                    filename_entry_pairs[j].up_second;

                if (line_ptr >= line_ptr_end) {
                    free(filename_entry_pairs);
                    _dwarf_error_string(dbg, err,
                        DW_DLE_LINE_NUMBER_HEADER_ERROR,
                        "DW_DLE_LINE_NUMBER_HEADER_ERROR: "
                        "file name format count too large "
                        "to be correct. Corrupt DWARF/");
                    return DW_DLV_ERROR;
                }
                switch (lntype) {
                /* The LLVM LNCT is documented in
                    https://releases.llvm.org/9.0.0/docs
                    /AMDGPUUsage.html
                */

                /* Cannot find the GNU items documented.? */
                case DW_LNCT_GNU_decl_file: /* FORM udata*/
                    res = _dwarf_decode_line_udata_form(dbg,
                        lntype,lnform,
                        &line_ptr,
                        &dirindex,
                        line_ptr_end,
                        err);
                    if (res != DW_DLV_OK) {
                        free(filename_entry_pairs);
                        return res;
                    }
                    curline->fi_gnu_decl_file = dirindex;
                    curline->fi_gnu_decl_file_present = TRUE;
                    break;
                case DW_LNCT_GNU_decl_line: /* FORM udata */
                    res = _dwarf_decode_line_udata_form(dbg,
                        lntype,lnform,
                        &line_ptr,
                        &dirindex,
                        line_ptr_end,
                        err);
                    if (res != DW_DLV_OK) {
                        free(filename_entry_pairs);
                        return res;
                    }
                    curline->fi_gnu_decl_line = dirindex;
                    curline->fi_gnu_decl_line_present = TRUE;
                    break;

                case DW_LNCT_path: {
                    res = _dwarf_decode_line_string_form(dbg,
                        lntype, lnform,
                        local_length_size,
                        &line_ptr,
                        line_ptr_end,
                        (char **)&curline->fi_file_name,
                        err);
                    if (res != DW_DLV_OK) {
                        free(filename_entry_pairs);
                        return res;
                    }
                    }
                    break;
                case DW_LNCT_GNU_subprogram_name: {
                    res = _dwarf_decode_line_string_form(dbg,
                        lntype, lnform,
                        local_length_size,
                        &line_ptr,
                        line_ptr_end,
                        (char **)&curline->fi_gnu_subprogram_name,
                        err);
                    if (res != DW_DLV_OK) {
                        free(filename_entry_pairs);
                        return res;
                    }
                    }
                    break;
                case DW_LNCT_LLVM_source: {
                    res = _dwarf_decode_line_string_form(dbg,
                        lntype, lnform,
                        local_length_size,
                        &line_ptr,
                        line_ptr_end,
                        (char **)&curline->fi_llvm_source,
                        err);
                    if (res != DW_DLV_OK) {
                        free(filename_entry_pairs);
                        return res;
                    }
                    }
                    break;
                case DW_LNCT_directory_index:
                    res = _dwarf_decode_line_udata_form(dbg,
                        lntype,lnform,
                        &line_ptr,
                        &dirindex,
                        line_ptr_end,
                        err);
                    if (res != DW_DLV_OK) {
                        free(filename_entry_pairs);
                        return res;
                    }
                    curline->fi_dir_index = dirindex;
                    curline->fi_dir_index_present = TRUE;
                    break;
                case DW_LNCT_timestamp:
                    res = _dwarf_decode_line_udata_form(dbg,
                        lntype, lnform,
                        &line_ptr,
                        &curline->fi_time_last_mod,
                        line_ptr_end,
                        err);
                    if (res != DW_DLV_OK) {
                        free(filename_entry_pairs);
                        return res;
                    }
                    curline->fi_time_last_mod_present = TRUE;
                    break;
                case DW_LNCT_size:
                    res = _dwarf_decode_line_udata_form(dbg,
                        lntype, lnform,
                        &line_ptr,
                        &curline->fi_file_length,
                        line_ptr_end,
                        err);
                    if (res != DW_DLV_OK) {
                        free(filename_entry_pairs);
                        return res;
                    }
                    curline->fi_file_length_present = TRUE;
                    break;
                case DW_LNCT_MD5: { /* form DW_FORM_data16 */
                    if (filename_entry_pairs[j].up_second !=
                        DW_FORM_data16) {
                        free(filename_entry_pairs);
                        _dwarf_error(dbg, err,
                            DW_DLE_LNCT_MD5_WRONG_FORM);
                        return  DW_DLV_ERROR;
                    }
                    res = _dwarf_extract_data16(dbg,
                        line_ptr,
                        line_ptr,
                        line_ptr_end,
                        &curline->fi_md5_value,
                        err);
                    if (res != DW_DLV_OK) {
                        free(filename_entry_pairs);
                        return res;
                    }
                    curline->fi_md5_present = TRUE;
                    line_ptr = line_ptr +
                        sizeof(curline->fi_md5_value);
                    }
                    break;

                default:
                    _dwarf_report_bad_lnct(dbg,
                        lntype,
                        DW_DLE_LINE_NUMBER_HEADER_ERROR,
                        "DW_DLE_LINE_NUMBER_HEADER_ERROR",
                        err);
                    free(filename_entry_pairs);
                    return DW_DLV_ERROR;
                }
                if (line_ptr > line_ptr_end) {
                    free(filename_entry_pairs);
                    _dwarf_error_string(dbg, err,
                        DW_DLE_LINE_NUMBER_HEADER_ERROR,
                        "DW_DLE_LINE_NUMBER_HEADER_ERROR: "
                        "Reading line table header filenames "
                        "runs off end of section. Corrupt Dwarf");
                    return DW_DLV_ERROR;
                }
            }
        }
        line_context->lc_file_format_values = filename_entry_pairs;
        filename_entry_pairs = 0;
    }
    /*  For two-level line tables, read the
        subprograms table. */
    if (version == EXPERIMENTAL_LINE_TABLES_VERSION) {
        Dwarf_Unsigned subprog_format_count = 0;
        Dwarf_Unsigned *subprog_entry_types = 0;
        Dwarf_Unsigned *subprog_entry_forms = 0;
        Dwarf_Unsigned subprogs_count = 0;
        Dwarf_Unsigned i = 0;
        Dwarf_Unsigned j = 0;
        int dres = 0;

        /*  line_ptr_end is *after* the valid area */
        if (line_ptr >= line_ptr_end) {
            _dwarf_error(dbg, err, DW_DLE_LINE_NUMBER_HEADER_ERROR);
            return DW_DLV_ERROR;
        }
        subprog_format_count = *(unsigned char *) line_ptr;
        line_ptr = line_ptr + sizeof(Dwarf_Small);
        subprog_entry_types = malloc(sizeof(Dwarf_Unsigned) *
            subprog_format_count);
        if (subprog_entry_types == NULL) {
            _dwarf_error_string(dbg, err, DW_DLE_ALLOC_FAIL,
                "DW_DLE_ALLOC_FAIL allocating subprog_entry_types");
            return DW_DLV_ERROR;
        }
        if (subprog_format_count > total_length) {
            IssueExpError(dbg,err,
                "Subprog format count Count too "
                "large to be real",
                subprog_format_count);
            free(subprog_entry_types);
            return DW_DLV_ERROR;
        }
        if (line_ptr >= line_ptr_end) {
            free(subprog_entry_types);
            _dwarf_error_string(dbg, err,
                DW_DLE_LINE_NUMBER_HEADER_ERROR,
                "DW_DLE_LINE_NUMBER_HEADER_ERROR: "
                "Line table forms odd, experimental libdwarf");
            return DW_DLV_ERROR;
        }

        subprog_entry_forms = malloc(sizeof(Dwarf_Unsigned) *
            subprog_format_count);
        if (subprog_entry_forms == NULL) {
            free(subprog_entry_types);
            _dwarf_error(dbg, err, DW_DLE_ALLOC_FAIL);
            _dwarf_error_string(dbg, err, DW_DLE_ALLOC_FAIL,
                "DW_DLE_ALLOC_FAIL allocating subprog_entry_forms");
            return DW_DLV_ERROR;
        }

        for (i = 0; i < subprog_format_count; i++) {

            dres=read_uword_de(&line_ptr,subprog_entry_types+i,
                dbg,err,line_ptr_end);
            if (dres != DW_DLV_OK) {
                free(subprog_entry_types);
                free(subprog_entry_forms);
                return dres;
            }
            if (subprog_entry_types[i] > total_length) {
                IssueExpError(dbg,err,
                    "Subprog entry_types[i] count Count too "
                    "large to be real",
                    subprog_entry_types[i]);
                free(subprog_entry_types);
                free(subprog_entry_forms);
                return DW_DLV_ERROR;
            }
            dres=read_uword_de(&line_ptr,subprog_entry_forms+i,
                dbg,err,line_ptr_end);
            if (dres != DW_DLV_OK) {
                free(subprog_entry_types);
                free(subprog_entry_forms);
                return dres;
            }
            if (subprog_entry_forms[i] > total_length) {
                IssueExpError(dbg,err,
                    "Subprog entry_forms[i] count Count too "
                    "large to be real",
                    subprog_entry_forms[i]);
                free(subprog_entry_types);
                free(subprog_entry_forms);
                return DW_DLV_ERROR;
            }
        }
        dres=read_uword_de(&line_ptr,&subprogs_count,
            dbg,err,line_ptr_end);
        if (dres != DW_DLV_OK) {
            free(subprog_entry_types);
            free(subprog_entry_forms);
            return dres;
        }
        if (subprogs_count > total_length) {
            IssueExpError(dbg,err,
                "Subprogs Count too large to be real",
                subprogs_count);
            free(subprog_entry_types);
            free(subprog_entry_forms);
            return DW_DLV_ERROR;
        }
        line_context->lc_subprogs =
            malloc(sizeof(struct Dwarf_Subprog_Entry_s) *
                subprogs_count);
        if (line_context->lc_subprogs == NULL) {
            free(subprog_entry_types);
            free(subprog_entry_forms);
            _dwarf_error(dbg, err, DW_DLE_ALLOC_FAIL);
            return DW_DLV_ERROR;
        }
        memset(line_context->lc_subprogs, 0,
            sizeof(struct Dwarf_Subprog_Entry_s) * subprogs_count);
        for (i = 0; i < subprogs_count; i++) {
            struct Dwarf_Subprog_Entry_s *curline =
                line_context->lc_subprogs + i;
            if (line_ptr >= line_ptr_end) {
                free(subprog_entry_types);
                free(subprog_entry_forms);
                _dwarf_error_string(dbg, err,
                    DW_DLE_LINE_NUMBER_HEADER_ERROR,
                    "DW_DLE_LINE_NUMBER_HEADER_ERROR:"
                    " Reading suprogram entry subprogs"
                    " in experimental line table"
                    " we run off the end of the table");
                return DW_DLV_ERROR;
            }
            for (j = 0; j < subprog_format_count; j++) {
                Dwarf_Unsigned lntype =
                    subprog_entry_types[j];
                Dwarf_Unsigned lnform =
                    subprog_entry_forms[j];
                switch (lntype) {
                case DW_LNCT_GNU_subprogram_name:
                    res = _dwarf_decode_line_string_form(dbg,
                        lntype,lnform,
                        local_length_size,
                        &line_ptr,
                        line_ptr_end,
                        (char **)&curline->ds_subprog_name,
                        err);
                    if (res != DW_DLV_OK) {
                        free(subprog_entry_types);
                        free(subprog_entry_forms);
                        return res;
                    }
                    break;
                case DW_LNCT_GNU_decl_file:
                    res = _dwarf_decode_line_udata_form(dbg,
                        lntype,lnform,
                        &line_ptr,
                        &curline->ds_decl_file,
                        line_ptr_end,
                        err);
                    if (res != DW_DLV_OK) {
                        free(subprog_entry_forms);
                        free(subprog_entry_types);
                        return res;
                    }
                    break;
                case DW_LNCT_GNU_decl_line:
                    res = _dwarf_decode_line_udata_form(dbg,
                        lntype,lnform,
                        &line_ptr,
                        &curline->ds_decl_line,
                        line_ptr_end,
                        err);
                    if (res != DW_DLV_OK) {
                        free(subprog_entry_forms);
                        free(subprog_entry_types);
                        return res;
                    }
                    break;
                default:
                    free(subprog_entry_forms);
                    free(subprog_entry_types);
                    _dwarf_report_bad_lnct(dbg,
                        lntype,
                        DW_DLE_LINE_NUMBER_HEADER_ERROR,
                        "DW_DLE_LINE_NUMBER_HEADER_ERROR",
                        err);
                    return DW_DLV_ERROR;
                }
                if (line_ptr >= line_ptr_end) {
                    free(subprog_entry_types);
                    free(subprog_entry_forms);
                    _dwarf_error_string(dbg, err,
                        DW_DLE_LINE_NUMBER_HEADER_ERROR,
                        "DW_DLE_LINE_NUMBER_HEADER_ERROR:"
                        " Reading suprogram entry DW_LNCT* types "
                        " we run off the end of the table");
                    return DW_DLV_ERROR;
                }
            }
        }

        free(subprog_entry_types);
        free(subprog_entry_forms);
        line_context->lc_subprogs_count = subprogs_count;
    }
    if (version == EXPERIMENTAL_LINE_TABLES_VERSION) {
        lp_begin = line_context->lc_line_prologue_start +
            line_context->lc_logicals_table_offset;
    } else {
        lp_begin = line_context->lc_line_prologue_start +
            line_context->lc_prologue_length;
    }
    if (line_ptr > line_ptr_end) {
        _dwarf_error(dbg, err, DW_DLE_LINE_OFFSET_BAD);
        return DW_DLV_ERROR;
    }
    if (line_ptr != lp_begin) {
        if (line_ptr > lp_begin) {
            _dwarf_error(dbg, err, DW_DLE_LINE_PROLOG_LENGTH_BAD);
            return DW_DLV_ERROR;
        } else {
            /*  Bug in compiler. These
                bytes are really part of the instruction
                stream.  The line_context->lc_prologue_length is
                wrong (12 too high).  */
            if (bogus_bytes_ptr) {
                *bogus_bytes_ptr = line_ptr;
            }
            if (bogus_bytes) {
                /*  How far off things are. We expect the
                    value 12 ! Negative value seems impossible. */
                /*  ptrdiff_t is generated but not named */
                *bogus_bytes = (lp_begin - line_ptr);
            }
        }
        /*  Ignore the lp_begin calc. Assume line_ptr right.
            Making up for compiler bug. */
        lp_begin = line_ptr;
    }
    line_context->lc_line_ptr_start = lp_begin;
    if (line_context->lc_actuals_table_offset) {
        /* This means two tables. */
        line_context->lc_table_count = 2;
    } else {
        if (line_context->lc_line_ptr_end > lp_begin) {
            line_context->lc_table_count = 1;
        } else {
            line_context->lc_table_count = 0;
        }
    }
    *updated_data_start_out = lp_begin;
    return DW_DLV_OK;
}

/*  Read one line table program. For two-level line tables, this
    function is called once for each table. */
static int
read_line_table_program(Dwarf_Debug dbg,
    Dwarf_Small *line_ptr,
    Dwarf_Small *line_ptr_end,
    Dwarf_Small *orig_line_ptr,
    Dwarf_Small *section_start,
    Dwarf_Line_Context line_context,
    Dwarf_Half address_size,
    Dwarf_Bool doaddrs, /* Only TRUE if SGI IRIX rqs calling. */
    Dwarf_Bool dolines,
    Dwarf_Bool is_single_table,
    Dwarf_Bool is_actuals_table,
    Dwarf_Error *error,
    int *err_count_out)
{
    Dwarf_Unsigned i = 0;
    Dwarf_File_Entry cur_file_entry = 0;
    Dwarf_Line *logicals = line_context->lc_linebuf_logicals;
    Dwarf_Unsigned logicals_count =
        line_context->lc_linecount_logicals;

    struct Dwarf_Line_Registers_s regs;

    /*  This is a pointer to the current line being added to the line
        matrix. */
    Dwarf_Line curr_line = 0;

    /*  These variables are used to decode leb128 numbers. Leb128_num
        holds the decoded number, and leb128_length is its length in
        bytes. */
    Dwarf_Unsigned leb128_num = 0;
    Dwarf_Signed advance_line = 0;

    /*  This is the operand of the latest fixed_advance_pc extended
        opcode. */
    Dwarf_Unsigned fixed_advance_pc = 0;

    /*  Counts the number of lines in the line matrix. */
    Dwarf_Unsigned line_count = 0;

    /*  This is the length of an extended opcode instr.  */
    Dwarf_Unsigned instr_length = 0;

    /*  Used to chain together pointers to line table entries that are
        later used to create a block of Dwarf_Line entries. */
    Dwarf_Chain chain_line = NULL;
    Dwarf_Chain head_chain = NULL;
    Dwarf_Chain curr_chain = NULL;

    /*  This points to a block of Dwarf_Lines, a pointer to which is
        returned in linebuf. */
    Dwarf_Line *block_line = 0;

    /*  Mark a line record as being DW_LNS_set_address */
    Dwarf_Bool is_addr_set = FALSE;

    (void)orig_line_ptr;
    (void)err_count_out;
    /*  Initialize the one state machine variable that depends on the
        prefix.  */
    _dwarf_set_line_table_regs_default_values(&regs,
        line_context->lc_version_number,
        line_context->lc_default_is_stmt);

    /* Start of statement program.  */
    while (line_ptr < line_ptr_end) {
        int type = 0;
        Dwarf_Small opcode = 0;

#ifdef PRINTING_DETAILS
        {
        dwarfstring m9a;
        dwarfstring_constructor(&m9a);
        dwarfstring_append_printf_u(&m9a,
            " [0x%06" DW_PR_DSx "] ",
            /*  ptrdiff_t generated but not named */
            (line_ptr - section_start));
        _dwarf_printf(dbg,dwarfstring_string(&m9a));
        dwarfstring_destructor(&m9a);
        }
#endif /* PRINTING_DETAILS */
        opcode = *(Dwarf_Small *) line_ptr;
        line_ptr++;
        /* 'type' is the output */
        WHAT_IS_OPCODE(type, opcode, line_context->lc_opcode_base,
            line_context->lc_opcode_length_table, line_ptr,
            line_context->lc_std_op_count);

        if (type == LOP_DISCARD) {
            int oc = 0;
            int opcnt = line_context->lc_opcode_length_table[opcode];
#ifdef PRINTING_DETAILS
            {
            dwarfstring m9b;
            dwarfstring_constructor(&m9b);
            dwarfstring_append_printf_i(&m9b,
                "*** DWARF CHECK: DISCARD standard opcode %d ",
                opcode);
            dwarfstring_append_printf_i(&m9b,
                "with %d operands: not understood.", opcnt);
            _dwarf_printf(dbg,dwarfstring_string(&m9b));
            *err_count_out += 1;
            dwarfstring_destructor(&m9b);
            }
#endif /* PRINTING_DETAILS */
            for (oc = 0; oc < opcnt; oc++) {
                int ocres = 0;
                /*  Read and discard operands we don't
                    understand.
                    arbitrary choice of unsigned read.
                    signed read would work as well.    */
                Dwarf_Unsigned utmp2 = 0;

                (void) utmp2;
                ocres =  read_uword_de( &line_ptr,&utmp2,
                    dbg,error,line_ptr_end);
                if (ocres == DW_DLV_ERROR) {
                    _dwarf_free_chain_entries(dbg,head_chain,
                        line_count);
                    if (curr_line) {
                        dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                        curr_line = 0;
                    }
                    return DW_DLV_ERROR;
                }

#ifdef PRINTING_DETAILS
                {
                dwarfstring m9e;
                dwarfstring_constructor(&m9e);
                dwarfstring_append_printf_u(&m9e,
                    " %" DW_PR_DUu,
                    utmp2);
                dwarfstring_append_printf_u(&m9e,
                    " (0x%" DW_PR_XZEROS DW_PR_DUx ")",
                    utmp2);
                _dwarf_printf(dbg,dwarfstring_string(&m9e));
                dwarfstring_destructor(&m9e);
                }
#endif /* PRINTING_DETAILS */
            }
#ifdef PRINTING_DETAILS
            _dwarf_printf(dbg,"***\n");
#endif /* PRINTING_DETAILS */
        } else if (type == LOP_SPECIAL) {
            /*  This op code is a special op in the object, no matter
                that it might fall into the standard op range in this
                compile. That is, these are special opcodes between
                opcode_base and MAX_LINE_OP_CODE.  (including
                opcode_base and MAX_LINE_OP_CODE) */
#ifdef PRINTING_DETAILS
            unsigned origop = opcode;
#endif /* PRINTING_DETAILS */
            Dwarf_Unsigned operation_advance = 0;

            opcode = opcode - line_context->lc_opcode_base;
            operation_advance =
                (opcode / line_context->lc_line_range);

            if (line_context->lc_maximum_ops_per_instruction < 2) {
                regs.lr_address = regs.lr_address +
                    (operation_advance *
                    line_context->lc_minimum_instruction_length);
            } else {
                regs.lr_address = regs.lr_address +
                    (line_context->lc_minimum_instruction_length *
                    ((regs.lr_op_index + operation_advance)/
                    line_context->lc_maximum_ops_per_instruction));
                regs.lr_op_index =
                    (regs.lr_op_index +operation_advance)%
                    line_context->lc_maximum_ops_per_instruction;
            }

            regs.lr_line = regs.lr_line + line_context->lc_line_base +
                opcode % line_context->lc_line_range;
            if ((Dwarf_Signed)regs.lr_line < 0) {
                /* Something is badly wrong */
                dwarfstring m;

                dwarfstring_constructor(&m);
                dwarfstring_append_printf_i(&m,
                    "\nERROR: DW_DLE_LINE_TABLE_LINENO_ERROR "
                    "The line number computes as %d "
                    "and negative line numbers "
                    "are not correct.",(Dwarf_Signed)regs.lr_line);
                _dwarf_error_string(dbg, error,
                    DW_DLE_LINE_TABLE_LINENO_ERROR,
                    dwarfstring_string(&m));
                dwarfstring_destructor(&m);
                regs.lr_line = 0;
                _dwarf_free_chain_entries(dbg,head_chain,
                    line_count);
                if (curr_line) {
                    dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                    curr_line = 0;
                }
                return DW_DLV_ERROR;
            }
#ifdef PRINTING_DETAILS
            {
            dwarfstring ma;
            dwarfstring mb;

            dwarfstring_constructor(&ma);
            dwarfstring_constructor(&mb);
            dwarfstring_append_printf_u(&mb,"Specialop %3u", origop);
            _dwarf_printf(dbg,dwarfstring_string(&ma));
            dwarfstring_destructor(&ma);
            print_line_detail(dbg,dwarfstring_string(&mb),
                (int)opcode,(unsigned)(line_count+1),
                &regs,is_single_table,
                is_actuals_table);
            dwarfstring_destructor(&mb);
            dwarfstring_destructor(&ma);
            }
#endif /* PRINTING_DETAILS */

            if (dolines) {
                curr_line =
                    (Dwarf_Line) _dwarf_get_alloc(dbg,DW_DLA_LINE,1);
                if (curr_line == NULL) {
                    _dwarf_free_chain_entries(dbg,head_chain,
                        line_count);
                    _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
                    return DW_DLV_ERROR;
                }

                /* Mark a line record as being DW_LNS_set_address */
                curr_line->li_l_data.li_is_addr_set =
                    is_addr_set;
                is_addr_set = FALSE;
                curr_line->li_address = regs.lr_address;
                curr_line->li_l_data.li_file =
                    (Dwarf_Signed) regs.lr_file;
                curr_line->li_l_data.li_line =
                    (Dwarf_Signed) regs.lr_line;
                curr_line->li_l_data.li_column =
                    (Dwarf_Half) regs.lr_column;
                curr_line->li_l_data.li_is_stmt =
                    regs.lr_is_stmt;
                curr_line->li_l_data.li_basic_block =
                    regs.lr_basic_block;
                curr_line->li_l_data.li_end_sequence =
                    curr_line->li_l_data.
                    li_epilogue_begin = regs.lr_epilogue_begin;
                curr_line->li_l_data.li_prologue_end =
                    regs.lr_prologue_end;
                curr_line->li_l_data.li_isa =
                    regs.lr_isa;
                curr_line->li_l_data.li_discriminator =
                    regs.lr_discriminator;
                curr_line->li_l_data.li_call_context =
                    regs.lr_call_context;
                curr_line->li_l_data.li_subprogram =
                    regs.lr_subprogram;
                curr_line->li_context = line_context;
                curr_line->li_is_actuals_table = is_actuals_table;
                line_count++;

                chain_line = (Dwarf_Chain)
                    _dwarf_get_alloc(dbg, DW_DLA_CHAIN, 1);
                if (chain_line == NULL) {
                    if (curr_line) {
                        dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                    }
                    _dwarf_free_chain_entries(dbg,head_chain,
                        line_count);
                    _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
                    return DW_DLV_ERROR;
                }
                chain_line->ch_itemtype = DW_DLA_LINE;
                chain_line->ch_item = curr_line;
                _dwarf_update_chain_list(chain_line,&head_chain,
                    &curr_chain);
                curr_line = 0;
            }

            regs.lr_basic_block = FALSE;
            regs.lr_prologue_end = FALSE;
            regs.lr_epilogue_begin = FALSE;
            regs.lr_discriminator = 0;
#ifdef PRINTING_DETAILS
#endif /* PRINTING_DETAILS */
        } else if (type == LOP_STANDARD) {
#ifdef PRINTING_DETAILS
            dwarfstring mb;
#endif /* PRINTING_DETAILS */

            switch (opcode) {
            case DW_LNS_copy:{

#ifdef PRINTING_DETAILS
                print_line_detail(dbg,"DW_LNS_copy",
                    opcode,(unsigned int)line_count+1,
                    &regs,is_single_table,
                    is_actuals_table);
#endif /* PRINTING_DETAILS */
                if (dolines) {
                    curr_line = (Dwarf_Line) _dwarf_get_alloc(dbg,
                        DW_DLA_LINE, 1);
                    if (curr_line == NULL) {
                        _dwarf_free_chain_entries(dbg,head_chain,
                            line_count);
                        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
                        return DW_DLV_ERROR;
                    }

                    /* Mark a line record as DW_LNS_set_address */
                    curr_line->li_l_data.li_is_addr_set =
                        is_addr_set;
                    is_addr_set = FALSE;

                    curr_line->li_address = regs.lr_address;
                    curr_line->li_l_data.li_file =
                        (Dwarf_Signed) regs.lr_file;
                    curr_line->li_l_data.li_line =
                        (Dwarf_Signed) regs.lr_line;
                    curr_line->li_l_data.li_column =
                        (Dwarf_Half) regs.lr_column;
                    curr_line->li_l_data.li_is_stmt =
                        regs.lr_is_stmt;
                    curr_line->li_l_data.
                        li_basic_block = regs.lr_basic_block;
                    curr_line->li_l_data.
                        li_end_sequence = regs.lr_end_sequence;
                    curr_line->li_context = line_context;
                    curr_line->li_is_actuals_table = is_actuals_table;
                    curr_line->li_l_data.
                        li_epilogue_begin = regs.lr_epilogue_begin;
                    curr_line->li_l_data.
                        li_prologue_end = regs.lr_prologue_end;
                    curr_line->li_l_data.li_isa =
                        regs.lr_isa;
                    curr_line->li_l_data.li_discriminator
                        = regs.lr_discriminator;
                    curr_line->li_l_data.li_call_context
                        = regs.lr_call_context;
                    curr_line->li_l_data.li_subprogram =
                        regs.lr_subprogram;
                    line_count++;

                    chain_line = (Dwarf_Chain)
                        _dwarf_get_alloc(dbg, DW_DLA_CHAIN, 1);
                    if (chain_line == NULL) {
                        dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                        _dwarf_free_chain_entries(dbg,head_chain,
                            line_count);
                        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
                        return DW_DLV_ERROR;
                    }
                    chain_line->ch_itemtype = DW_DLA_LINE;
                    chain_line->ch_item = curr_line;
                    _dwarf_update_chain_list(chain_line,&head_chain,
                        &curr_chain);
                    curr_line = 0;
                }

                regs.lr_basic_block = FALSE;
                regs.lr_prologue_end = FALSE;
                regs.lr_epilogue_begin = FALSE;
                regs.lr_discriminator = 0;
                }
                break;
            case DW_LNS_advance_pc:{
                Dwarf_Unsigned utmp2 = 0;
                int advres = 0;

                advres =  read_uword_de( &line_ptr,&utmp2,
                    dbg,error,line_ptr_end);
                if (advres == DW_DLV_ERROR) {
                    _dwarf_free_chain_entries(dbg,head_chain,
                        line_count);
                    if (curr_line) {
                        dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                    }
                    return DW_DLV_ERROR;
                }

#ifdef PRINTING_DETAILS
                dwarfstring_constructor(&mb);
                dwarfstring_append_printf_i(&mb,
                    "DW_LNS_advance_pc val %" DW_PR_DSd,
                    utmp2);
                dwarfstring_append_printf_u(&mb,
                    " 0x%" DW_PR_XZEROS DW_PR_DUx "\n",
                    utmp2);
                _dwarf_printf(dbg,dwarfstring_string(&mb));
                dwarfstring_destructor(&mb);
#endif /* PRINTING_DETAILS */
                leb128_num = utmp2;
                regs.lr_address = regs.lr_address +
                    line_context->lc_minimum_instruction_length *
                    leb128_num;
                }
                break;
            case DW_LNS_advance_line:{
                Dwarf_Signed stmp = 0;
                int alres = 0;

                alres =  read_sword_de( &line_ptr,&stmp,
                    dbg,error,line_ptr_end);
                if (alres == DW_DLV_ERROR) {
                    _dwarf_free_chain_entries(dbg,head_chain,
                        line_count);
                    if (curr_line) {
                        dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                    }
                    return DW_DLV_ERROR;
                }
                advance_line = (Dwarf_Signed) stmp;

#ifdef PRINTING_DETAILS
                dwarfstring_constructor(&mb);
                dwarfstring_append_printf_i(&mb,
                    "DW_LNS_advance_line val %" DW_PR_DSd,
                    advance_line);
                dwarfstring_append_printf_u(&mb,
                    " 0x%" DW_PR_XZEROS DW_PR_DSx "\n",
                    advance_line);
                _dwarf_printf(dbg,dwarfstring_string(&mb));
                dwarfstring_destructor(&mb);
#endif /* PRINTING_DETAILS */
                regs.lr_line = regs.lr_line + advance_line;
                if ((Dwarf_Signed)regs.lr_line < 0) {
                    dwarfstring m;

                    dwarfstring_constructor(&m);
                    dwarfstring_append_printf_i(&m,
                        "\nERROR: DW_DLE_LINE_TABLE_LINENO_ERROR"
                        " The line number is %d "
                        "and negative line numbers after "
                        "DW_LNS_ADVANCE_LINE ",
                        (Dwarf_Signed)regs.lr_line);
                    dwarfstring_append_printf_i(&m,
                        " of %d "
                        "are not correct.",stmp);
                    _dwarf_error_string(dbg, error,
                        DW_DLE_LINE_TABLE_LINENO_ERROR,
                        dwarfstring_string(&m));
                    dwarfstring_destructor(&m);
                    regs.lr_line = 0;
                    if (curr_line) {
                        dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                    }
                    _dwarf_free_chain_entries(dbg,head_chain,
                        line_count);
                    return DW_DLV_ERROR;
                }
                }
                break;
            case DW_LNS_set_file:{
                Dwarf_Unsigned utmp2 = 0;
                int sfres = 0;

                sfres =  read_uword_de( &line_ptr,&utmp2,
                    dbg,error,line_ptr_end);
                if (sfres == DW_DLV_ERROR) {
                    _dwarf_free_chain_entries(dbg,head_chain,
                        line_count);
                    if (curr_line) {
                        dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                    }
                    return DW_DLV_ERROR;
                }
                {
                    Dwarf_Signed fno = (Dwarf_Signed)utmp2;
                    if (fno < 0) {
                        _dwarf_free_chain_entries(dbg,head_chain,
                            line_count);
                        if (curr_line) {
                            dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                        }
                        _dwarf_error_string(dbg,error,
                            DW_DLE_LINE_INDEX_WRONG,
                            "DW_DLE_LINE_INDEX_WRONG "
                            "A DW_LNS_set_file has an "
                            "Impossible "
                            "file number ");
                        return DW_DLV_ERROR;
                    }
                }

                regs.lr_file = utmp2;
#ifdef PRINTING_DETAILS
                dwarfstring_constructor(&mb);
                dwarfstring_append_printf_i(&mb,
                    "DW_LNS_set_file  %ld\n",
                    regs.lr_file);
                _dwarf_printf(dbg,dwarfstring_string(&mb));
                dwarfstring_destructor(&mb);
#endif /* PRINTING_DETAILS */
                }
                break;
            case DW_LNS_set_column:{
                Dwarf_Unsigned utmp2 = 0;
                int scres = 0;

                scres =  read_uword_de( &line_ptr,&utmp2,
                    dbg,error,line_ptr_end);
                if (scres == DW_DLV_ERROR) {
                    _dwarf_free_chain_entries(dbg,head_chain,
                        line_count);
                    if (curr_line) {
                        dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                    }
                    return DW_DLV_ERROR;
                }
                {
                    Dwarf_Signed cno = (Dwarf_Signed)utmp2;
                    if (cno < 0) {
                        _dwarf_free_chain_entries(dbg,head_chain,
                            line_count);
                        if (curr_line) {
                            dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                        }
                        _dwarf_error_string(dbg,error,
                            DW_DLE_LINE_INDEX_WRONG,
                            "DW_DLE_LINE_INDEX_WRONG "
                            "A DW_LNS_set_column has an "
                            "impossible "
                            "column number ");
                        return DW_DLV_ERROR;
                    }
                }

                regs.lr_column = utmp2;
#ifdef PRINTING_DETAILS
                dwarfstring_constructor(&mb);

                dwarfstring_append_printf_i(&mb,
                    "DW_LNS_set_column val %" DW_PR_DSd ,
                    regs.lr_column);
                dwarfstring_append_printf_u(&mb,
                    " 0x%" DW_PR_XZEROS DW_PR_DSx "\n",
                    regs.lr_column);
                _dwarf_printf(dbg,dwarfstring_string(&mb));
                dwarfstring_destructor(&mb);
#endif /* PRINTING_DETAILS */
                }
                break;
            case DW_LNS_negate_stmt:{
                regs.lr_is_stmt = !regs.lr_is_stmt;
#ifdef PRINTING_DETAILS
                _dwarf_printf(dbg, "DW_LNS_negate_stmt\n");
#endif /* PRINTING_DETAILS */
                }
                break;
            case DW_LNS_set_basic_block:{
                regs.lr_basic_block = TRUE;
#ifdef PRINTING_DETAILS
                _dwarf_printf(dbg,
                    "DW_LNS_set_basic_block\n");
#endif /* PRINTING_DETAILS */
                }
                break;

            case DW_LNS_const_add_pc:{
                opcode = MAX_LINE_OP_CODE -
                    line_context->lc_opcode_base;
                if (line_context->lc_maximum_ops_per_instruction < 2){
                    Dwarf_Unsigned operation_advance =
                        (opcode / line_context->lc_line_range);
                    regs.lr_address = regs.lr_address +
                        line_context->lc_minimum_instruction_length *
                            operation_advance;
                } else {
                    Dwarf_Unsigned operation_advance =
                        (opcode / line_context->lc_line_range);
                    regs.lr_address = regs.lr_address +
                        line_context->lc_minimum_instruction_length *
                        ((regs.lr_op_index + operation_advance)/
                        line_context->lc_maximum_ops_per_instruction);
                    regs.lr_op_index =
                        (regs.lr_op_index +operation_advance)%
                        line_context->lc_maximum_ops_per_instruction;
                }
#ifdef PRINTING_DETAILS
                dwarfstring_constructor(&mb);
                dwarfstring_append_printf_u(&mb,
                    "DW_LNS_const_add_pc new address 0x%"
                    DW_PR_XZEROS DW_PR_DSx "\n",
                    regs.lr_address);
                _dwarf_printf(dbg,dwarfstring_string(&mb));
                dwarfstring_destructor(&mb);
#endif /* PRINTING_DETAILS */
                }
                break;
            case DW_LNS_fixed_advance_pc:{
                Dwarf_Unsigned fpc = 0;
                int apres = 0;

                apres = _dwarf_read_unaligned_ck_wrapper(dbg,
                    &fpc,line_ptr,DWARF_HALF_SIZE,line_ptr_end,
                    error);
                fixed_advance_pc = fpc;
                if (apres == DW_DLV_ERROR) {
                    if (curr_line) {
                        dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                    }
                    _dwarf_free_chain_entries(dbg,head_chain,
                        line_count);
                    return apres;
                }
                line_ptr += DWARF_HALF_SIZE;
                if (line_ptr > line_ptr_end) {
                    dwarfstring g;
                    /*  ptrdiff_t is generated but not named */
                    Dwarf_Unsigned localoff =
                        (line_ptr >= section_start)?
                        (line_ptr - section_start):0xfffffff;

                    dwarfstring_constructor(&g);
                    dwarfstring_append_printf_u(&g,
                        "DW_DLE_LINE_TABLE_BAD reading "
                        "DW_LNS_fixed_advance_pc we are "
                        "off this line table at section "
                        "offset. 0x%x .",
                        localoff);
                    _dwarf_error_string(dbg, error,
                        DW_DLE_LINE_TABLE_BAD,
                        dwarfstring_string(&g));
                    dwarfstring_destructor(&g);
                    if (curr_line) {
                        dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                    }
                    _dwarf_free_chain_entries(dbg,head_chain,
                        line_count);
                    return DW_DLV_ERROR;
                }
                {   Dwarf_Unsigned oldad = regs.lr_address;
                    regs.lr_address = oldad + fixed_advance_pc;
                    if (regs.lr_address < oldad) {
                        _dwarf_error_string(dbg, error,
                            DW_DLE_LINE_TABLE_BAD,
                            "DW_DLE_LINE_TABLE_BAD: "
                            "DW_LNS_fixed_advance_pc "
                            "overflows when added to current "
                            "line table pc.");
                        return DW_DLV_ERROR;
                    }
                }
                regs.lr_op_index = 0;
#ifdef PRINTING_DETAILS
                dwarfstring_constructor(&mb);
                dwarfstring_append_printf_i(&mb,
                    "DW_LNS_fixed_advance_pc val %"
                    DW_PR_DSd, fixed_advance_pc);
                dwarfstring_append_printf_u(&mb,
                    " 0x%" DW_PR_XZEROS DW_PR_DSx,
                    fixed_advance_pc);
                dwarfstring_append_printf_u(&mb,
                    " new address 0x%"
                    DW_PR_XZEROS DW_PR_DSx "\n",
                    regs.lr_address);
                _dwarf_printf(dbg,
                    dwarfstring_string(&mb));
                dwarfstring_destructor(&mb);
#endif /* PRINTING_DETAILS */
                }
                break;

                /* New in DWARF3 */
            case DW_LNS_set_prologue_end:{
                regs.lr_prologue_end = TRUE;
                }
                break;
                /* New in DWARF3 */
            case DW_LNS_set_epilogue_begin:{
                regs.lr_epilogue_begin = TRUE;
#ifdef PRINTING_DETAILS
                _dwarf_printf(dbg,
                    "DW_LNS_set_prologue_end set true.\n");
#endif /* PRINTING_DETAILS */
                }
                break;

                /* New in DWARF3 */
            case DW_LNS_set_isa:{
                Dwarf_Unsigned utmp2 = 0;
                int sires = 0;

                sires =  read_uword_de( &line_ptr,&utmp2,
                    dbg,error,line_ptr_end);
                if (sires == DW_DLV_ERROR) {
                    _dwarf_free_chain_entries(dbg,head_chain,
                        line_count);
                    if (curr_line) {
                        dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                    }
                    return DW_DLV_ERROR;
                }

                regs.lr_isa = (Dwarf_Half)utmp2;

#ifdef PRINTING_DETAILS
                dwarfstring_constructor(&mb);
                dwarfstring_append_printf_u(&mb,
                    "DW_LNS_set_isa new value 0x%"
                    DW_PR_XZEROS DW_PR_DUx ".\n",
                    utmp2);
                _dwarf_printf(dbg,dwarfstring_string(&mb));
                dwarfstring_destructor(&mb);
#endif /* PRINTING_DETAILS */
                if (regs.lr_isa != utmp2) {
                    /*  The value of the isa did
                        not fit in our
                        local so we record it wrong.
                        declare an error. */
                    if (curr_line) {
                        dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                    }
                    _dwarf_free_chain_entries(dbg,
                        head_chain,line_count);
                    _dwarf_error(dbg, error,
                        DW_DLE_LINE_NUM_OPERANDS_BAD);
                    return DW_DLV_ERROR;
                }
                }
                break;

                /*  Experimental two-level line tables */
                /*  DW_LNS_set_address_from_logical and
                    DW_LNS_set_subprogram
                    share the same opcode. Disambiguate by checking
                    is_actuals_table. */
            case DW_LNS_set_subprogram:

                if (is_actuals_table) {
                    /* DW_LNS_set_address_from_logical */
                    Dwarf_Signed stmp = 0;
                    int atres = 0;

                    atres =  read_sword_de( &line_ptr,&stmp,
                        dbg,error,line_ptr_end);
                    if (atres == DW_DLV_ERROR) {
                        _dwarf_free_chain_entries(dbg,head_chain,
                            line_count);
                        if (curr_line) {
                            dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                        }
                        return DW_DLV_ERROR;
                    }
                    advance_line = (Dwarf_Signed) stmp;
                    regs.lr_line = regs.lr_line + advance_line;
                    if ((Dwarf_Signed)regs.lr_line < 0) {
                        dwarfstring m;

                        dwarfstring_constructor(&m);
                        dwarfstring_append_printf_i(&m,
                            "\nERROR: DW_DLE_LINE_TABLE_LINENO_ERROR"
                            " The line number is %d "
                            "and negative line numbers after "
                            "DW_LNS_set_subprogram ",
                            (Dwarf_Signed)regs.lr_line);
                        dwarfstring_append_printf_i(&m,
                            " of %d applied "
                            "are not correct.",stmp);
                        _dwarf_error_string(dbg, error,
                            DW_DLE_LINE_TABLE_LINENO_ERROR,
                            dwarfstring_string(&m));
                        dwarfstring_destructor(&m);
                        regs.lr_line = 0;
                        _dwarf_free_chain_entries(dbg,
                            head_chain,line_count);
                        if (curr_line) {
                            dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                        }
                        return DW_DLV_ERROR;

                    }
                    if (regs.lr_line >= 1 &&
                        regs.lr_line - 1 < logicals_count) {
                        regs.lr_address =
                            logicals[regs.lr_line - 1]->li_address;
                        regs.lr_op_index = 0;
#ifdef PRINTING_DETAILS /* block 1 print */
                        dwarfstring_constructor(&mb);
                        dwarfstring_append_printf_i(&mb,
                            "DW_LNS_set_address_from"
                            "_logical "
                            "%" DW_PR_DSd,
                            stmp);
                        dwarfstring_append_printf_u(&mb,
                            " 0x%" DW_PR_XZEROS DW_PR_DSx,
                            stmp);
                        dwarfstring_append_printf_u(&mb,
                            "  newaddr="
                            " 0x%" DW_PR_XZEROS DW_PR_DUx ".\n",
                            regs.lr_address);
                        _dwarf_printf(dbg,
                            dwarfstring_string(&mb));
                        dwarfstring_destructor(&mb);
#endif /* PRINTING_DETAILS */
                    } else {
#ifdef PRINTING_DETAILS /* block 2 print */
                        dwarfstring_constructor(&mb);
                        dwarfstring_append_printf_i(&mb,
                            "DW_LNS_set_address_from_logical line"
                            " is %" DW_PR_DSd ,
                            regs.lr_line);
                        dwarfstring_append_printf_u(&mb,
                            " 0x%" DW_PR_XZEROS DW_PR_DSx ".\n",
                            regs.lr_line);
                        _dwarf_printf(dbg,
                            dwarfstring_string(&mb));
                        dwarfstring_destructor(&mb);
#endif /* PRINTING_DETAILS */
                    }
                } else {
                    /*  DW_LNS_set_subprogram,
                        building logicals table.  */
                    Dwarf_Unsigned utmp2 = 0;
                    int spres = 0;

                    regs.lr_call_context = 0;
                    spres =  read_uword_de( &line_ptr,&utmp2,
                        dbg,error,line_ptr_end);
                    if (spres == DW_DLV_ERROR) {
                        _dwarf_free_chain_entries(dbg,head_chain,
                            line_count);
                        if (curr_line) {
                            dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                        }
                        return DW_DLV_ERROR;
                    }
                    regs.lr_subprogram = utmp2;
#ifdef PRINTING_DETAILS /* block 3 print */
                    dwarfstring_constructor(&mb);
                    dwarfstring_append_printf_i(&mb,
                        "DW_LNS_set_subprogram "
                        "%" DW_PR_DSd,
                        utmp2);
                    dwarfstring_append_printf_u(&mb,
                        " 0x%" DW_PR_XZEROS DW_PR_DSx "\n",
                        utmp2);
                    _dwarf_printf(dbg,
                        dwarfstring_string(&mb));
                    dwarfstring_destructor(&mb);
#endif /* PRINTING_DETAILS */
                }
                break;
                /* Experimental two-level line tables */
            case DW_LNS_inlined_call: {
                Dwarf_Signed stmp = 0;
                Dwarf_Unsigned ilcuw = 0;
                int icres  = 0;

                icres =  read_sword_de( &line_ptr,&stmp,
                    dbg,error,line_ptr_end);
                if (icres == DW_DLV_ERROR) {
                    _dwarf_free_chain_entries(dbg,head_chain,
                        line_count);
                    if (curr_line) {
                        dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                    }
                    return DW_DLV_ERROR;
                }
                regs.lr_call_context = line_count + stmp;
                icres =  read_uword_de(&line_ptr,&ilcuw,
                    dbg,error,line_ptr_end);
                regs.lr_subprogram = ilcuw;
                if (icres == DW_DLV_ERROR) {
                    _dwarf_free_chain_entries(dbg,head_chain,
                        line_count);
                    if (curr_line) {
                        dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                    }
                    return DW_DLV_ERROR;
                }

#ifdef PRINTING_DETAILS
                dwarfstring_constructor(&mb);
                dwarfstring_append_printf_i(&mb,
                    "DW_LNS_inlined_call "
                    "%" DW_PR_DSd ,stmp);
                dwarfstring_append_printf_u(&mb,
                    " (0x%" DW_PR_XZEROS DW_PR_DSx "),",
                    stmp);
                dwarfstring_append_printf_i(&mb,
                    "%" DW_PR_DSd,
                    regs.lr_subprogram);
                dwarfstring_append_printf_u(&mb,
                    " (0x%" DW_PR_XZEROS DW_PR_DSx ")",
                    regs.lr_subprogram);
                dwarfstring_append_printf_i(&mb,
                    "  callcontext=" "%" DW_PR_DSd ,
                    regs.lr_call_context);
                dwarfstring_append_printf_u(&mb,
                    " (0x%" DW_PR_XZEROS DW_PR_DSx ")\n",
                    regs.lr_call_context);
                _dwarf_printf(dbg,
                    dwarfstring_string(&mb));
                dwarfstring_destructor(&mb);
#endif /* PRINTING_DETAILS */
                }
                break;

                /* Experimental two-level line tables */
            case DW_LNS_pop_context: {
                Dwarf_Unsigned logical_num = regs.lr_call_context;
                Dwarf_Chain logical_chain = head_chain;
                Dwarf_Line logical_line = 0;

                if (logical_num > 0 && logical_num <= line_count) {
                    for (i = 1; i < logical_num; i++) {
                        logical_chain = logical_chain->ch_next;
                    }
                    logical_line =
                        (Dwarf_Line) logical_chain->ch_item;
                    regs.lr_file =
                        logical_line->li_l_data.li_file;
                    regs.lr_line =
                        logical_line->li_l_data.li_line;
                    regs.lr_column =
                        logical_line->
                            li_l_data.li_column;
                    regs.lr_discriminator =
                        logical_line->
                            li_l_data.li_discriminator;
                    regs.lr_is_stmt =
                        logical_line->
                            li_l_data.li_is_stmt;
                    regs.lr_call_context =
                        logical_line->
                            li_l_data.li_call_context;
                    regs.lr_subprogram =
                        logical_line->
                            li_l_data.li_subprogram;
#ifdef PRINTING_DETAILS
                    {
                    dwarfstring pcon;
                    dwarfstring_constructor(&pcon);
                    dwarfstring_append_printf_u(&pcon,
                        "DW_LNS_pop_context set"
                        " from logical "
                        "%" DW_PR_DUu ,logical_num);
                    dwarfstring_append_printf_u(&pcon,
                        " (0x%" DW_PR_XZEROS DW_PR_DUx ")\n",
                        logical_num);
                    _dwarf_printf(dbg,
                        dwarfstring_string(&pcon));
                    dwarfstring_destructor(&pcon);
                    }
                } else {
                    dwarfstring pcon;
                    dwarfstring_constructor(&pcon);
                    dwarfstring_append_printf_u(&pcon,
                        "DW_LNS_pop_context does nothing, logical"
                        "%" DW_PR_DUu ,
                        logical_num);
                    dwarfstring_append_printf_u(&pcon,
                        " (0x%" DW_PR_XZEROS DW_PR_DUx ")\n",
                        logical_num);
                    _dwarf_printf(dbg,
                        dwarfstring_string(&pcon));
                    dwarfstring_destructor(&pcon);
#endif /* PRINTING_DETAILS */
                }
                }
                break;
            default:
                _dwarf_error_string(dbg, error,
                    DW_DLE_LINE_TABLE_BAD,
                    "DW_DLE_LINE_TABLE_BAD: "
                    "Impossible standard line table operator");
                return DW_DLV_ERROR;
            } /* End switch (opcode) */
        } else if (type == LOP_EXTENDED) {
            Dwarf_Unsigned utmp3 = 0;
            Dwarf_Small ext_opcode = 0;
            int leres = 0;

            leres =  read_uword_de( &line_ptr,&utmp3,
                dbg,error,line_ptr_end);
            if (leres == DW_DLV_ERROR) {
                _dwarf_free_chain_entries(dbg,head_chain,
                    line_count);
                if (curr_line) {
                    dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                }
                return DW_DLV_ERROR;
            }

            instr_length =  utmp3;
            /*  Dwarf_Small is a ubyte and the extended opcode is a
                ubyte, though not stated as clearly in the
                2.0.0 spec as one might hope. */
            if (line_ptr >= line_ptr_end) {
                dwarfstring g;
                /*  ptrdiff_t is generated but not named */
                Dwarf_Unsigned localoffset =
                    (line_ptr >= section_start)?
                    (line_ptr - section_start) : 0;

                dwarfstring_constructor(&g);
                dwarfstring_append_printf_u(&g,
                    "DW_DLE_LINE_TABLE_BAD reading "
                    "extended op we are "
                    "off this line table at section "
                    "offset 0x%x .",
                    localoffset);
                _dwarf_error_string(dbg, error,
                    DW_DLE_LINE_TABLE_BAD,
                    dwarfstring_string(&g));
                dwarfstring_destructor(&g);
                if (curr_line) {
                    dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                }
                _dwarf_free_chain_entries(dbg,head_chain,line_count);
                return DW_DLV_ERROR;
            }
            ext_opcode = *(Dwarf_Small *) line_ptr;
            line_ptr++;
            if (line_ptr > line_ptr_end) {
                dwarfstring g;
                /*  ptrdiff_t is generated but not named */
                Dwarf_Unsigned localoff =
                    (line_ptr >= section_start)?
                    (line_ptr - section_start):0xfffffff;

                dwarfstring_constructor(&g);
                dwarfstring_append_printf_u(&g,
                    "DW_DLE_LINE_TABLE_BAD reading "
                    "extended op opcode we are "
                    "off this line table at section "
                    "offset 0x%x .",
                    localoff);
                _dwarf_error_string(dbg, error,
                    DW_DLE_LINE_TABLE_BAD,
                    dwarfstring_string(&g));
                dwarfstring_destructor(&g);
                _dwarf_free_chain_entries(dbg,head_chain,line_count);
                if (curr_line) {
                    dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                }
                return DW_DLV_ERROR;
            }
            switch (ext_opcode) {

            case DW_LNE_end_sequence:{
                regs.lr_end_sequence = TRUE;
                if (dolines) {
                    curr_line = (Dwarf_Line)
                        _dwarf_get_alloc(dbg, DW_DLA_LINE, 1);
                    if (!curr_line) {
                        _dwarf_free_chain_entries(dbg,head_chain,
                            line_count);
                        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
                        return DW_DLV_ERROR;
                    }

#ifdef PRINTING_DETAILS
                    print_line_detail(dbg,
                        "DW_LNE_end_sequence extended",
                        (int)ext_opcode,
                        (unsigned int)line_count+1,&regs,
                        is_single_table, is_actuals_table);
#endif /* PRINTING_DETAILS */
                    curr_line->li_address = regs.lr_address;
                    curr_line->li_l_data.li_file =
                        (Dwarf_Signed) regs.lr_file;
                    curr_line->li_l_data.li_line =
                        (Dwarf_Signed) regs.lr_line;
                    curr_line->li_l_data.li_column =
                        (Dwarf_Half) regs.lr_column;
                    curr_line->li_l_data.li_is_stmt =
                        regs.lr_is_stmt;
                    curr_line->li_l_data.
                        li_basic_block = regs.lr_basic_block;
                    curr_line->li_l_data.
                        li_end_sequence = regs.lr_end_sequence;
                    curr_line->li_context = line_context;
                    curr_line->li_is_actuals_table = is_actuals_table;
                    curr_line->li_l_data.
                        li_epilogue_begin = regs.lr_epilogue_begin;
                    curr_line->li_l_data.
                        li_prologue_end = regs.lr_prologue_end;
                    curr_line->li_l_data.li_isa =
                        regs.lr_isa;
                    curr_line->li_l_data.li_discriminator
                        = regs.lr_discriminator;
                    curr_line->li_l_data.li_call_context
                        = regs.lr_call_context;
                    curr_line->li_l_data.li_subprogram =
                        regs.lr_subprogram;
                    line_count++;
                    chain_line = (Dwarf_Chain)
                        _dwarf_get_alloc(dbg, DW_DLA_CHAIN, 1);
                    if (chain_line == NULL) {
                        dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                        _dwarf_free_chain_entries(dbg,head_chain,
                            line_count);
                        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
                        return DW_DLV_ERROR;
                    }
                    chain_line->ch_itemtype = DW_DLA_LINE;
                    chain_line->ch_item = curr_line;
                    _dwarf_update_chain_list(chain_line,
                        &head_chain,&curr_chain);
                    curr_line = 0;
                }
                _dwarf_set_line_table_regs_default_values(&regs,
                    line_context->lc_version_number,
                    line_context->lc_default_is_stmt);
                }
                break;

            case DW_LNE_set_address:{
                int sares = 0;
                /*  READ_UNALIGNED_CK(dbg, regs.lr_address,
                    Dwarf_Addr,
                    line_ptr, address_size,error,line_ptr_end); */
                sares = _dwarf_read_unaligned_ck_wrapper(dbg,
                    &regs.lr_address,line_ptr,
                    address_size,line_ptr_end,
                    error);
                if (sares == DW_DLV_ERROR) {
                    if (curr_line) {
                        dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                    }
                    _dwarf_free_chain_entries(dbg,head_chain,
                        line_count);
                    return sares;
                }

                /* Mark a line record as being DW_LNS_set_address */
                is_addr_set = TRUE;
#ifdef PRINTING_DETAILS
                {
                dwarfstring sadd;
                dwarfstring_constructor(&sadd);
                dwarfstring_append_printf_u(&sadd,
                    "DW_LNE_set_address address 0x%"
                    DW_PR_XZEROS DW_PR_DUx "\n",
                    regs.lr_address);
                _dwarf_printf(dbg,dwarfstring_string(&sadd));
                dwarfstring_destructor(&sadd);
                }
#endif /* PRINTING_DETAILS */
                if (doaddrs) {
                    /* SGI IRIX rqs processing only. */
                    curr_line = (Dwarf_Line) _dwarf_get_alloc(dbg,
                        DW_DLA_LINE, 1);
                    if (!curr_line) {
                        _dwarf_free_chain_entries(dbg,head_chain,
                            line_count);
                        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
                        return DW_DLV_ERROR;
                    }
                    /*  Mark a line record as being
                        DW_LNS_set_address */
                    curr_line->li_l_data.li_is_addr_set
                        = is_addr_set;
                    is_addr_set = FALSE;
                    curr_line->li_address = regs.lr_address;
#ifdef __sgi /* SGI IRIX ONLY */
                    /*  ptrdiff_t is generated but not named */
                    curr_line->li_offset =
                        line_ptr - dbg->de_debug_line.dss_data;
#endif /* __sgi */
                    line_count++;
                    chain_line = (Dwarf_Chain)
                        _dwarf_get_alloc(dbg, DW_DLA_CHAIN, 1);
                    if (chain_line == NULL) {
                        dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                        _dwarf_free_chain_entries(dbg,head_chain,
                            line_count);
                        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
                        return DW_DLV_ERROR;
                    }
                    chain_line->ch_itemtype = DW_DLA_LINE;
                    chain_line->ch_item = curr_line;
                    _dwarf_update_chain_list(chain_line,
                        &head_chain,&curr_chain);
                    curr_line = 0;
                }
                regs.lr_op_index = 0;
                line_ptr += address_size;
                if (line_ptr > line_ptr_end) {
                    dwarfstring g;
                    /*  ptrdiff_t is generated but not named */
                    Dwarf_Unsigned localoff =
                        (line_ptr >= section_start)?
                        (line_ptr - section_start):0xfffffff;

                    dwarfstring_constructor(&g);
                    dwarfstring_append_printf_u(&g,
                        "DW_DLE_LINE_TABLE_BAD reading "
                        "DW_LNE_set_address we are "
                        "off this line table at section "
                        "offset 0x%x .",
                        localoff);
                    _dwarf_error_string(dbg, error,
                        DW_DLE_LINE_TABLE_BAD,
                        dwarfstring_string(&g));
                    dwarfstring_destructor(&g);
                    if (curr_line) {
                        dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                    }
                    _dwarf_free_chain_entries(dbg,head_chain,
                        line_count);
                    return DW_DLV_ERROR;
                }
                }
                break;

            case DW_LNE_define_file:
                if (dolines) {
                    int dlres = 0;
                    Dwarf_Unsigned value = 0;

                    cur_file_entry = (Dwarf_File_Entry)
                        malloc(sizeof(struct Dwarf_File_Entry_s));
                    if (cur_file_entry == NULL) {
                        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
                        if (curr_line) {
                            dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                        }
                        _dwarf_free_chain_entries(dbg,head_chain,
                            line_count);
                        return DW_DLV_ERROR;
                    }
                    memset(cur_file_entry,0,
                        sizeof(struct Dwarf_File_Entry_s));
                    _dwarf_add_to_files_list(line_context,
                        cur_file_entry);
                    cur_file_entry->fi_file_name =
                        (Dwarf_Small *) line_ptr;
                    dlres = _dwarf_check_string_valid(dbg,
                        line_ptr,line_ptr,line_ptr_end,
                        DW_DLE_DEFINE_FILE_STRING_BAD,error);
                    if (dlres != DW_DLV_OK) {
                        _dwarf_free_chain_entries(dbg,head_chain,
                            line_count);
                        if (curr_line) {
                            dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                        }
                        return dlres;
                    }
                    line_ptr = line_ptr + strlen((char *) line_ptr)
                        + 1;
                    dlres =  read_uword_de( &line_ptr,&value,
                        dbg,error,line_ptr_end);
                    if (dlres == DW_DLV_ERROR) {
                        _dwarf_free_chain_entries(dbg,head_chain,
                            line_count);
                        if (curr_line) {
                            dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                        }
                        return DW_DLV_ERROR;
                    }
                    cur_file_entry->fi_dir_index =
                        (Dwarf_Signed)value;
                    cur_file_entry->fi_dir_index_present = TRUE;
                    dlres =  read_uword_de( &line_ptr,&value,
                        dbg,error,line_ptr_end);
                    if (dlres == DW_DLV_ERROR) {
                        _dwarf_free_chain_entries(dbg,head_chain,
                            line_count);
                        if (curr_line) {
                            dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                        }
                        return DW_DLV_ERROR;
                    }
                    cur_file_entry->fi_time_last_mod = value;
                    dlres =  read_uword_de( &line_ptr,&value,
                        dbg,error,line_ptr_end);
                    if (dlres == DW_DLV_ERROR) {
                        _dwarf_free_chain_entries(dbg,head_chain,
                            line_count);
                        if (curr_line) {
                            dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                        }
                        return DW_DLV_ERROR;
                    }
                    cur_file_entry->fi_file_length = value;
                    cur_file_entry->fi_dir_index_present = TRUE;
                    cur_file_entry->fi_time_last_mod_present = TRUE;
                    cur_file_entry->fi_file_length_present = TRUE;
#ifdef PRINTING_DETAILS
                    {
                    dwarfstring m9c;
                    dwarfstring_constructor(&m9c);
                    dwarfstring_append_printf_s(&m9c,
                        "DW_LNE_define_file %s \n",
                        (char *)cur_file_entry->fi_file_name);
                    dwarfstring_append_printf_i(&m9c,
                        "    dir index %d\n",
                        (int) cur_file_entry->fi_dir_index);

                    {
                        time_t tt3 = (time_t) cur_file_entry->
                            fi_time_last_mod;

                        /* ctime supplies newline */
                        dwarfstring_append_printf_u(&m9c,
                            "    last time 0x%x ",
                            (Dwarf_Unsigned)tt3);
                        dwarfstring_append_printf_s(&m9c,
                            "%s",
                            ctime(&tt3));
                    }
                    dwarfstring_append_printf_i(&m9c,
                        "    file length %ld ",
                        cur_file_entry->fi_file_length);
                    dwarfstring_append_printf_u(&m9c,
                        "0x%lx\n",
                        cur_file_entry->fi_file_length);
                    _dwarf_printf(dbg,dwarfstring_string(&m9c));
                    dwarfstring_destructor(&m9c);
                    }
#endif /* PRINTING_DETAILS */
                }
                break;
            case DW_LNE_set_discriminator:{
                /* New in DWARF4 */
                int sdres = 0;
                Dwarf_Unsigned utmp2 = 0;

                sdres =  read_uword_de( &line_ptr,&utmp2,
                    dbg,error,line_ptr_end);
                if (sdres == DW_DLV_ERROR) {
                    _dwarf_free_chain_entries(dbg,head_chain,
                        line_count);
                    if (curr_line) {
                        dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                    }
                    return DW_DLV_ERROR;
                }
                regs.lr_discriminator = utmp2;

#ifdef PRINTING_DETAILS
                {
                dwarfstring mk;
                dwarfstring_constructor(&mk);
                dwarfstring_append_printf_u(&mk,
                    "DW_LNE_set_discriminator 0x%"
                    DW_PR_XZEROS DW_PR_DUx "\n",
                    utmp2);
                _dwarf_printf(dbg,dwarfstring_string(&mk));
                dwarfstring_destructor(&mk);
                }
#endif /* PRINTING_DETAILS */
                }
                break;
            default:{
                /*  This is an extended op code we do not know about,
                    other than we know now many bytes it is
                    and the op code and the bytes of operand. */
                Dwarf_Unsigned remaining_bytes = 0;
                /*  ptrdiff_t is generated but not named */
                Dwarf_Unsigned space_left =
                    (line_ptr <= line_ptr_end)?
                    (line_ptr_end - line_ptr):0xfffffff;
                if (instr_length > 0) {
                    remaining_bytes = instr_length -1;
                }

                /*  By catching this here instead of PRINTING_DETAILS
                    we avoid reading off of our data of interest*/
                if (instr_length < 1 ||
                    space_left < remaining_bytes ||
                    remaining_bytes > DW_LNE_LEN_MAX) {
                    dwarfstring g;
                    /*  ptrdiff_t is generated but not named */
                    Dwarf_Unsigned localoff =
                        (line_ptr >= section_start)?
                        (line_ptr - section_start):0xfffffff;

                    dwarfstring_constructor(&g);
                    dwarfstring_append_printf_u(&g,
                        "DW_DLE_LINE_TABLE_BAD reading "
                        "unknown DW_LNE_extended op opcode 0x%x ",
                        ext_opcode);
                    dwarfstring_append_printf_u(&g,
                        "we are "
                        "off this line table at section "
                        "offset 0x%x and ",
                        localoff);
                    dwarfstring_append_printf_u(&g,
                        "instruction length "
                        "%u.",instr_length);
                    _dwarf_error_string(dbg, error,
                        DW_DLE_LINE_TABLE_BAD,
                        dwarfstring_string(&g));
                    dwarfstring_destructor(&g);
                    if (curr_line) {
                        dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                    }
                    _dwarf_free_chain_entries(dbg,head_chain,
                        line_count);
                    return DW_DLV_ERROR;
                }

#ifdef PRINTING_DETAILS
                {
                dwarfstring m9d;
                dwarfstring_constructor(&m9d);
                dwarfstring_append_printf_u(&m9d,
                    "DW_LNE extended op 0x%x ",
                    ext_opcode);
                dwarfstring_append_printf_u(&m9d,
                    "Bytecount: %" DW_PR_DUu ,
                    (Dwarf_Unsigned)instr_length);
                if (remaining_bytes > 0) {
                    /*  If remaining bytes > distance to end
                        we will have an error. */
                    dwarfstring_append(&m9d," linedata: 0x");
                    while (remaining_bytes > 0) {
                        dwarfstring_append_printf_u(&m9d,
                            "%02x",
                            (unsigned char)(*(line_ptr)));
                        line_ptr++;
#if 0
                        /*  A test above (see space_left above)
                            proves we will not run off the end here.
                            The following test is too late anyway,
                            we might have read off the end just
                            before line_ptr incremented! */
                        if (line_ptr >= line_ptr_end) {
                            dwarfstring g;
                            /*  ptrdiff_t generated but not named */
                            Dwarf_Unsigned localoff =
                                (line_ptr >= section_start)?
                                (line_ptr - section_start):0xfffffff;

                            dwarfstring_constructor(&g);
                            dwarfstring_append_printf_u(&g,
                                "DW_DLE_LINE_TABLE_BAD reading "
                                "DW_LNE extended op remaining bytes "
                                "we are "
                                "off this line table at section "
                                "offset 0x%x .",
                                localoff);
                            _dwarf_error_string(dbg, error,
                                DW_DLE_LINE_TABLE_BAD,
                                dwarfstring_string(&g));
                            dwarfstring_destructor(&g);
                            dwarfstring_destructor(&m9d);
                            if (curr_line) {
                                dwarf_dealloc(dbg,curr_line,
                                    DW_DLA_LINE);
                            }
                            _dwarf_free_chain_entries(dbg,
                                head_chain,line_count);
                            return DW_DLV_ERROR;
                        }
#endif
                        remaining_bytes--;
                    }
                }
                _dwarf_printf(dbg,dwarfstring_string(&m9d));
                dwarfstring_destructor(&m9d);
                }
#else /* ! PRINTING_DETAILS */
                line_ptr += remaining_bytes;
                if (line_ptr > line_ptr_end) {
                    dwarfstring g;
                    /*  ptrdiff_t generated but not named */
                    Dwarf_Unsigned localoff =
                        (line_ptr >= section_start)?
                        (line_ptr - section_start):0xfffffff;

                    dwarfstring_constructor(&g);
                    dwarfstring_append_printf_u(&g,
                        "DW_DLE_LINE_TABLE_BAD reading "
                        "DW_LNE extended op remaining bytes "
                        "we are "
                        "off this line table at section "
                        "offset 0x%x .",
                        localoff);
                    _dwarf_error_string(dbg, error,
                        DW_DLE_LINE_TABLE_BAD,
                        dwarfstring_string(&g));
                    dwarfstring_destructor(&g);
                    if (curr_line) {
                        dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
                    }
                    _dwarf_free_chain_entries(dbg,head_chain,
                        line_count);
                    return DW_DLV_ERROR;
                }
#endif /* PRINTING_DETAILS */
                _dwarf_printf(dbg,"\n");
                }
                break;
            } /* End switch. */
        } else {
            /* ASSERT: impossible, see the macro definition */
            _dwarf_free_chain_entries(dbg,head_chain,
                line_count);
            _dwarf_error_string(dbg,error,
                DW_DLE_LINE_TABLE_BAD,
                "DW_DLE_LINE_TABLE_BAD: Actually is "
                "an impossible type from WHAT_IS_CODE");
            return DW_DLV_ERROR;
        }
    }
    block_line = (Dwarf_Line *)
        _dwarf_get_alloc(dbg, DW_DLA_LIST, line_count);
    if (block_line == NULL) {
        curr_chain = head_chain;
        _dwarf_free_chain_entries(dbg,head_chain,line_count);
        if (curr_line) {
            dwarf_dealloc(dbg,curr_line,DW_DLA_LINE);
            curr_line = 0;
        }
        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
        return DW_DLV_ERROR;
    }

    curr_chain = head_chain;
    for (i = 0; i < line_count; i++) {
        Dwarf_Chain t = 0;
        *(block_line + i) = curr_chain->ch_item;
        curr_chain->ch_item = 0;
        curr_chain->ch_itemtype = 0;
        t = curr_chain;
        curr_chain = curr_chain->ch_next;
        dwarf_dealloc(dbg, t, DW_DLA_CHAIN);
    }

    if (is_single_table || !is_actuals_table) {
        line_context->lc_linebuf_logicals = block_line;
        line_context->lc_linecount_logicals = line_count;
    } else {
        line_context->lc_linebuf_actuals = block_line;
        line_context->lc_linecount_actuals = line_count;
    }
#ifdef PRINTING_DETAILS
    {
    dwarfstring mc;
    dwarfstring_constructor(&mc);
    if (is_single_table) {
        if (!line_count) {
            dwarfstring_append_printf_u(&mc,
                " Line table is present (offset 0x%"
                DW_PR_XZEROS DW_PR_DUx
                ") but no lines present\n",
                line_context->lc_section_offset);
        }
    } else if (is_actuals_table) {
        if (!line_count) {
            dwarfstring_append_printf_u(&mc,
                " Line table present (offset 0x%"
                DW_PR_XZEROS DW_PR_DUx
                ") but no actuals lines present\n",
                line_context->lc_section_offset);
        }
    } else {
        if (!line_count) {
            dwarfstring_append_printf_u(&mc,
                " Line table present (offset 0x%"
                DW_PR_XZEROS DW_PR_DUx
                ") but no logicals lines present\n",
                line_context->lc_section_offset);
        }
    }
    if (dwarfstring_strlen(&mc)) {
        _dwarf_printf(dbg,dwarfstring_string(&mc));
    }
    dwarfstring_destructor(&mc);
    }
#endif /* PRINTING_DETAILS */
    return DW_DLV_OK;
}
