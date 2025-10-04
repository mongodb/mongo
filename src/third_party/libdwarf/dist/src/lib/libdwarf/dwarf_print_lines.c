/*
  Copyright (C) 2000,2002,2004,2005,2006 Silicon Graphics, Inc.  All Rights Reserved.
  Portions Copyright (C) 2007-2020 David Anderson. All Rights Reserved.
  Portions Copyright 2012 SN Systems Ltd. All rights reserved.

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

#include <config.h>

#include <stdlib.h> /* free() malloc() realloc() */
#include <string.h> /* memset() strlen() */
#include <time.h>   /* ctime() */

#if defined(_WIN32) && defined(HAVE_STDAFX_H)
#include "stdafx.h"
#endif /* HAVE_STDAFX_H */

#include "dwarf.h"
#include "libdwarf.h"
#include "dwarf_local_malloc.h"
#include "libdwarf_private.h"
#include "dwarf_base_types.h"
#include "dwarf_opaque.h"
#include "dwarf_alloc.h"
#include "dwarf_error.h"
#include "dwarf_util.h"
#include "dwarf_line.h"
#include "dwarf_string.h"

#define PRINTING_DETAILS 1
/*  for dwarfstring_constructor_static, saving lots of malloc
    and free but beware: make sure each routine using
    this DOES NOT call another routine using it.
    And do not use it in Dwarf_Error related calls.
    would be safer to have a buffer per function, but
    currently not necessary. */
static char locallinebuf[200];

static void
print_line_header(Dwarf_Debug dbg,
    Dwarf_Bool is_single_tab,
    Dwarf_Bool is_actuals_tab)
{
if (!is_single_tab) {
    /*  Ugly indenting follows, it makes lines shorter
        to see them better.
        Best to use a wider text window to really
        see how it looks.*/
if (is_actuals_tab) {
_dwarf_printf(dbg,"\nActuals Table\n");
_dwarf_printf(dbg,
"                                                         be\n"
"                                                         ls\n"
"                                                         ce\n"
" section    op                                           kq\n"
" offset     code                address/index    row isa ??\n");
    return;
} else {
_dwarf_printf(dbg,"\nLogicals Table\n");
_dwarf_printf(dbg,
"                                                        "
    "                      s pe\n"
"                                                        "
    "                      tirp\n"
"                                                        "
    "                      msoi\n"
" section          op                                    "
    "                      tall\n"
" offset      row  code                address/indx fil l"
    "ne col disc cntx subp ????\n");
    return;
}
}

/* Single level table */
_dwarf_printf(dbg,
"                                                         "
    "s b e p e i d\n"
"                                                         "
    "t l s r p s i\n"
"                                                         "
    "m c e o i a s\n"
" section    op                                       col "
    "t k q l l   c\n"
" offset     code               address     file line umn ? ? ? ? ?\n"
);
} /* End of function with ugly indenting. */

static void
print_line_detail(
    Dwarf_Debug dbg,
    const char *prefix,
    int opcode,
    unsigned curr_line,
    struct Dwarf_Line_Registers_s * regs,
    Dwarf_Bool is_single_table, Dwarf_Bool is_actuals_table)
{
    dwarfstring m1;

    dwarfstring_constructor_static(&m1,locallinebuf,
        sizeof(locallinebuf));
    if (!is_single_table && is_actuals_table) {
        dwarfstring_append_printf_s(&m1,"%-15s ",(char *)prefix);
        dwarfstring_append_printf_i(&m1,"%3d ",opcode);
        dwarfstring_append_printf_u(&m1,"0x%" DW_PR_XZEROS DW_PR_DUx,
            regs->lr_address);
        dwarfstring_append_printf_u(&m1,"/%01u",regs->lr_op_index);
        dwarfstring_append_printf_u(&m1," %5lu", regs->lr_line);
        dwarfstring_append_printf_u(&m1," %3u",regs->lr_isa);
        dwarfstring_append_printf_i(&m1,"   %1d",
            regs->lr_basic_block);
        dwarfstring_append_printf_i(&m1,"%1d\n",
            regs->lr_end_sequence);
        _dwarf_printf(dbg,dwarfstring_string(&m1));
        dwarfstring_destructor(&m1);
        return;
    }
    if (!is_single_table && !is_actuals_table) {
        dwarfstring_append_printf_i(&m1,
            "[%3d] "  /* row number */, curr_line);
        dwarfstring_append_printf_s(&m1,
            "%-15s ",(char *)prefix);
        dwarfstring_append_printf_i(&m1,
            "%3d ",opcode);
        dwarfstring_append_printf_u(&m1,
            "x%" DW_PR_XZEROS DW_PR_DUx, regs->lr_address);
        dwarfstring_append_printf_u(&m1,
            "/%01u", regs->lr_op_index);
        dwarfstring_append_printf_u(&m1," %2lu ",regs->lr_file);
        dwarfstring_append_printf_u(&m1,"%4lu  ",regs->lr_line);
        dwarfstring_append_printf_u(&m1,"%1lu",regs->lr_column);
        if (regs->lr_discriminator ||
            regs->lr_prologue_end ||
            regs->lr_epilogue_begin ||
            regs->lr_isa ||
            regs->lr_is_stmt ||
            regs->lr_call_context ||
            regs->lr_subprogram) {
            dwarfstring_append_printf_u(&m1,
                "   x%02" DW_PR_DUx ,
                regs->lr_discriminator); /* DWARF4 */
            dwarfstring_append_printf_u(&m1,
                "  x%02" DW_PR_DUx,
                regs->lr_call_context); /* EXPERIMENTAL */
            dwarfstring_append_printf_u(&m1,
                "  x%02" DW_PR_DUx ,
                regs->lr_subprogram); /* EXPERIMENTAL */
            dwarfstring_append_printf_i(&m1,
                "  %1d", regs->lr_is_stmt);
            dwarfstring_append_printf_i(&m1,
                "%1d", (int) regs->lr_isa);
            dwarfstring_append_printf_i(&m1,
                "%1d", regs->lr_prologue_end); /* DWARF3 */
            dwarfstring_append_printf_i(&m1,
                "%1d", regs->lr_epilogue_begin); /* DWARF3 */
        }
        dwarfstring_append(&m1,"\n");
        _dwarf_printf(dbg,dwarfstring_string(&m1));
        dwarfstring_destructor(&m1);
        return;
    }
    /*  In the first quoted line below:
        3d looks better than 2d, but best to do that as separate
        change and test from two-level-line-tables.  */
    dwarfstring_append_printf_s(&m1,
        "%-15s ",(char *)prefix);
    dwarfstring_append_printf_i(&m1,
        "%2d ",opcode);
    dwarfstring_append_printf_u(&m1,
        "0x%" DW_PR_XZEROS DW_PR_DUx " ",
        regs->lr_address);
    dwarfstring_append_printf_u(&m1,
        "%2lu   ", regs->lr_file);
    dwarfstring_append_printf_u(&m1,
        "%4lu ", regs->lr_line);
    dwarfstring_append_printf_u(&m1,
        "%2lu   ", regs->lr_column);
    dwarfstring_append_printf_i(&m1,
        "%1d ",regs->lr_is_stmt);
    dwarfstring_append_printf_i(&m1,
        "%1d ", regs->lr_basic_block);
    dwarfstring_append_printf_i(&m1,
        "%1d",regs->lr_end_sequence);
    if (regs->lr_discriminator ||
        regs->lr_prologue_end ||
        regs->lr_epilogue_begin ||
        regs->lr_isa) {
        dwarfstring_append_printf_i(&m1,
            " %1d", regs->lr_prologue_end); /* DWARF3 */
        dwarfstring_append_printf_i(&m1,
            " %1d", regs->lr_epilogue_begin); /* DWARF3 */
        dwarfstring_append_printf_i(&m1,
            " %1d", regs->lr_isa); /* DWARF3 */
        dwarfstring_append_printf_u(&m1,
            " 0x%" DW_PR_DUx , regs->lr_discriminator); /* DWARF4 */
    }
    dwarfstring_append(&m1, "\n");
    _dwarf_printf(dbg,dwarfstring_string(&m1));
    dwarfstring_destructor(&m1);
}

#include "dwarf_line_table_reader_common.h"

static void
print_include_directory_details(Dwarf_Debug dbg,
    unsigned int line_version,
    Dwarf_Line_Context line_context)
{
    Dwarf_Unsigned u = 0;
    dwarfstring    m4;
    Dwarf_Unsigned indexbase = 0;
    Dwarf_Unsigned indexlimit = 0;

    dwarfstring_constructor_static(&m4,locallinebuf,
        sizeof(locallinebuf));
    if (line_version == DW_LINE_VERSION5) {
        Dwarf_Unsigned i = 0;
        Dwarf_Unsigned dfcount =
            line_context->lc_directory_entry_format_count;

        dwarfstring_constructor(&m4);
        dwarfstring_append_printf_u(&m4,
            "  directory entry format count %u\n",dfcount);
        _dwarf_printf(dbg,dwarfstring_string(&m4));
        dwarfstring_reset(&m4);
        for ( ; i < dfcount;++i) {
            struct Dwarf_Unsigned_Pair_s *valpair = 0;
            const char *tname = 0;
            const char *fname = 0;
            int res;

            valpair = line_context->lc_directory_format_values +i;
            dwarfstring_append_printf_u(&m4,
                "  format [%2u] ",i);
            res = dwarf_get_LNCT_name((unsigned int)
                valpair->up_first, &tname);
            if ( res != DW_DLV_OK) {
                tname = "<unknown type>";
            }
            dwarfstring_append_printf_u (&m4,
                " type 0x%" DW_PR_XZEROS DW_PR_DUx,
                valpair->up_first);
            dwarfstring_append_printf_s (&m4,
                " %-20s\n",(char *)tname);
            res = dwarf_get_FORM_name((unsigned int)
                valpair->up_second,&fname);
            if ( res != DW_DLV_OK) {
                fname = "<unknown form>";
            }
            dwarfstring_append_printf_u(&m4,
                "               code 0x%" DW_PR_XZEROS DW_PR_DUx ,
                valpair->up_second);
            dwarfstring_append_printf_s(&m4,
                " %-20s\n", (char *)fname);
            _dwarf_printf(dbg,dwarfstring_string(&m4));
            dwarfstring_reset(&m4);

        }
    }
    /*  Common print of the directories.
        For DWARF 2,3,4 it has always started
        the indexing at 0 even though the directory index
        in line entries starts at 1 (zero meaning
        current directory at compile time).
        That is odd, given the non-dash-v printed
        starting at 1.  So lets adjust for consistency. */
    if (line_version == DW_LINE_VERSION5) {
        dwarfstring_append_printf_i(&m4,
            "  include directories count %d\n",
            (int) line_context->lc_include_directories_count);
    } else {
        if (!line_context->lc_include_directories_count) {
            dwarfstring_append_printf_i(&m4,
                "  include directories count %d\n",
                (int) line_context->lc_include_directories_count);
        } else {
            dwarfstring_append_printf_i(&m4,
                "  include directories count %d"
                " (index starts at 1)\n",
                (int) line_context->lc_include_directories_count);
        }
    }
    _dwarf_printf(dbg,dwarfstring_string(&m4));
    dwarfstring_reset(&m4);
    if (line_version == DW_LINE_VERSION5) {
        indexbase = 0;
        indexlimit =  line_context->lc_include_directories_count;
    } else {
        indexbase = 1;
        indexlimit = 1 + line_context->lc_include_directories_count;
    }
    for (u = indexbase; u < indexlimit; ++u) {
        dwarfstring_append_printf_u(&m4,
            "  include dir[%u] ",u);
        dwarfstring_append_printf_s(&m4,
            "%s\n",(char *)
            line_context->lc_include_directories[u-indexbase]);
        _dwarf_printf(dbg,dwarfstring_string(&m4));
        dwarfstring_reset(&m4);
    }
    dwarfstring_destructor(&m4);
}

static void
print_just_file_entry_details(Dwarf_Debug dbg,
    Dwarf_Line_Context line_context)
{
    unsigned fiu = 0;
    Dwarf_File_Entry fe = line_context->lc_file_entries;
    Dwarf_File_Entry fe2 = fe;
    dwarfstring m3;
    unsigned increment = 1;

    if (line_context->lc_version_number == DW_LINE_VERSION5 ) {
        increment = 0;
    }
    dwarfstring_constructor_static(&m3,locallinebuf,
        sizeof(locallinebuf));
    dwarfstring_append_printf_i(&m3,
        "  file names count      %d\n",
        line_context->lc_file_entry_count);
    _dwarf_printf(dbg,dwarfstring_string(&m3));
    dwarfstring_reset(&m3);
    for (fiu = 0 ; fe2 ; fe2 = fe->fi_next,++fiu ) {
        Dwarf_Unsigned tlm2 = 0;
        unsigned filenum = 0;

        fe = fe2;
        tlm2 = fe->fi_time_last_mod;
        filenum = fiu+increment;

        /*  The space character at the end of line is silly,
            but lets leave it there for the moment to avoid
            changing output.  */
        if (line_context->lc_file_entry_count > 9) {
            dwarfstring_append_printf_u(&m3,
                "  file[%2u] ",fiu);
        } else {
            dwarfstring_append_printf_u(&m3,
                "  file[%u]  ", fiu);
        }
        /*  DWARF5 can have a null fi_file_name
            if the format code in the
            line table header is unknown, such
            as in a corrupt object file. */
        dwarfstring_append_printf_s(&m3,
            "%-20s ",
            fe->fi_file_name?
            (char *) fe->fi_file_name:
            "<no file name>");
        dwarfstring_append_printf_u(&m3,
            "(file-number: %u)\n",
            filenum);
        _dwarf_printf(dbg,dwarfstring_string(&m3));
        dwarfstring_reset(&m3);
        if (fe->fi_dir_index_present) {
            Dwarf_Unsigned di = 0;
            di = fe->fi_dir_index;
            dwarfstring_append_printf_i(&m3,
                "    dir index %d\n", di);
        }
        if (fe->fi_time_last_mod_present) {
            time_t tt = (time_t) tlm2;

            /* ctime supplies newline */
            dwarfstring_append_printf_u(&m3,
                "    last time 0x%x ",tlm2);
            dwarfstring_append(&m3,(char *)ctime(&tt));
        }
        if (fe->fi_file_length_present) {
            Dwarf_Unsigned fl = 0;

            fl = fe->fi_file_length;
            dwarfstring_append_printf_i(&m3,
                "    file length %ld ",fl);
            dwarfstring_append_printf_u(&m3,
                "0x%lx\n",fl);
        }
        if (fe->fi_md5_present) {
            char *c = (char *)&fe->fi_md5_value;
            char *end = c+sizeof(fe->fi_md5_value);
            dwarfstring_append(&m3, "    file md5 value 0x");
            while(c < end) {
                dwarfstring_append_printf_u(&m3,
                    "%02x",0xff&*c);
                ++c;
            }
            dwarfstring_append(&m3,"\n");
        }
        if (fe->fi_llvm_source) {
            dwarfstring_append_printf_s(&m3,
                "%-20s\n",
                (char *) fe->fi_llvm_source);
        }
        if (fe->fi_gnu_subprogram_name) {
            dwarfstring_append_printf_s(&m3,
                "%-20s\n",
                (char *) fe->fi_gnu_subprogram_name);
        }
        if (fe->fi_gnu_decl_file_present) {
            Dwarf_Unsigned di = 0;
            di = fe->fi_gnu_decl_file;
            dwarfstring_append_printf_i(&m3,
                "    gnu decl file %d\n", di);
        }
        if (fe->fi_gnu_decl_line_present) {
            Dwarf_Unsigned di = 0;
            di = fe->fi_gnu_decl_line;
            dwarfstring_append_printf_i(&m3,
                "    gnu decl line %d\n", di);
        }
        if (dwarfstring_strlen(&m3)) {
            _dwarf_printf(dbg,dwarfstring_string(&m3));
            dwarfstring_reset(&m3);
        }
    }
    dwarfstring_destructor(&m3);
}

static void
print_file_entry_details(Dwarf_Debug dbg,
    unsigned int line_version,
    Dwarf_Line_Context line_context)
{
    dwarfstring m5;

    dwarfstring_constructor_static(&m5,locallinebuf,
        sizeof(locallinebuf));
    if (line_version == DW_LINE_VERSION5) {
        Dwarf_Unsigned i = 0;
        Dwarf_Unsigned dfcount =
            line_context->lc_file_name_format_count;

        dwarfstring_append_printf_u(&m5,
            "  file entry format count      %u\n",dfcount);
        for ( ; i < dfcount;++i) {
            struct Dwarf_Unsigned_Pair_s *valpair = 0;
            const char *tname = 0;
            const char *fname = 0;
            int res;

            valpair = line_context->lc_file_format_values +i;
            dwarfstring_append_printf_u(&m5,
                "  format [%2u] ",i);
            res = dwarf_get_LNCT_name((unsigned int)
                valpair->up_first,&tname);
            if ( res != DW_DLV_OK) {
                tname = "<unknown type>";
            }
            dwarfstring_append_printf_u(&m5,
                " type 0x%" DW_PR_XZEROS DW_PR_DUx,
                valpair->up_first);
            dwarfstring_append_printf_s(&m5,
                " %-20s\n",(char *)tname);
            res = dwarf_get_FORM_name((unsigned int)
                valpair->up_second,&fname);
            if ( res != DW_DLV_OK) {
                fname = "<unknown form>";
            }
            dwarfstring_append_printf_u(&m5,
                "               code 0x%"
                DW_PR_XZEROS DW_PR_DUx,
                valpair->up_second);
            dwarfstring_append_printf_s(&m5, " %-20s\n",
                (char *)fname);
            _dwarf_printf(dbg,dwarfstring_string(&m5));
            dwarfstring_reset(&m5);
        }
        dwarfstring_destructor(&m5);
        print_just_file_entry_details(dbg,line_context);
    } else {
        print_just_file_entry_details(dbg,line_context);
        dwarfstring_destructor(&m5);
    }
}

static void
print_experimental_subprograms_list(Dwarf_Debug dbg,
    Dwarf_Line_Context line_context)
{
    /*  Print the subprograms list. */
    Dwarf_Unsigned count = line_context->lc_subprogs_count;
    Dwarf_Unsigned exu = 0;
    Dwarf_Subprog_Entry sub = line_context->lc_subprogs;
    dwarfstring m6;

    dwarfstring_constructor_static(&m6,locallinebuf,
        sizeof(locallinebuf));
    dwarfstring_append_printf_u(&m6,
        "  subprograms count %" DW_PR_DUu "\n",count);
    if (count > 0) {
        dwarfstring_append(&m6,"    indx  file   line   name\n");
    }
    _dwarf_printf(dbg,dwarfstring_string(&m6));
    dwarfstring_reset(&m6);
    for (exu = 0 ; exu < count ; exu++,sub++) {
        dwarfstring_append_printf_u(&m6,
            "    [%2" DW_PR_DUu,exu+1);
        dwarfstring_append_printf_u(&m6,
            "] %4" DW_PR_DUu,sub->ds_decl_file);
        dwarfstring_append_printf_u(&m6,
            "    %4" DW_PR_DUu ,sub->ds_decl_line);
        dwarfstring_append_printf_s(&m6,
            " %s\n",(char *)sub->ds_subprog_name);
        _dwarf_printf(dbg,dwarfstring_string(&m6));
        dwarfstring_reset(&m6);
    }
    dwarfstring_destructor(&m6);
}

static void
do_line_print_now(Dwarf_Debug dbg,int line_version,
    Dwarf_Small * comp_dir,
    Dwarf_Line_Context line_context) ;
static void print_experimental_counts(Dwarf_Debug dbg,
    int line_version,
    Dwarf_Line_Context line_context);

static int print_actuals_and_locals(Dwarf_Debug dbg,
    Dwarf_Line_Context line_context,
    Dwarf_Unsigned bogus_bytes_count,
    Dwarf_Small *bogus_bytes_ptr,
    Dwarf_Small *orig_line_ptr,
    Dwarf_Small *line_ptr,
    Dwarf_Small *section_start,
    Dwarf_Small *line_ptr_actuals,
    Dwarf_Small *line_ptr_end,
    Dwarf_Half   address_size,
    int *        err_count_out,
    Dwarf_Error *error);

/*  return DW_DLV_OK if ok. else DW_DLV_NO_ENTRY or DW_DLV_ERROR
    If err_count_out is non-NULL, this is a special 'check'
    call.  */
static int
_dwarf_internal_printlines(Dwarf_Die die,
    int * err_count_out,
    int only_line_header,
    Dwarf_Error * error)
{
    /*  This pointer is used to scan the portion of the .debug_line
        section for the current cu. */
    Dwarf_Small *line_ptr = 0;
    Dwarf_Small *orig_line_ptr = 0;

    /*  Pointer to a DW_AT_stmt_list attribute in case
        it exists in the die. */
    Dwarf_Attribute stmt_list_attr = 0;

    /*  Pointer to DW_AT_comp_dir attribute in die. */
    Dwarf_Attribute comp_dir_attr = 0;

    /*  Pointer to name of compilation directory. */
    Dwarf_Small *comp_dir = NULL;

    /*  Offset into .debug_line specified by a DW_AT_stmt_list
        attribute. */
    Dwarf_Unsigned line_offset = 0;

    /*  These variables are used to decode leb128 numbers. Leb128_num
        holds the decoded number, and leb128_length is its length in
        bytes. */
    Dwarf_Half attrform = 0;

    /*  In case there are weird bytes 'after' the line table
        prologue this lets us print something. This is a gcc
        compiler bug and we expect the bytes count to be 12.  */
    Dwarf_Small* bogus_bytes_ptr = 0;
    Dwarf_Unsigned bogus_bytes_count = 0;
    Dwarf_Half address_size = 0;
    Dwarf_Unsigned fission_offset = 0;
    unsigned line_version = 0;

    /* The Dwarf_Debug this die belongs to. */
    Dwarf_Debug dbg = 0;
    Dwarf_CU_Context cu_context = 0;
    Dwarf_Line_Context line_context = 0;
    int resattr = DW_DLV_ERROR;
    int lres =    DW_DLV_ERROR;
    int res  =    DW_DLV_ERROR;
    Dwarf_Small *line_ptr_actuals  = 0;
    Dwarf_Small *line_ptr_end = 0;
    Dwarf_Small *section_start = 0;

    /* ***** BEGIN CODE ***** */

    if (error != NULL) {
        *error = NULL;
    }

    CHECK_DIE(die, DW_DLV_ERROR);
    cu_context = die->di_cu_context;
    dbg = cu_context->cc_dbg;

    res = _dwarf_load_section(dbg, &dbg->de_debug_line,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    if (!dbg->de_debug_line.dss_size) {
        return DW_DLV_NO_ENTRY;
    }

    address_size = _dwarf_get_address_size(dbg, die);
    resattr = dwarf_attr(die, DW_AT_stmt_list, &stmt_list_attr,
        error);
    if (resattr != DW_DLV_OK) {
        return resattr;
    }
    /*  The list of relevant FORMs is small.
        DW_FORM_data4, DW_FORM_data8, DW_FORM_sec_offset
    */
    lres = dwarf_whatform(stmt_list_attr,&attrform,error);
    if (lres != DW_DLV_OK) {
        dwarf_dealloc_attribute(stmt_list_attr);
        return lres;
    }
    if (attrform != DW_FORM_data4 && attrform != DW_FORM_data8 &&
        attrform != DW_FORM_sec_offset ) {
        dwarf_dealloc_attribute(stmt_list_attr);
        _dwarf_error(dbg, error, DW_DLE_LINE_OFFSET_BAD);
        return DW_DLV_ERROR;
    }
    lres = dwarf_global_formref(stmt_list_attr, &line_offset, error);
    if (lres != DW_DLV_OK) {
        dwarf_dealloc_attribute(stmt_list_attr);
        return lres;
    }

    if (line_offset >= dbg->de_debug_line.dss_size) {
        dwarf_dealloc_attribute(stmt_list_attr);
        _dwarf_error(dbg, error, DW_DLE_LINE_OFFSET_BAD);
        return DW_DLV_ERROR;
    }
    section_start =  dbg->de_debug_line.dss_data;
    {
        Dwarf_Unsigned fission_size = 0;
        int resfis = _dwarf_get_fission_addition_die(die,
            DW_SECT_LINE,
            &fission_offset,&fission_size,error);
        if (resfis != DW_DLV_OK) {
            dwarf_dealloc_attribute(stmt_list_attr);
            return resfis;
        }
    }

    orig_line_ptr = section_start + line_offset + fission_offset;
    line_ptr = orig_line_ptr;
    dwarf_dealloc_attribute(stmt_list_attr);

    /*  If die has DW_AT_comp_dir attribute, get the string
        that names the compilation directory. */
    resattr = dwarf_attr(die, DW_AT_comp_dir, &comp_dir_attr, error);
    if (resattr == DW_DLV_ERROR) {
        return resattr;
    }
    if (resattr == DW_DLV_OK) {
        int cres = DW_DLV_ERROR;
        char *cdir = 0;

        cres = dwarf_formstring(comp_dir_attr, &cdir, error);
        if (cres == DW_DLV_ERROR) {
            dwarf_dealloc_attribute(comp_dir_attr);
            comp_dir_attr = 0;
            return cres;
        }
        if (cres == DW_DLV_OK) {
            comp_dir = (Dwarf_Small *) cdir;
        }
    }
    if (comp_dir_attr) {
        dwarf_dealloc_attribute(comp_dir_attr);
        comp_dir_attr = 0;
    }
    line_context = (Dwarf_Line_Context)
        _dwarf_get_alloc(dbg, DW_DLA_LINE_CONTEXT, 1);
    if (line_context == NULL) {
        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
        return DW_DLV_ERROR;
    }
    {
        Dwarf_Small *newlinep = 0;
        int dres = _dwarf_read_line_table_header(dbg,
            cu_context,
            section_start,
            line_ptr,
            dbg->de_debug_line.dss_size,
            &newlinep,
            line_context,
            &bogus_bytes_ptr,
            &bogus_bytes_count,
            error,
            err_count_out);
        if (dres == DW_DLV_ERROR) {
            dwarf_srclines_dealloc_b(line_context);
            return dres;
        }
        if (dres == DW_DLV_NO_ENTRY) {
            dwarf_srclines_dealloc_b(line_context);
            return dres;
        }
        line_ptr_end = line_context->lc_line_ptr_end;
        line_ptr = newlinep;
        if (line_context->lc_actuals_table_offset > 0) {
            line_ptr_actuals = line_context->lc_line_prologue_start +
                line_context->lc_actuals_table_offset;
        }
    }
    line_version = line_context->lc_version_number;
    line_context->lc_compilation_directory = comp_dir;
    if (only_line_header) {
        /* Just checking for header errors, nothing more here.*/
        dwarf_srclines_dealloc_b(line_context);
        return DW_DLV_OK;
    }
    do_line_print_now(dbg,line_version,comp_dir,line_context);
    print_include_directory_details(dbg,line_version,line_context);
    print_file_entry_details(dbg,line_version,line_context);
    print_experimental_counts(dbg, line_version,line_context);
    res = print_actuals_and_locals(dbg, line_context,
        bogus_bytes_count,bogus_bytes_ptr,
        orig_line_ptr,
        line_ptr,
        section_start,
        line_ptr_actuals,
        line_ptr_end,
        address_size,
        err_count_out,
        error);
    if (res  !=  DW_DLV_OK) {
        return res;
    }
    return DW_DLV_OK;
}

static void
do_line_print_now(Dwarf_Debug dbg,
    int line_version,
    Dwarf_Small *comp_dir,
    Dwarf_Line_Context line_context)
{
    dwarfstring m7;
    Dwarf_Unsigned i = 0;

    dwarfstring_constructor(&m7);
    dwarfstring_append_printf_i(&m7,
        "total line info length %ld bytes,",
        line_context->lc_total_length);

    dwarfstring_append_printf_u(&m7,
        " line offset 0x%" DW_PR_XZEROS DW_PR_DUx,
        line_context->lc_section_offset);

    dwarfstring_append_printf_u(&m7,
        " %" DW_PR_DUu "\n",
        line_context->lc_section_offset);

    if (line_version <= DW_LINE_VERSION5) {
        dwarfstring_append_printf_i(&m7,
            "  line table version     %d\n",
        (int) line_context->lc_version_number);
    } else {
        dwarfstring_append_printf_u(&m7,
            "  line table version 0x%x\n",
            (int) line_context->lc_version_number);
    }
    if (line_version == DW_LINE_VERSION5) {
        dwarfstring_append_printf_i(&m7,
            "  address size          %d\n",
            line_context->lc_address_size);
        dwarfstring_append_printf_i(&m7,
            "  segment selector size %d\n",
            line_context->lc_segment_selector_size);
    }
    _dwarf_printf(dbg,dwarfstring_string(&m7));
    dwarfstring_reset(&m7);
    dwarfstring_append_printf_i(&m7,
        "  line table length field length %d\n",
        line_context->lc_length_field_length);
    dwarfstring_append_printf_i(&m7,
        "  prologue length       %d\n",
        line_context->lc_prologue_length);
    dwarfstring_append_printf_s(&m7,
        "  compilation_directory %s\n",
        comp_dir ? ((char *) comp_dir) : "");

    dwarfstring_append_printf_i(&m7,
        "  min instruction length %d\n",
        line_context->lc_minimum_instruction_length);
    _dwarf_printf(dbg,dwarfstring_string(&m7));
    dwarfstring_reset(&m7);
    if (line_version == DW_LINE_VERSION5 ||
        line_version == DW_LINE_VERSION4 ||
        line_version == EXPERIMENTAL_LINE_TABLES_VERSION) {
        dwarfstring_append_printf_u(&m7,
            "  maximum ops per instruction %u\n",
            line_context->lc_maximum_ops_per_instruction);
        _dwarf_printf(dbg,dwarfstring_string(&m7));
        dwarfstring_reset(&m7);
    }
    if (line_version == EXPERIMENTAL_LINE_TABLES_VERSION) {
        dwarfstring_append_printf_u(&m7, "  actuals table offset "
            "0x%" DW_PR_XZEROS DW_PR_DUx "\n",
            line_context->lc_actuals_table_offset);
        dwarfstring_append_printf_u(&m7,"  logicals table offset "
            "0x%" DW_PR_XZEROS DW_PR_DUx "\n",
            line_context->lc_logicals_table_offset);
        _dwarf_printf(dbg,dwarfstring_string(&m7));
        dwarfstring_reset(&m7);
    }
    dwarfstring_append_printf_i(&m7,
        "  default is stmt        %d\n",
        (int)line_context->lc_default_is_stmt);
    dwarfstring_append_printf_i(&m7,
        "  line base              %d\n",
        (int)line_context->lc_line_base);
    dwarfstring_append_printf_i(&m7,
        "  line_range             %d\n",
        (int)line_context->lc_line_range);
    dwarfstring_append_printf_i(&m7,
        "  opcode base            %d\n",
        (int)line_context->lc_opcode_base);
    dwarfstring_append_printf_i(&m7,
        "  standard opcode count  %d\n",
        (int)line_context->lc_std_op_count);
    _dwarf_printf(dbg,dwarfstring_string(&m7));
    dwarfstring_reset(&m7);

    for (i = 1; i < line_context->lc_opcode_base; i++) {
        dwarfstring_append_printf_i(&m7,
            "  opcode[%2d] length", (int) i);
        dwarfstring_append_printf_i(&m7,
            "  %d\n",
            (int) line_context->lc_opcode_length_table[i - 1]);
        _dwarf_printf(dbg,dwarfstring_string(&m7));
        dwarfstring_reset(&m7);
    }
    dwarfstring_destructor(&m7);
}

static void
print_experimental_counts(Dwarf_Debug dbg, int line_version,
    Dwarf_Line_Context line_context)
{
    if (line_version == EXPERIMENTAL_LINE_TABLES_VERSION) {
        print_experimental_subprograms_list(dbg,line_context);
    }
}

static int
print_actuals_and_locals(Dwarf_Debug dbg,
    Dwarf_Line_Context line_context,
    Dwarf_Unsigned bogus_bytes_count,
    Dwarf_Small *bogus_bytes_ptr,
    Dwarf_Small *orig_line_ptr,
    Dwarf_Small *line_ptr,
    Dwarf_Small *section_start,
    Dwarf_Small *line_ptr_actuals,
    Dwarf_Small *line_ptr_end,
    Dwarf_Half   address_size,
    int *        err_count_out,
    Dwarf_Error *error)
{
    int res = 0;
    dwarfstring m8;
    Dwarf_Unsigned offset = 0;

    dwarfstring_constructor(&m8);
    if (bogus_bytes_count > 0) {
        Dwarf_Unsigned wcount = bogus_bytes_count;
        Dwarf_Unsigned boffset = bogus_bytes_ptr - section_start;

        dwarfstring_append_printf_u(&m8,
            "*** DWARF CHECK: the line table prologue  header_length "
            " is %" DW_PR_DUu " too high, we pretend it is smaller.",
            wcount);
        dwarfstring_append_printf_u(&m8,
            "Section offset: 0x%"
            DW_PR_XZEROS DW_PR_DUx,
            boffset);
        dwarfstring_append_printf_u(&m8,
            " (%" DW_PR_DUu ") ***\n",
            boffset);
        *err_count_out += 1;
    }
    offset = line_ptr - section_start;
    dwarfstring_append_printf_u(&m8,
        "  statement prog offset in section: 0x%"
        DW_PR_XZEROS DW_PR_DUx,
        offset);
    dwarfstring_append_printf_u(&m8,
        " (%" DW_PR_DUu ")\n",
        offset);
    _dwarf_printf(dbg,dwarfstring_string(&m8));
    dwarfstring_reset(&m8);

    {
        Dwarf_Bool doaddrs = FALSE;
        Dwarf_Bool dolines = TRUE;

        if (!line_ptr_actuals) {
            /* Normal single level line table. */

            Dwarf_Bool is_single_table = TRUE;
            Dwarf_Bool is_actuals_table = FALSE;
            print_line_header(dbg, is_single_table, is_actuals_table);
            res = read_line_table_program(dbg,
                line_ptr, line_ptr_end, orig_line_ptr,
                section_start,
                line_context,
                address_size, doaddrs, dolines,
                is_single_table,
                is_actuals_table,
                error,
                err_count_out);
            if (res != DW_DLV_OK) {
                dwarfstring_destructor(&m8);
                dwarf_srclines_dealloc_b(line_context);
                return res;
            }
        } else {
            Dwarf_Bool is_single_table = FALSE;
            Dwarf_Bool is_actuals_table = FALSE;
            if (line_context->lc_version_number !=
                EXPERIMENTAL_LINE_TABLES_VERSION) {
                dwarf_srclines_dealloc_b(line_context);
                dwarfstring_destructor(&m8);
                _dwarf_error(dbg, error, DW_DLE_VERSION_STAMP_ERROR);
                return DW_DLV_ERROR;
            }
            /* Read Logicals */
            print_line_header(dbg, is_single_table, is_actuals_table);
            res = read_line_table_program(dbg,
                line_ptr, line_ptr_actuals, orig_line_ptr,
                section_start,
                line_context,
                address_size, doaddrs, dolines,
                is_single_table,
                is_actuals_table,
                error,err_count_out);
            if (res != DW_DLV_OK) {
                dwarfstring_destructor(&m8);
                dwarf_srclines_dealloc_b(line_context);
                return res;
            }
            if (line_context->lc_actuals_table_offset > 0) {
                is_actuals_table = TRUE;
                /* Read Actuals */

                print_line_header(dbg, is_single_table,
                    is_actuals_table);
                res = read_line_table_program(dbg,
                    line_ptr_actuals, line_ptr_end, orig_line_ptr,
                    section_start,
                    line_context,
                    address_size, doaddrs, dolines,
                    is_single_table,
                    is_actuals_table,
                    error,
                    err_count_out);
                if (res != DW_DLV_OK) {
                    dwarfstring_destructor(&m8);
                    dwarf_srclines_dealloc_b(line_context);
                    return res;
                }
            }
        }
    }
    dwarfstring_destructor(&m8);
    dwarf_srclines_dealloc_b(line_context);
    return DW_DLV_OK;
}

/*  This is support for dwarfdump: making it possible
    for clients wanting line detail info on stdout
    to get that detail without including internal libdwarf
    header information.
    Caller passes in compilation unit DIE.

    These *print_lines() functions print two-level tables in full
    even when the user is not asking for both (ie, when
    the caller asked for dwarf_srclines().
    It was an accident, but after a short reflection
    this seems like a good idea for -vvv. */
int
dwarf_print_lines(Dwarf_Die die,
    Dwarf_Error * error,
    int *error_count)
{
    int only_line_header = 0;
    int res = _dwarf_internal_printlines(die,
        error_count,
        only_line_header,error);
    return res;
}

/* The check is in case we are not printing full line data,
   this gets some of the issues noted with .debug_line,
   but not all. Call dwarf_print_lines() to get all issues.
   Intended for apps like dwarfdump.
   dwarf_check_lineheader_b() new 14 April 2020.
*/
int
dwarf_check_lineheader_b(Dwarf_Die die, int *err_count_out,
    Dwarf_Error *error)
{
    int res = 0;

    int only_line_header = 1;
    res = _dwarf_internal_printlines(die,err_count_out,
        only_line_header,error);
    return res;
}
