/* Copyright (C) 2000-2006 Silicon Graphics, Inc.  All Rights Reserved.
   Portions Copyright (C) 2007-2020 David Anderson. All Rights Reserved.
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

#include <config.h>

#ifdef HAVE_STDINT_H
#include <stdint.h> /* uintptr_t */
#endif /* HAVE_STDINT_H */
#include <stdlib.h> /* free() malloc() realloc() */
#include <string.h> /* memset() strlen() */

#if defined(_WIN32) && defined(HAVE_STDAFX_H)
#include "stdafx.h"
#endif /* HAVE_STDAFX_H */

#include "dwarf.h"
#include "libdwarf.h"
#include "dwarf_local_malloc.h"
#include "libdwarf_private.h"
#include "dwarf_base_types.h"
#include "dwarf_safe_strcpy.h"
#include "dwarf_opaque.h"
#include "dwarf_alloc.h"
#include "dwarf_error.h"
#include "dwarf_util.h"
#include "dwarf_line.h"
#include "dwarf_string.h"
#include "dwarf_debuglink.h"

/* Line Register Set initial conditions. */
static struct Dwarf_Line_Registers_s
    _dwarf_line_table_regs_default_values = {
    /* Dwarf_Addr lr_address */ 0,
    /* Dwarf_Unsigned lr_file */ 1,
    /* Dwarf_Unsigned lr_line */  1,
    /* Dwarf_Unsigned lr_column */  0,
    /* Dwarf_Bool lr_is_stmt */  FALSE,
    /* Dwarf_Bool lr_basic_block */  FALSE,
    /* Dwarf_Bool lr_end_sequence */  FALSE,
    /* Dwarf_Bool lr_prologue_end */  FALSE,
    /* Dwarf_Bool lr_epilogue_begin */  FALSE,
    /* Dwarf_Small lr_isa */  0,
    /* Dwarf_Unsigned lr_op_index  */  0,
    /* Dwarf_Unsigned lr_discriminator */  0,
    /* Dwarf_Unsigned lr_call_context */  0,
    /* Dwarf_Unsigned lr_subprogram */  0,
};

void
_dwarf_set_line_table_regs_default_values(Dwarf_Line_Registers regs,
    unsigned lineversion,
    Dwarf_Bool is_stmt)
{
    (void)lineversion;
    *regs = _dwarf_line_table_regs_default_values;
    /*  Remember that 0xf006 is the version of
        the experimental line table */
    if (lineversion == DW_LINE_VERSION5) {
        /*  DWARF5 Section 2.14 says default 0 for line table
            file numbering..
            DWARF5 Table 6.4 says the line table file
            register defaults to 1 (as did DWARF2,3,4).

            gcc 11.2.0 uses line register
            default 1, while correctly numbering files from 0.
            it sets file 0 and file 2 to the file of the CU
            and file 1 applies to an included file
            so things work when the first line table
            entries needed come from an included file
            (such as a static inline function definition).
            See regressiontests/issue137gh/README

            clang 14 entirely avoids use of the default
            file register value, it always uses
            DW_LNS_set_file in the line table. */
        regs->lr_file = 1;
    }
    regs->lr_is_stmt = is_stmt;
}

/*  Detect Windows full paths as well as Unix/Linux.
    ASSERT: fname != NULL  */
Dwarf_Bool
_dwarf_file_name_is_full_path(Dwarf_Small  *fname)
{
    Dwarf_Small firstc = *fname;
    /*  Not relative path if
        - path begins with \\ (UNC path)
        - path begins with ?:\, with ? being a letter
        - path bagins with \
        see
        https://docs.microsoft.com/en-us/windows/win32/\
        fileio/naming-a-file#paths */
    if (!firstc) {
        return FALSE;
    }
    if (firstc == '/') {
        return TRUE;
    }
    if (firstc == '\\') {
        return TRUE;
    }
    /*  We assume anything starting with c: (etc)
        is a genuine Windows name. That turns out
        to be important as we dump PE objects on
        linux! It's safe too, as a specially crafted
        file might have add path output, but would
        not break anything.  */
    if (((firstc >= 'a') && (firstc <= 'z')) ||
        ((firstc >= 'A') && (firstc <= 'Z'))) {
        if (fname[1] == ':') {
            /*  Some test cases use /, some \\ */
            if (fname[2] == '\\') {
                return TRUE;
            }
            if (fname[2] == '/') {
                return TRUE;
            }
            /*  This is a relative path to the current
                directory on the drive named.
                Windows has a 'current directory'
                with each drive letter in use.  */
        }
    }
    return FALSE;
}
#include "dwarf_line_table_reader_common.h"

/*  Used for a short time in the next two functions.
    Not saved.  If multithreading ever allowed this
    will have to change to be function local
    non-static buffers. */
static char targbuf[300];
static char nbuf[300];

static int
ret_simple_full_path(Dwarf_Debug dbg,
    char *file_name,
    char ** name_ptr_out,
    Dwarf_Error *error)
{
    char *tmp = 0;
    char * mstr = 0;
    size_t mlen = 0;
    dwarfstring targ;
    dwarfstring nxt;

    dwarfstring_constructor_static(&targ,
        targbuf,sizeof(targbuf));
    dwarfstring_constructor_static(&nxt,
        nbuf,sizeof(nbuf));

    dwarfstring_append(&nxt,file_name);
    _dwarf_pathjoinl(&targ,&nxt);
    mstr= dwarfstring_string(&targ);
    mlen = dwarfstring_strlen(&targ) +1;
    tmp = (char *) _dwarf_get_alloc(dbg, DW_DLA_STRING,
        (Dwarf_Unsigned)mlen);
    if (tmp) {
        _dwarf_safe_strcpy(tmp,mlen, mstr,mlen-1);
        *name_ptr_out = tmp;
        dwarfstring_destructor(&targ);
        dwarfstring_destructor(&nxt);
        return DW_DLV_OK;
    }
    dwarfstring_destructor(&targ);
    dwarfstring_destructor(&nxt);
    _dwarf_error_string(dbg,error,DW_DLE_ALLOC_FAIL,
        "DW_DLE_ALLOC_FAIL: "
        "Allocation of space for a simple full path "
        "from line table header data fails." );
    return DW_DLV_ERROR;
}

static void
_dwarf_dirno_string(Dwarf_Line_Context line_context,
    Dwarf_Unsigned dirno,
    unsigned include_dir_offset,
    dwarfstring *dwstr_out)
{
    if ((dirno - include_dir_offset) >=
        line_context->lc_include_directories_count) {

        /*  Corrupted data. We try to continue.
            Make the text look like a full-path */
        dwarfstring_append_printf_u(dwstr_out,
            "/ERROR<corrupt include directory index %u"
            " unusable,",
            dirno);
        dwarfstring_append_printf_u(dwstr_out,
            " only %u directories present>",
            line_context->lc_include_directories_count);
        return;
    }
    {
        char *inc_dir_name =
            (char *)line_context->lc_include_directories[
            dirno - include_dir_offset];
        if (!inc_dir_name) {
            /*  This should never ever happen except in case
                of a corrupted object file.
                Make the text look like a full-path */
            inc_dir_name =
                "/ERROR<erroneous NULL include dir pointer>";
        }
        dwarfstring_append(dwstr_out,inc_dir_name);
    }
    return;
}

/*  With this routine we ensure the file full path
    is calculated identically for
    dwarf_srcfiles() and _dwarf_filename()

    As of March 14 2020 this *always*
    does an allocation for the string. dwarf_dealloc
    is crucial to do no matter what.
    So we have consistency.

    dwarf_finish() will do the dealloc if nothing else does.
    Unless the calling application did the call
    dwarf_set_de_alloc_flag(0).

    The treatment of DWARF5 differs from DWARF < 5
    as the line table header in DW5 lists the
    compilation directory directly. 10 August 2023.

    _dwarf_pathjoinl() takes care of / and Windows \
*/
static int
create_fullest_file_path(Dwarf_Debug dbg,
    Dwarf_File_Entry fe,
    Dwarf_Line_Context line_context,
    char ** name_ptr_out,
    Dwarf_Error *error)
{
    Dwarf_Unsigned dirno = 0;
    char *full_name = 0;
    char *file_name = 0;
    /*  Large enough that almost never will any malloc
        be needed by dwarfstring.  Arbitrary size. */
    dwarfstring targ;
    unsigned linetab_version = line_context->lc_version_number;

    file_name = (char *) fe->fi_file_name;
    if (!file_name) {
        _dwarf_error(dbg, error, DW_DLE_NO_FILE_NAME);
        return DW_DLV_ERROR;
    }
    if (_dwarf_file_name_is_full_path((Dwarf_Small *)file_name)) {
        int res = 0;

        res = ret_simple_full_path(dbg,
            file_name,
            name_ptr_out,
            error);
        return res;
    }
    {
        int need_dir = FALSE;
        unsigned include_dir_offset = 1;
        static char compdirbuf[300];
        static char filenamebuf[300];
        dwarfstring compdir;
        dwarfstring incdir;
        dwarfstring filename;

        dwarfstring_constructor_static(&targ,
            targbuf,sizeof(targbuf));
        dwarfstring_constructor_static(&compdir,
            compdirbuf,sizeof(compdirbuf));
        dwarfstring_constructor_fixed(&incdir,300);
        dwarfstring_constructor_static(&filename,
            filenamebuf,sizeof(filenamebuf));
        if (line_context->lc_compilation_directory) {
            char * comp_dir_name =
                (char *)line_context->lc_compilation_directory;
            dwarfstring_append(&compdir,comp_dir_name);
        }
        need_dir = FALSE;
        dirno = fe->fi_dir_index;
        include_dir_offset = 0;
        /*  Remember that 0xf006 is the version of
            the experimental line table */
        if (linetab_version == DW_LINE_VERSION5) {
            /* DWARF5 */
            need_dir = TRUE;
            include_dir_offset = 0;
        } else {
            /* EXPERIMENTAL_LINE_TABLES_VERSION or 2,3, or 4 */
            if (dirno) {
                need_dir = TRUE;
                include_dir_offset = 1;
            }/* else, no dirno, need_dir = FALSE
                Take directory from DW_AT_comp_dir */
        }

        if (dirno > line_context->lc_include_directories_count) {
            /*  This is quite corrupted. */
            dwarfstring_destructor(&targ);
            dwarfstring_destructor(&compdir);
            dwarfstring_destructor(&filename);
            dwarfstring_reset(&incdir);
            dwarfstring_append_printf_u(&incdir,
                "DW_DLE_INCL_DIR_NUM_BAD: "
                "corrupt include directory index %u"
                " unusable,", dirno);
            dwarfstring_append_printf_u(&incdir,
                " only %u directories present.",
                line_context->lc_include_directories_count);
            _dwarf_error_string(dbg, error, DW_DLE_INCL_DIR_NUM_BAD,
                dwarfstring_string(&incdir));
            dwarfstring_destructor(&incdir);
            return DW_DLV_ERROR;
        }
        if (need_dir ) {
            _dwarf_dirno_string(line_context,dirno,
                include_dir_offset,&incdir);
        }
        dwarfstring_append(&filename,file_name);
        if (dwarfstring_strlen(&incdir) > 0 &&
            _dwarf_file_name_is_full_path(
            (Dwarf_Small*)dwarfstring_string(&incdir))) {

            /* incdir is full path,Ignore DW_AT_comp_dir
                and (for DWARF5 include_dir[0]) */
            _dwarf_pathjoinl(&targ,&incdir);
            _dwarf_pathjoinl(&targ,&filename);
        } else {
            /* Join two or all three strings,
                ignoring empty/irrelevant ones. */
            /*  Remember that 0xf006 is the version of
                the experimental line table */
            if (linetab_version != DW_LINE_VERSION5) {
                if (dwarfstring_strlen(&compdir) > 0) {
                    _dwarf_pathjoinl(&targ,&compdir);
                }
            } else if (!include_dir_offset && dirno)  {
                /*  Don't do this if DW5 and dirno
                    was zero, doing 0 here will
                    duplicate the comp dir */
                dwarfstring_reset(&compdir);
                _dwarf_dirno_string(line_context,0,
                    include_dir_offset,&compdir);
                if (dwarfstring_strlen(&compdir) > 0) {
                    _dwarf_pathjoinl(&targ,&compdir);
                }
            }
            if (dwarfstring_strlen(&incdir) > 0) {
                _dwarf_pathjoinl(&targ,&incdir);
            }
            _dwarf_pathjoinl(&targ,&filename);
        }
        {
            char *mname = dwarfstring_string(&targ);
            size_t mlen = dwarfstring_strlen(&targ)+1;
            full_name = (char *) _dwarf_get_alloc(dbg, DW_DLA_STRING,
                (Dwarf_Unsigned)mlen);
            if (!full_name) {
                dwarfstring_destructor(&targ);
                dwarfstring_destructor(&incdir);
                dwarfstring_destructor(&compdir);
                dwarfstring_destructor(&filename);
                _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
                return DW_DLV_ERROR;
            }
            _dwarf_safe_strcpy(full_name,mlen,mname,mlen-1);
        }
        *name_ptr_out = full_name;
        dwarfstring_destructor(&targ);
        dwarfstring_destructor(&incdir);
        dwarfstring_destructor(&compdir);
        dwarfstring_destructor(&filename);
    }
    return DW_DLV_OK;
}

static void
report_bogus_stmt_list_form(Dwarf_Debug dbg,
    Dwarf_Half attrform, Dwarf_Error *error)
{
    dwarfstring m; /* OK constructor_fixed  */
    dwarfstring f; /* Ok constructor_static */
    char buf[32];
    const char *formname = 0;

    buf[0] = 0;
    dwarfstring_constructor_static(&f,buf,sizeof(buf));
    dwarf_get_FORM_name(attrform,&formname);
    if (!formname) {
        dwarfstring_append_printf_u(&f,"Invalid Form Code "
            " 0x" DW_PR_DUx,attrform);
    } else {
        dwarfstring_append(&f,(char *)formname);
    }
    dwarfstring_constructor_fixed(&m,200);
    dwarfstring_append_printf_s(&m,
        "DW_DLE_LINE_OFFSET_WRONG_FORM: form %s "
        "instead of an allowed section offset form.",
        dwarfstring_string(&f));
    _dwarf_error_string(dbg, error, DW_DLE_LINE_OFFSET_WRONG_FORM,
        dwarfstring_string(&m));
    dwarfstring_destructor(&m);
    dwarfstring_destructor(&f);
}

/*  Although source files is supposed to return the
    source files in the compilation-unit, it does
    not look for any in the statement program.  In
    other words, it ignores those defined using the
    extended opcode DW_LNE_define_file.
    We do not know of a producer that uses DW_LNE_define_file.

    In DWARF2,3,4 the array of sourcefiles is represented
    differently than DWARF5.
    DWARF 2,3,4, and experimental line table:
        Subtract 1 from the DW_AT_decl_file etc
        to index into the array of names.
        zero means there is no file.
    DWARF 5:
        DW_AT_decl_file etc numbers should be directly
        used to index into the array of names.
        Do not subtract anything.
    For further information
    see the discussion of dwarf_srcfiles() in
    libdwarf2.1.pdf version 3.16 and later, Section
    6.14 around page 117.
*/
int
dwarf_srcfiles(Dwarf_Die die,
    char ***srcfiles,
    Dwarf_Signed * srcfilecount,
    Dwarf_Error * error)
{
    /*  This pointer is used to scan the portion of the .debug_line
        section for the current cu. */
    Dwarf_Small *line_ptr = 0;

    /*  Pointer to a DW_AT_stmt_list attribute
        in case it exists in the die. */
    Dwarf_Attribute stmt_list_attr = 0;

    const char * const_comp_name = 0;
    /*  Pointer to name of compilation directory. */
    const char * const_comp_dir = 0;
    Dwarf_Small *comp_dir = 0;

    /*  Offset into .debug_line specified by a DW_AT_stmt_list
        attribute. */
    Dwarf_Unsigned line_offset = 0;

    /*  This points to a block of char *'s, each of which points to a
        file name. */
    char **ret_files = 0;

    /*  The Dwarf_Debug this die belongs to. */
    Dwarf_Debug dbg = 0;
    Dwarf_CU_Context context = 0;
    Dwarf_Line_Context  line_context = 0;

    /*  Used to chain the file names. */
    Dwarf_Chain curr_chain = NULL;
    Dwarf_Chain head_chain = NULL;
    Dwarf_Chain * plast = &head_chain;

    Dwarf_Half attrform = 0;
    int resattr = DW_DLV_ERROR;
    int lres = DW_DLV_ERROR;
    unsigned i = 0;
    int res = DW_DLV_ERROR;
    Dwarf_Small *section_start = 0;

    /*  ***** BEGIN CODE ***** */
    /*  Reset error. */

    if (error != NULL) {
        *error = NULL;
    }

    CHECK_DIE(die, DW_DLV_ERROR);
    context = die->di_cu_context;
    dbg = context->cc_dbg;

    resattr = dwarf_attr(die, DW_AT_stmt_list,
        &stmt_list_attr, error);
    if (resattr != DW_DLV_OK) {
        return resattr;
    }

    if (dbg->de_debug_line.dss_index == 0) {
        dwarf_dealloc(dbg, stmt_list_attr, DW_DLA_ATTR);
        _dwarf_error(dbg, error, DW_DLE_DEBUG_LINE_NULL);
        return DW_DLV_ERROR;
    }

    res = _dwarf_load_section(dbg, &dbg->de_debug_line,error);
    if (res != DW_DLV_OK) {
        dwarf_dealloc(dbg, stmt_list_attr, DW_DLA_ATTR);
        return res;
    }
    if (!dbg->de_debug_line.dss_size) {
        dwarf_dealloc(dbg, stmt_list_attr, DW_DLA_ATTR);
        return DW_DLV_NO_ENTRY;
    }
    section_start = dbg->de_debug_line.dss_data;

    lres = dwarf_whatform(stmt_list_attr,&attrform,error);
    if (lres != DW_DLV_OK) {
        dwarf_dealloc(dbg, stmt_list_attr, DW_DLA_ATTR);
        return lres;
    }
    if (attrform == DW_FORM_addr) {
        Dwarf_Addr addr = 0;
        /*  DW_AT_producer
            4.2.1 (Based on Apple Inc. build 5658) (LLVM build 2.9)
            generated DW_FORM_addr for DW_AT_stmt_list! */
        lres = dwarf_formaddr(stmt_list_attr,&addr,error);
        if (lres != DW_DLV_OK) {
            if (lres == DW_DLV_ERROR) {
                report_bogus_stmt_list_form(dbg,
                    attrform,error);
                dwarf_dealloc(dbg, stmt_list_attr, DW_DLA_ATTR);
            }
            return lres;
        }
        line_offset = (Dwarf_Unsigned)addr;
    } else if (attrform != DW_FORM_data4 &&
        attrform != DW_FORM_data8 &&
        attrform != DW_FORM_sec_offset  &&
        attrform != DW_FORM_GNU_ref_alt) {
        report_bogus_stmt_list_form(dbg,
            attrform,error);
        dwarf_dealloc(dbg, stmt_list_attr, DW_DLA_ATTR);
        return DW_DLV_ERROR;
    } else {
        /* standard setup. */
        lres = dwarf_global_formref(stmt_list_attr,
            &line_offset, error);
        if (lres != DW_DLV_OK) {
            dwarf_dealloc(dbg, stmt_list_attr, DW_DLA_ATTR);
            return lres;
        }
    }
    if (line_offset >= dbg->de_debug_line.dss_size) {
        dwarf_dealloc(dbg, stmt_list_attr, DW_DLA_ATTR);
        _dwarf_error(dbg, error, DW_DLE_LINE_OFFSET_BAD);
        return DW_DLV_ERROR;
    }
    line_ptr = dbg->de_debug_line.dss_data + line_offset;
    {
        Dwarf_Unsigned fission_offset = 0;
        Dwarf_Unsigned fission_size = 0;
        int resl = _dwarf_get_fission_addition_die(die, DW_SECT_LINE,
            &fission_offset,&fission_size,error);
        if (resl != DW_DLV_OK) {
            dwarf_dealloc(dbg, stmt_list_attr, DW_DLA_ATTR);
            return resl;
        }
        line_ptr += fission_offset;
        if (line_ptr > dbg->de_debug_line.dss_data +
            dbg->de_debug_line.dss_size) {
            dwarf_dealloc(dbg, stmt_list_attr, DW_DLA_ATTR);
            _dwarf_error(dbg, error, DW_DLE_FISSION_ADDITION_ERROR);
            return DW_DLV_ERROR;
        }
    }
    dwarf_dealloc(dbg, stmt_list_attr, DW_DLA_ATTR);
    stmt_list_attr = 0;

    resattr = _dwarf_internal_get_die_comp_dir(die, &const_comp_dir,
        &const_comp_name,error);
    if (resattr == DW_DLV_ERROR) {
        return resattr;
    }

    /* Horrible cast away const to match historical interfaces. */
    comp_dir = (Dwarf_Small *)const_comp_dir;
    line_context = (Dwarf_Line_Context)
        _dwarf_get_alloc(dbg, DW_DLA_LINE_CONTEXT, 1);
    if (line_context == NULL) {
        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
        return DW_DLV_ERROR;
    }
    line_context->lc_new_style_access = FALSE;
    /*  We are in dwarf_srcfiles() */
    {
        Dwarf_Small *line_ptr_out = 0;
        int dres = 0;

        dres = _dwarf_read_line_table_header(dbg,
            context,
            section_start,
            line_ptr,
            dbg->de_debug_line.dss_size,
            &line_ptr_out,
            line_context,
            NULL, NULL,error,
            0);

        if (dres == DW_DLV_ERROR) {
            dwarf_dealloc(dbg, line_context, DW_DLA_LINE_CONTEXT);
            line_context = 0;
            return dres;
        }
        if (dres == DW_DLV_NO_ENTRY) {
            dwarf_dealloc(dbg, line_context, DW_DLA_LINE_CONTEXT);
            line_context = 0;
            return dres;
        }
    }
    /*  For DWARF5, use of DW_AT_comp_dir not needed.
        Line table file names and directories
        start with comp_dir and name. */
    line_context->lc_compilation_directory = comp_dir;
    /* We are in dwarf_srcfiles() */
    {
        Dwarf_File_Entry fe = 0;
        Dwarf_File_Entry fe2 =line_context->lc_file_entries;
        Dwarf_Signed baseindex = 0;
        Dwarf_Signed file_count = 0;
        Dwarf_Signed endindex = 0;
        Dwarf_Signed ifp = 0;

        res =  dwarf_srclines_files_indexes(line_context, &baseindex,
            &file_count, &endindex, error);
        if (res != DW_DLV_OK) {
            return res;
        }
        for (ifp = baseindex; ifp < endindex;
            ++ifp,fe2 = fe->fi_next ) {
            int sres = 0;
            char *name_out = 0;

            fe = fe2;
            sres = create_fullest_file_path(dbg,fe,line_context,
                &name_out,error);
            if (sres != DW_DLV_OK) {
                dwarf_dealloc(dbg, line_context, DW_DLA_LINE_CONTEXT);
                /* This can leak some strings */
                return sres;
            }
            curr_chain =
                (Dwarf_Chain) _dwarf_get_alloc(dbg, DW_DLA_CHAIN, 1);
            if (curr_chain == NULL) {
                dwarf_dealloc(dbg,name_out,DW_DLA_STRING);
                dwarf_dealloc(dbg, line_context, DW_DLA_LINE_CONTEXT);
                _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
                return DW_DLV_ERROR;
            }
            curr_chain->ch_item = name_out;
            (*plast) = curr_chain;
            plast = &(curr_chain->ch_next);
        }
    }
    if (!head_chain) {
        dwarf_dealloc(dbg, line_context, DW_DLA_LINE_CONTEXT);
        *srcfiles = NULL;
        *srcfilecount = 0;
        return DW_DLV_NO_ENTRY;
    }

    /* We are in dwarf_srcfiles() */
    if (line_context->lc_file_entry_count == 0) {
        dwarf_dealloc(dbg, line_context, DW_DLA_LINE_CONTEXT);
        *srcfiles = NULL;
        *srcfilecount = 0;
        return DW_DLV_NO_ENTRY;
    }

    ret_files = (char **)
        _dwarf_get_alloc(dbg, DW_DLA_LIST,
        line_context->lc_file_entry_count);
    if (ret_files == NULL) {
        dwarf_dealloc(dbg, line_context, DW_DLA_LINE_CONTEXT);
        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
        return DW_DLV_ERROR;
    }

    curr_chain = head_chain;
    for (i = 0; i < line_context->lc_file_entry_count; i++) {
        Dwarf_Chain prev = 0;
        *(ret_files + i) = curr_chain->ch_item;
        curr_chain->ch_item = 0;
        prev = curr_chain;
        curr_chain = curr_chain->ch_next;
        dwarf_dealloc(dbg, prev, DW_DLA_CHAIN);
    }
    /*  Our chain is not recorded in the line_context so
        the line_context destructor will not destroy our
        list of strings or our strings.
        Our caller has to do the deallocations.  */
    {
        Dwarf_Signed srccount =
            (Dwarf_Signed)line_context->lc_file_entry_count;
        if (srccount < 0) {
            /*  Impossible corruption! */
            _dwarf_error_string(dbg,error,DW_DLE_LINE_COUNT_WRONG,
                "DW_DLE_LINE_COUNT_WRONG "
                "Call to dwarf_srcfiles finds an impossible "
                "source files count");
            return DW_DLV_ERROR;
        }
        *srcfiles = ret_files;
        *srcfilecount = srccount;
    }
    dwarf_dealloc(dbg, line_context, DW_DLA_LINE_CONTEXT);
    return DW_DLV_OK;
}

/*  Return DW_DLV_OK if ok. else DW_DLV_NO_ENTRY or DW_DLV_ERROR
    doaddrs is true iff this is being called for
    SGI IRIX rqs processing
    (ie, not a normal libdwarf dwarf_srclines or
    two-level  user call at all).
    dolines is true iff this is called by a dwarf_srclines call.

    In case of error or NO_ENTRY in this code we use the
    dwarf_srcline_dealloc(line_context)
    and dealloc of DW_DLA_LINE_CONTEXT
    from the new interface for uniformity here.
*/
int
_dwarf_internal_srclines(Dwarf_Die die,
    Dwarf_Bool is_new_interface,
    Dwarf_Unsigned * version,
    Dwarf_Small    * table_count, /* returns 0,1, or 2 */
    Dwarf_Line_Context *line_context_out,
    Dwarf_Line ** linebuf,
    Dwarf_Signed * linecount,
    Dwarf_Line ** linebuf_actuals,
    Dwarf_Signed * linecount_actuals,
    Dwarf_Bool doaddrs,
    Dwarf_Bool dolines,
    Dwarf_Error * error)
{
    /*  This pointer is used to scan the portion of the .debug_line
        section for the current cu. */
    Dwarf_Small *line_ptr = 0;

    /*  This points to the last byte of the
        .debug_line portion for the current cu. */
    Dwarf_Small *line_ptr_end = 0;

    /*  For two-level line tables, this points to the
        first byte of the
        actuals table (and the end of the logicals table)
        for the current cu. */
    Dwarf_Small *line_ptr_actuals = 0;
    Dwarf_Small *section_start = 0;
    Dwarf_Small *section_end = 0;

    /*  Pointer to a DW_AT_stmt_list attribute in case
        it exists in the die. */
    Dwarf_Attribute stmt_list_attr = 0;

    const char * const_comp_name = 0;
    /*  Pointer to name of compilation directory. */
    const char * const_comp_dir = NULL;
    Dwarf_Small *comp_dir = NULL;

    /*  Offset into .debug_line specified by a DW_AT_stmt_list
        attribute. */
    Dwarf_Unsigned line_offset = 0;

    /*  Pointer to a Dwarf_Line_Context_s structure that contains the
        context such as file names and include directories for the set
        of lines being generated.
        This is always recorded on an
        DW_LNS_end_sequence operator,
        on  all special opcodes, and on DW_LNS_copy.
        */
    Dwarf_Line_Context line_context = 0;
    Dwarf_CU_Context   cu_context = 0;
    Dwarf_Unsigned fission_offset = 0;

    /*  The Dwarf_Debug this die belongs to. */
    Dwarf_Debug dbg = 0;
    int resattr = DW_DLV_ERROR;
    int lres = DW_DLV_ERROR;
    Dwarf_Half address_size = 0;
    Dwarf_Small * orig_line_ptr = 0;

    int res = DW_DLV_ERROR;

    /*  ***** BEGIN CODE ***** */
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

    address_size = (Dwarf_Half)_dwarf_get_address_size(dbg, die);
    resattr = dwarf_attr(die, DW_AT_stmt_list, &stmt_list_attr,
        error);
    if (resattr != DW_DLV_OK) {
        return resattr;
    }
    lres = dwarf_global_formref(stmt_list_attr, &line_offset, error);
    if (lres != DW_DLV_OK) {
        dwarf_dealloc(dbg, stmt_list_attr, DW_DLA_ATTR);
        return lres;
    }

    if (line_offset >= dbg->de_debug_line.dss_size) {
        dwarf_dealloc(dbg, stmt_list_attr, DW_DLA_ATTR);
        _dwarf_error(dbg, error, DW_DLE_LINE_OFFSET_BAD);
        return DW_DLV_ERROR;
    }
    section_start = dbg->de_debug_line.dss_data;
    section_end = section_start  +dbg->de_debug_line.dss_size;
    line_ptr = dbg->de_debug_line.dss_data + line_offset;
    {
        Dwarf_Unsigned fission_size = 0;
        int resf = _dwarf_get_fission_addition_die(die, DW_SECT_LINE,
            &fission_offset,&fission_size,error);
        if (resf != DW_DLV_OK) {
            dwarf_dealloc(dbg, stmt_list_attr, DW_DLA_ATTR);
            return resf;
        }

        /*  fission_offset may be 0, and adding 0 to a null pointer
            is undefined behavior with some compilers. */
        if (fission_offset) {
            line_ptr += fission_offset;
        }
        if (line_ptr > section_end) {
            dwarf_dealloc(dbg, stmt_list_attr, DW_DLA_ATTR);
            _dwarf_error_string(dbg, error,
                DW_DLE_FISSION_ADDITION_ERROR,
                "DW_DLE_FISSION_ADDITION_ERROR: "
                "on  retrieving the fission addition value for "
                "adding that into the line table offset "
                "results in running off "
                "the end of the line table. Corrupt DWARF.");
            return DW_DLV_ERROR;
        }
    }
    section_start = dbg->de_debug_line.dss_data;
    section_end = section_start  +dbg->de_debug_line.dss_size;
    orig_line_ptr = section_start + line_offset + fission_offset;
    line_ptr = orig_line_ptr;
    dwarf_dealloc(dbg, stmt_list_attr, DW_DLA_ATTR);
    if ((line_offset + fission_offset) >
        dbg->de_debug_line.dss_size) {
        _dwarf_error(dbg, error, DW_DLE_LINE_OFFSET_BAD);
        return DW_DLV_ERROR;
    }
    if (line_ptr > section_end) {
        _dwarf_error(dbg, error, DW_DLE_LINE_OFFSET_BAD);
        return DW_DLV_ERROR;
    }

    /*  If die has DW_AT_comp_dir attribute, get the string that names
        the compilation directory. */
    resattr = _dwarf_internal_get_die_comp_dir(die, &const_comp_dir,
        &const_comp_name,error);
    if (resattr == DW_DLV_ERROR) {
        return resattr;
    }
    /* Horrible cast to match historic interfaces. */
    comp_dir = (Dwarf_Small *)const_comp_dir;
    line_context = (Dwarf_Line_Context)
        _dwarf_get_alloc(dbg, DW_DLA_LINE_CONTEXT, 1);
    if (line_context == NULL) {
        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
        return DW_DLV_ERROR;
    }
    line_context->lc_new_style_access =
        (unsigned char)is_new_interface;
    line_context->lc_compilation_directory = comp_dir;
    /*  We are in dwarf_internal_srclines() */
    {
        Dwarf_Small *newlinep = 0;
        int resp = _dwarf_read_line_table_header(dbg,
            cu_context,
            section_start,
            line_ptr,
            dbg->de_debug_line.dss_size,
            &newlinep,
            line_context,
            NULL,NULL,
            error,
            0);

        if (resp == DW_DLV_ERROR) {
            if (is_new_interface) {
                dwarf_srclines_dealloc_b(line_context);
            } else {
                dwarf_dealloc(dbg,line_context,DW_DLA_LINE_CONTEXT);
            }
            return resp;
        }
        if (resp == DW_DLV_NO_ENTRY) {
            if (is_new_interface) {
                dwarf_srclines_dealloc_b(line_context);
            } else {
                dwarf_dealloc(dbg,line_context,DW_DLA_LINE_CONTEXT);
            }
            return resp;
        }
        line_ptr_end = line_context->lc_line_ptr_end;
        line_ptr = newlinep;
        if (line_context->lc_actuals_table_offset > 0) {
            line_ptr_actuals = line_context->lc_line_prologue_start +
                line_context->lc_actuals_table_offset;
        }
    }

    if (line_ptr_actuals == 0) {
        /* ASSERT: lc_table_count == 1 or lc_table_count == 0 */
        int err_count_out = 0;
        /* Normal style (single level) line table. */
        Dwarf_Bool is_actuals_table = FALSE;
        Dwarf_Bool local_is_single_table = TRUE;
        res = read_line_table_program(dbg,
            line_ptr, line_ptr_end, orig_line_ptr,
            section_start,
            line_context,
            address_size, doaddrs, dolines,
            local_is_single_table,
            is_actuals_table,
            error,
            &err_count_out);
        if (res != DW_DLV_OK) {
            if (is_new_interface) {
                dwarf_srclines_dealloc_b(line_context);
            } else {
                dwarf_dealloc(dbg,line_context,DW_DLA_LINE_CONTEXT);
            }
            return res;
        }
        if (linebuf) {
            *linebuf = line_context->lc_linebuf_logicals;
        }
        if (linecount) {
            Dwarf_Signed lcl =
                (Dwarf_Signed)line_context->lc_linecount_logicals;
            if (lcl < 0) {
                _dwarf_error_string(dbg,error,DW_DLE_LINE_COUNT_WRONG,
                    "DW_DLE_LINE_COUNT_WRONG "
                    "Call to dwarf_srclines finds an impossible "
                    "lines count");
                return DW_DLV_ERROR;
            }
            *linecount =  lcl;
        }
        if (linebuf_actuals) {
            *linebuf_actuals = NULL;
        }
        if (linecount_actuals) {
            *linecount_actuals = 0;
        }
    } else {
        Dwarf_Bool is_actuals_table = FALSE;
        Dwarf_Bool local2_is_single_table = FALSE;
        int err_count_out = 0;

        line_context->lc_is_single_table  = FALSE;
        /*  Two-level line table.
            First read the logicals table. */
        res = read_line_table_program(dbg,
            line_ptr, line_ptr_actuals, orig_line_ptr,
            section_start,
            line_context,
            address_size, doaddrs, dolines,
            local2_is_single_table,
            is_actuals_table, error,
            &err_count_out);
        if (res != DW_DLV_OK) {
            if (is_new_interface) {
                dwarf_srclines_dealloc_b(line_context);
            } else {
                dwarf_dealloc(dbg,line_context,DW_DLA_LINE_CONTEXT);
            }
            return res;
        }

        if (linecount) {
            Dwarf_Signed lcl =
                (Dwarf_Signed)line_context->lc_linecount_logicals;
            if (lcl < 0) {
                _dwarf_error_string(dbg,error,DW_DLE_LINE_COUNT_WRONG,
                    "DW_DLE_LINE_COUNT_WRONG "
                    "Call to dwarf_srclines finds an Impossible "
                    "lines count");
                return DW_DLV_ERROR;
            }
            *linecount =  lcl;
        }
        if (linebuf) {
            *linebuf = line_context->lc_linebuf_logicals;
        }
        if (is_new_interface) {
            /* ASSERT: linebuf_actuals == NULL  */
            is_actuals_table = TRUE;
            /* The call requested an actuals table
                and one is present. So now read that one. */
            res = read_line_table_program(dbg,

                line_ptr_actuals, line_ptr_end, orig_line_ptr,
                section_start,
                line_context,
                address_size, doaddrs, dolines,
                local2_is_single_table,
                is_actuals_table, error,
                &err_count_out);
            if (res != DW_DLV_OK) {
                dwarf_srclines_dealloc_b(line_context);
                return res;
            }
            if (linebuf_actuals) {
                *linebuf_actuals = line_context->lc_linebuf_actuals;
            }
            if (linecount_actuals) {
                Dwarf_Signed lca =
                    (Dwarf_Signed)line_context->lc_linecount_actuals;
                if (lca < 0) {
                    _dwarf_error_string(dbg,error,
                        DW_DLE_LINE_COUNT_WRONG,
                        "DW_DLE_LINE_COUNT_WRONG "
                        "Call to dwarf_srclines finds an Impossible "
                        "lines count");
                    return DW_DLV_ERROR;
                }
                *linecount_actuals = lca;
            }
        }
    }
    if (!is_new_interface && linecount &&
        (linecount == 0 ||*linecount == 0) &&
        (linecount_actuals == 0  || *linecount_actuals == 0)) {
        /*  Here we have no actual lines of any kind. In other words,
            it looks like a debugfission
            (now called split dwarf) line table skeleton or
            a caller not prepared for skeletons or two-level reading..
            In that case there are no line entries so the context
            had nowhere to be recorded. Hence we have to delete it
            else we would leak the context.  */
        dwarf_dealloc(dbg, line_context, DW_DLA_LINE_CONTEXT);
        line_context = 0;
        return DW_DLV_OK;
    }
    *table_count = line_context->lc_table_count;
    if (version != NULL) {
        *version = line_context->lc_version_number;
    }
    *line_context_out = line_context;
    return DW_DLV_OK;
}

int
dwarf_get_ranges_section_name(Dwarf_Debug dbg,
    const char **section_name_out,
    Dwarf_Error * error)
{
    struct Dwarf_Section_s *sec = 0;
    if (error != NULL) {
        *error = NULL;
    }
    CHECK_DBG(dbg,error,"dwarf_get_ranges_section_name()");
    sec = &dbg->de_debug_ranges;
    if (sec->dss_size == 0) {
        /* We don't have such a  section at all. */
        return DW_DLV_NO_ENTRY;
    }
    *section_name_out = sec->dss_name;
    return DW_DLV_OK;
}

int
dwarf_get_aranges_section_name(Dwarf_Debug dbg,
    const char **section_name_out,
    Dwarf_Error * error)
{
    struct Dwarf_Section_s *sec = 0;

    CHECK_DBG(dbg,error,"dwarf_get_aranges_section_name()");
    if (error != NULL) {
        *error = NULL;
    }
    sec = &dbg->de_debug_aranges;
    if (sec->dss_size == 0) {
        /* We don't have such a  section at all. */
        return DW_DLV_NO_ENTRY;
    }
    *section_name_out = sec->dss_name;
    return DW_DLV_OK;
}
int
dwarf_get_line_section_name(Dwarf_Debug dbg,
    const char **section_name_out,
    Dwarf_Error * error)
{
    struct Dwarf_Section_s *sec = 0;

    CHECK_DBG(dbg,error,"dwarf_get_line_section_name)");
    if (error != NULL) {
        *error = NULL;
    }
    sec = &dbg->de_debug_line;
    if (sec->dss_size == 0) {
        /* We don't have such a  section at all. */
        return DW_DLV_NO_ENTRY;
    }
    *section_name_out = sec->dss_name;
    return DW_DLV_OK;
}

int
dwarf_get_line_section_name_from_die(Dwarf_Die die,
    const char **section_name_out,
    Dwarf_Error * error)
{
    /*  The Dwarf_Debug this die belongs to. */
    Dwarf_Debug dbg = 0;
    struct Dwarf_Section_s *sec = 0;

    /*  ***** BEGIN CODE ***** */
    if (error) {
        *error = NULL;
    }

    CHECK_DIE(die, DW_DLV_ERROR);
    dbg = die->di_cu_context->cc_dbg;
    sec = &dbg->de_debug_line;
    if (sec->dss_size == 0) {
        /* We don't have such a  section at all. */
        return DW_DLV_NO_ENTRY;
    }
    *section_name_out = sec->dss_name;
    return DW_DLV_OK;
}

int
dwarf_get_string_section_name(Dwarf_Debug dbg,
    const char **section_name_out,
    Dwarf_Error * error)
{
    struct Dwarf_Section_s *sec = 0;

    CHECK_DBG(dbg,error,"dwarf_get_string_section_name()");
    /*  ***** BEGIN CODE ***** */
    if (error != NULL) {
        *error = NULL;
    }
    sec = &dbg->de_debug_str;
    if (sec->dss_size == 0) {
        /* We don't have such a  section at all. */
        return DW_DLV_NO_ENTRY;
    }
    *section_name_out = sec->dss_name;
    return DW_DLV_OK;
}

/* New October 2015. */
int
dwarf_srclines_b(Dwarf_Die die,
    Dwarf_Unsigned  * version_out,
    Dwarf_Small     * table_count,
    Dwarf_Line_Context * line_context,
    Dwarf_Error * error)
{
    Dwarf_Signed linecount_actuals = 0;
    Dwarf_Line *linebuf = 0;
    Dwarf_Line *linebuf_actuals = 0;
    Dwarf_Signed linecount = 0;
    Dwarf_Bool is_new_interface = TRUE;
    int res = 0;
    Dwarf_Unsigned tcount = 0;

    res  = _dwarf_internal_srclines(die,
        is_new_interface,
        version_out,
        table_count,
        line_context,
        &linebuf,
        &linecount,
        &linebuf_actuals,
        &linecount_actuals,
        /* addrlist= */ FALSE,
        /* linelist= */ TRUE,
        error);
    if (res == DW_DLV_OK) {
        (*line_context)->lc_new_style_access = TRUE;
    }
    if (linecount_actuals) {
        tcount++;
    }
    if (linecount) {
        tcount++;
    }
    *table_count = (Dwarf_Small)tcount;
    return res;
}

/* New October 2015. */
int
dwarf_srclines_from_linecontext(Dwarf_Line_Context line_context,
    Dwarf_Line**     linebuf,
    Dwarf_Signed *   linecount,
    Dwarf_Error  *    error)
{
    if (!line_context || line_context->lc_magic != DW_CONTEXT_MAGIC) {
        _dwarf_error(NULL, error, DW_DLE_LINE_CONTEXT_BOTCH);
        return DW_DLV_ERROR;
    }
    if (!line_context->lc_new_style_access) {
        _dwarf_error(line_context->lc_dbg, error,
            DW_DLE_LINE_CONTEXT_BOTCH);
        return DW_DLV_ERROR;
    }
    {
        Dwarf_Signed lc =
            (Dwarf_Signed)line_context->lc_linecount_logicals;
        if (lc < 0) {
            _dwarf_error_string(line_context->lc_dbg,error,
                DW_DLE_LINE_COUNT_WRONG,
                "DW_DLE_LINE_COUNT_WRONG "
                "Call to dwarf_srclines_from_linecontext "
                "finds an Impossible "
                "lines count");
            return DW_DLV_ERROR;
        }
        *linebuf =   line_context->lc_linebuf_logicals;
        *linecount = lc;
    }
    return DW_DLV_OK;
}

/* New October 2015. */
int
dwarf_srclines_two_level_from_linecontext(
    Dwarf_Line_Context line_context,
    Dwarf_Line**     linebuf,
    Dwarf_Signed *   linecount,
    Dwarf_Line**     linebuf_actuals,
    Dwarf_Signed *   linecount_actuals,
    Dwarf_Error  *    error)
{
    if (!line_context || line_context->lc_magic != DW_CONTEXT_MAGIC) {
        _dwarf_error(NULL, error, DW_DLE_LINE_CONTEXT_BOTCH);
        return DW_DLV_ERROR;
    }
    if (!line_context->lc_new_style_access) {
        _dwarf_error(line_context->lc_dbg, error,
            DW_DLE_LINE_CONTEXT_BOTCH);
        return DW_DLV_ERROR;
    }
    {
        Dwarf_Signed lcl =
            (Dwarf_Signed)line_context->lc_linecount_logicals;
        Dwarf_Signed lca =
            (Dwarf_Signed)line_context->lc_linecount_actuals;
        if (lcl < 0) {
            _dwarf_error_string(line_context->lc_dbg,error,
                DW_DLE_LINE_COUNT_WRONG,
                "DW_DLE_LINE_COUNT_WRONG "
                "Call to dwarf_srclines_two_level_from_linecontext "
                "finds an Impossible "
                "lines count");
            return DW_DLV_ERROR;
        }
        if (lca < 0) {
            _dwarf_error_string(line_context->lc_dbg,error,
                DW_DLE_LINE_COUNT_WRONG,
                "DW_DLE_LINE_COUNT_WRONG "
                "Call to dwarf_srclines_two_level_from_linecontext "
                "finds an Impossible "
                "lines count");
            return DW_DLV_ERROR;
        }

        *linebuf =           line_context->lc_linebuf_logicals;
        *linecount =         line_context->lc_linecount_logicals;
        *linebuf_actuals =    line_context->lc_linebuf_actuals;
        *linecount_actuals = line_context->lc_linecount_actuals;
    }
    return DW_DLV_OK;
}

/* New October 2015. */
int
dwarf_srclines_table_offset(Dwarf_Line_Context line_context,
    Dwarf_Unsigned * offset,
    Dwarf_Error  *    error)
{
    if (!line_context ){
        _dwarf_error(NULL, error, DW_DLE_LINE_CONTEXT_BOTCH);
        return DW_DLV_ERROR;
    }
    if ( line_context->lc_magic != DW_CONTEXT_MAGIC) {
        _dwarf_error(NULL, error,
            DW_DLE_LINE_CONTEXT_BOTCH);
        return DW_DLV_ERROR;
    }
    *offset = line_context->lc_section_offset;
    return DW_DLV_OK;
}

/* New October 2015. */
/*  If the CU DIE  has no DW_AT_comp_dir then
    the pointer pushed back to *compilation_directory
    will be NULL.
    For DWARF5 the line table header has the compilation
    directory.  */
int
dwarf_srclines_comp_dir(Dwarf_Line_Context line_context,
    const char **  compilation_directory,
    Dwarf_Error  *  error)
{
    if (!line_context ){
        _dwarf_error(NULL, error, DW_DLE_LINE_CONTEXT_BOTCH);
        return DW_DLV_ERROR;
    }
    if (line_context->lc_magic != DW_CONTEXT_MAGIC) {
        _dwarf_error(NULL, error,
            DW_DLE_LINE_CONTEXT_BOTCH);
        return DW_DLV_ERROR;
    }
    *compilation_directory =
        (const char *)line_context->lc_compilation_directory;
    return DW_DLV_OK;
}

/* New October 2015. */
int
dwarf_srclines_subprog_count(Dwarf_Line_Context line_context,
    Dwarf_Signed * count_out,
    Dwarf_Error * error)
{
    if (!line_context ){
        _dwarf_error(NULL, error, DW_DLE_LINE_CONTEXT_BOTCH);
        return DW_DLV_ERROR;
    }
    if (line_context->lc_magic != DW_CONTEXT_MAGIC) {
        _dwarf_error(NULL, error, DW_DLE_LINE_CONTEXT_BOTCH);
        return DW_DLV_ERROR;
    }
    if (count_out) {
        Dwarf_Signed co =
            (Dwarf_Signed) line_context->lc_subprogs_count;
        if (co < 0) {
            _dwarf_error_string(line_context->lc_dbg,error,
                DW_DLE_LINE_COUNT_WRONG,
                "DW_DLE_LINE_COUNT_WRONG "
                "Call to dwarf_srclines_subprog_count "
                "finds an Impossible "
                "subprogs count");
            return DW_DLV_ERROR;
        }
        *count_out = co;
    }
    return DW_DLV_OK;
}
/* New October 2015. */
/*  Index says which to return.  Valid indexes are
    1-lc_subprogs_count
    */
int
dwarf_srclines_subprog_data(Dwarf_Line_Context line_context,
    Dwarf_Signed index_in,
    const char ** name,
    Dwarf_Unsigned *decl_file,
    Dwarf_Unsigned *decl_line,
    Dwarf_Error *error)
{
    Dwarf_Unsigned index = 0;
    Dwarf_Subprog_Entry sub = 0;
    /*  Negative values not sensible. Leaving traditional
        signed interfaces. */
    if (index_in < 0) {
        _dwarf_error_string(line_context->lc_dbg,error,
            DW_DLE_LINE_INDEX_WRONG,
            "DW_DLE_LINE_INDEX_WRONG "
            "Call to dwarf_srclines_subprog_data "
            "finds an Impossible "
            "index argument value");
        return DW_DLV_ERROR;
    }
    index = (Dwarf_Unsigned)index_in;
    if (!line_context || line_context->lc_magic != DW_CONTEXT_MAGIC) {
        _dwarf_error(NULL, error, DW_DLE_LINE_CONTEXT_BOTCH);
        return DW_DLV_ERROR;
    }
    if (index < 1 || index > line_context->lc_subprogs_count) {
        _dwarf_error(line_context->lc_dbg, error,
            DW_DLE_LINE_CONTEXT_INDEX_WRONG);
        return DW_DLV_ERROR;
    }
    sub = line_context->lc_subprogs + (index-1);
    *name = (const char *)sub->ds_subprog_name;
    *decl_file = sub->ds_decl_file;
    *decl_line = sub->ds_decl_line;
    return DW_DLV_OK;
}

/* New March 2018 making iteration through file names. */
int
dwarf_srclines_files_indexes(Dwarf_Line_Context line_context,
    Dwarf_Signed   *baseindex,
    Dwarf_Signed   *file_count,
    Dwarf_Signed   *endindex,
    Dwarf_Error    * error)
{
    if (line_context->lc_magic != DW_CONTEXT_MAGIC) {
        _dwarf_error(NULL, error, DW_DLE_LINE_CONTEXT_BOTCH);
        return DW_DLV_ERROR;
    }
    {
        Dwarf_Signed bi =
            (Dwarf_Signed)line_context->lc_file_entry_baseindex;
        Dwarf_Signed fc =
            (Dwarf_Signed)line_context->lc_file_entry_count;
        Dwarf_Signed ei =
            (Dwarf_Signed)line_context->lc_file_entry_endindex;
        if (bi < 0) {
            _dwarf_error_string(line_context->lc_dbg,error,
                DW_DLE_LINE_INDEX_WRONG,
                "DW_DLE_LINE_INDEX_WRONG "
                "Call to dwarf_srclines_subprog_data "
                "finds an Impossible "
                "file entry index value");
            return DW_DLV_ERROR;
        }
        if (fc < 0) {
            _dwarf_error_string(line_context->lc_dbg,error,
                DW_DLE_LINE_COUNT_WRONG,
                "DW_DLE_LINE_COUNT_WRONG "
                "Call to dwarf_srclines_subprog_data "
                "finds an Impossible "
                "file count index value");
            return DW_DLV_ERROR;
        }
        if (ei < 0) {
            _dwarf_error_string(line_context->lc_dbg,error,
                DW_DLE_LINE_INDEX_WRONG,
                "DW_DLE_LINE_INDEX_WRONG "
                "Call to dwarf_srclines_subprog_data "
                "finds an Impossible "
                "endindex value");
            return DW_DLV_ERROR;
        }
        *baseindex  = bi;
        *file_count = fc;
        *endindex   = ei;
    }
    return DW_DLV_OK;
}

int
dwarf_srclines_files_data_b(Dwarf_Line_Context line_context,
    Dwarf_Signed     index_in,
    const char **    name,
    Dwarf_Unsigned * directory_index,
    Dwarf_Unsigned * last_mod_time,
    Dwarf_Unsigned * file_length,
    Dwarf_Form_Data16 ** data16ptr,
    Dwarf_Error    * error)
{
    Dwarf_File_Entry fi = 0;
    Dwarf_Signed i  =0;
    Dwarf_Signed baseindex = 0;
    Dwarf_Signed file_count = 0;
    Dwarf_Signed endindex = 0;
    /*  Negative values not sensible. Leaving traditional
        signed interfaces. */
    Dwarf_Signed index = index_in;
    int res = 0;

    if (index_in < 0) {
        _dwarf_error_string(line_context->lc_dbg,error,
            DW_DLE_LINE_INDEX_WRONG,
            "DW_DLE_LINE_INDEX_WRONG "
            "Call to dwarf_srclines_files_data_b "
            "passes an Impossible "
            "index argument value");
        return DW_DLV_ERROR;
    }
    if (!line_context || line_context->lc_magic != DW_CONTEXT_MAGIC) {
        _dwarf_error(NULL, error, DW_DLE_LINE_CONTEXT_BOTCH);
        return DW_DLV_ERROR;
    }

    /*  Special accommodation of the special gnu experimental
        version number (a high number) so we cannot just
        say '5 or greater'. This is awkward, but at least
        if there is a version 6 or later it still allows
        the experimental table.  */
    res =  dwarf_srclines_files_indexes(line_context, &baseindex,
        &file_count, &endindex, error);
    if (res != DW_DLV_OK) {
        return res;
    }
    fi = line_context->lc_file_entries;
    if (baseindex < 0) {
        _dwarf_error_string(line_context->lc_dbg,error,
            DW_DLE_LINE_INDEX_WRONG,
            "DW_DLE_LINE_INDEX_WRONG "
            "Call to dwarf_srclines_file_data_b "
            "finds an Impossible "
            "file entry index value");
        return DW_DLV_ERROR;
    }
    if (file_count < 0) {
        _dwarf_error_string(line_context->lc_dbg,error,
            DW_DLE_LINE_COUNT_WRONG,
            "DW_DLE_LINE_COUNT_WRONG "
            "Call to dwarf_srclines_file_data_b "
            "finds an Impossible "
            "file count value");
        return DW_DLV_ERROR;
    }
    if (endindex < 0) {
        _dwarf_error_string(line_context->lc_dbg,error,
            DW_DLE_LINE_INDEX_WRONG,
            "DW_DLE_LINE_INDEX_WRONG "
            "Call to dwarf_srclines_file_data_b "
            "finds an Impossible "
            "endindex value");
        return DW_DLV_ERROR;
    }

    if (index < baseindex || index >= endindex) {
        _dwarf_error(line_context->lc_dbg, error,
            DW_DLE_LINE_CONTEXT_INDEX_WRONG);
            return DW_DLV_ERROR;
    }
    for ( i = baseindex;i < index; i++) {
        fi = fi->fi_next;
        if (!fi) {
            _dwarf_error(line_context->lc_dbg, error,
                DW_DLE_LINE_HEADER_CORRUPT);
            return DW_DLV_ERROR;
        }
    }

    if (name) {
        *name = (const char *)fi->fi_file_name;
    }
    if (directory_index) {
        *directory_index = fi->fi_dir_index;
    }
    if (last_mod_time) {
        *last_mod_time = fi->fi_time_last_mod;
    }
    if (file_length) {
        *file_length = fi->fi_file_length;
    }
    if (data16ptr) {
        if (fi->fi_md5_present) {
            *data16ptr = &fi->fi_md5_value;
        } else {
            *data16ptr = 0;
        }
    }
    return DW_DLV_OK;
}

/* New October 2015. */
int
dwarf_srclines_include_dir_count(Dwarf_Line_Context line_context,
    Dwarf_Signed * count,
    Dwarf_Error  * error)
{
    Dwarf_Signed scount = 0;
    if (!line_context || line_context->lc_magic != DW_CONTEXT_MAGIC) {
        _dwarf_error(NULL, error, DW_DLE_LINE_CONTEXT_BOTCH);
        return DW_DLV_ERROR;
    }
    scount = (Dwarf_Signed)line_context->lc_include_directories_count;
    if (scount < 0) {
        _dwarf_error_string(line_context->lc_dbg,error,
            DW_DLE_LINE_COUNT_WRONG,
            "DW_DLE_LINE_COUNT_WRONG "
            "Call to dwarf_srclines_include_dir_count "
            "finds an Impossible "
            "include directories count");
        return DW_DLV_ERROR;
    }
    *count = scount;
    return DW_DLV_OK;
}

/* New October 2015. */
int
dwarf_srclines_include_dir_data(Dwarf_Line_Context line_context,
    Dwarf_Signed   index_in,
    const char  ** name,
    Dwarf_Error *  error)
{
    /*  It never made sense that the srclines used a signed count.
        But that cannot be fixed in interfaces for compatibility.
        So we adjust here. */
    Dwarf_Unsigned index = (Dwarf_Unsigned)index_in;
    unsigned int version = 0;

    if (index_in < 0) {
        _dwarf_error_string(line_context->lc_dbg,error,
            DW_DLE_LINE_INDEX_WRONG,
            "DW_DLE_LINE_INDEX_WRONG "
            "Call to dwarf_srclines_include_dir_data "
            "finds an Impossible "
            "include directories count");
        return DW_DLV_ERROR;
    }
    if (!line_context || line_context->lc_magic != DW_CONTEXT_MAGIC) {
        _dwarf_error(NULL, error, DW_DLE_LINE_CONTEXT_BOTCH);
        return DW_DLV_ERROR;
    }
    version = line_context->lc_version_number;
    /*  Remember that 0xf006 is the version of
        the experimental line table */
    if (version == DW_LINE_VERSION5) {
        if (index >= line_context->lc_include_directories_count) {
            _dwarf_error(line_context->lc_dbg, error,
                DW_DLE_LINE_CONTEXT_INDEX_WRONG);
            return DW_DLV_ERROR;
        }
        *name = (const char *)
            (line_context->lc_include_directories[index]);
    } else {
        if (index < 1 ||
            index > line_context->lc_include_directories_count) {
            _dwarf_error(line_context->lc_dbg, error,
                DW_DLE_LINE_CONTEXT_INDEX_WRONG);
            return DW_DLV_ERROR;
        }
        *name = (const char *)
            (line_context->lc_include_directories[index-1]);
    }
    return DW_DLV_OK;
}

/* New October 2015. */
int
dwarf_srclines_version(Dwarf_Line_Context line_context,
    Dwarf_Unsigned *version_out,
    Dwarf_Small    *table_count_out,
    Dwarf_Error *error)
{
    if (!line_context || line_context->lc_magic != DW_CONTEXT_MAGIC) {
        _dwarf_error(NULL, error, DW_DLE_LINE_CONTEXT_BOTCH);
        return DW_DLV_ERROR;
    }
    *version_out = line_context->lc_version_number;
    *table_count_out = line_context->lc_table_count;
    return DW_DLV_OK;
}

/*  Every line table entry (except DW_DLE_end_sequence,
    which is returned using dwarf_lineendsequence())
    potentially has the begin-statement
    flag marked 'on'.   This returns thru *return_bool,
    the begin-statement flag.  */

int
dwarf_linebeginstatement(Dwarf_Line line,
    Dwarf_Bool * return_bool, Dwarf_Error * error)
{
    if (line == NULL || return_bool == 0) {
        _dwarf_error(NULL, error, DW_DLE_DWARF_LINE_NULL);
        return DW_DLV_ERROR;
    }

    *return_bool = (line->li_l_data.li_is_stmt);
    return DW_DLV_OK;
}

/*  At the end of any contiguous line-table there may be
    a DW_LNE_end_sequence operator.
    This returns non-zero thru *return_bool
    if and only if this 'line' entry was a DW_LNE_end_sequence.

    Within a compilation unit or function there may be multiple
    line tables, each ending with a DW_LNE_end_sequence.
    Each table describes a contiguous region.
    Because compilers may split function code up in arbitrary ways
    compilers may need to emit multiple contigous regions (ie
    line tables) for a single function.
    See the DWARF3 spec section 6.2.  */
int
dwarf_lineendsequence(Dwarf_Line line,
    Dwarf_Bool * return_bool,
    Dwarf_Error * error)
{
    if (line == NULL) {
        _dwarf_error(NULL, error, DW_DLE_DWARF_LINE_NULL);
        return DW_DLV_ERROR;
    }

    *return_bool = (line->li_l_data.li_end_sequence);
    return DW_DLV_OK;
}

/*  Each 'line' entry has a line-number.
    If the entry is a DW_LNE_end_sequence the line-number is
    meaningless (see dwarf_lineendsequence(), just above).  */
int
dwarf_lineno(Dwarf_Line line,
    Dwarf_Unsigned * ret_lineno, Dwarf_Error * error)
{
    if (line == NULL || ret_lineno == 0) {
        _dwarf_error(NULL, error, DW_DLE_DWARF_LINE_NULL);
        return DW_DLV_ERROR;
    }

    *ret_lineno = (line->li_l_data.li_line);
    return DW_DLV_OK;
}

/*  Each 'line' entry has a file-number, an index
    into the file table.
    If the entry is a DW_LNE_end_sequence the index is
    meaningless (see dwarf_lineendsequence(), just above).
    The file number returned is an index into the file table
    produced by dwarf_srcfiles(), but care is required: the
    li_file begins with 1 for DWARF2,3,4
    files, so that the li_file returned here
    is 1 greater than its index into the dwarf_srcfiles()
    output array.

    And entries from DW_LNE_define_file don't appear in
    the dwarf_srcfiles() output so file indexes from here may exceed
    the size of the dwarf_srcfiles() output array size.
*/
int
dwarf_line_srcfileno(Dwarf_Line line,
    Dwarf_Unsigned * ret_fileno, Dwarf_Error * error)
{
    if (line == NULL || ret_fileno == 0) {
        _dwarf_error(NULL, error, DW_DLE_DWARF_LINE_NULL);
        return DW_DLV_ERROR;
    }
    /*  li_file must be <= line->li_context->lc_file_entry_count else
        it is trash. li_file 0 means not attributable to
        any source file per dwarf2/3 spec.
        For DWARF5, li_file < lc_file_entry_count */
    *ret_fileno = (line->li_l_data.li_file);
    return DW_DLV_OK;
}

/*  Each 'line' entry has an is_addr_set attribute.
    If the entry is a DW_LNE_set_address, return TRUE through
    the *is_addr_set pointer.  */
int
dwarf_line_is_addr_set(Dwarf_Line line,
    Dwarf_Bool *is_addr_set, Dwarf_Error * error)
{
    if (line == NULL) {
        _dwarf_error(NULL, error, DW_DLE_DWARF_LINE_NULL);
        return DW_DLV_ERROR;
    }

    *is_addr_set = (line->li_l_data.li_is_addr_set);
    return DW_DLV_OK;
}

/*  Each 'line' entry has a line-address.
    If the entry is a DW_LNE_end_sequence the adddress
    is one-beyond the last address this contigous region
    covers, so the address is not inside the region,
    but is just outside it.  */
int
dwarf_lineaddr(Dwarf_Line line,
    Dwarf_Addr * ret_lineaddr, Dwarf_Error * error)
{
    if (line == NULL || ret_lineaddr == 0) {
        _dwarf_error(NULL, error, DW_DLE_DWARF_LINE_NULL);
        return DW_DLV_ERROR;
    }

    *ret_lineaddr = (line->li_address);
    return DW_DLV_OK;
}

/*  Each 'line' entry has a column-within-line (offset
    within the line) where the
    source text begins.
    If the entry is a DW_LNE_end_sequence the line-number is
    meaningless (see dwarf_lineendsequence(), just above).
    Lines of text begin at column 1.  The value 0
    means the line begins at the left edge of the line.
    (See the DWARF3 spec, section 6.2.2).
    So 0 and 1 mean essentially the same thing.
    dwarf_lineoff_b() is new in December 2011.
    */
int
dwarf_lineoff_b(Dwarf_Line line,
    Dwarf_Unsigned * ret_lineoff, Dwarf_Error * error)
{
    if (line == NULL || ret_lineoff == 0) {
        _dwarf_error(NULL, error, DW_DLE_DWARF_LINE_NULL);
        return DW_DLV_ERROR;
    }

    *ret_lineoff = line->li_l_data.li_column;
    return DW_DLV_OK;
}

static int
_dwarf_filename(Dwarf_Line_Context context,
    Dwarf_Unsigned fileno_in,
    char **ret_filename,
    const char *callername,
    Dwarf_Error *error)
{
    Dwarf_Signed i = 0;
    Dwarf_File_Entry file_entry = 0;
    Dwarf_Debug dbg = context->lc_dbg;
    int res = 0;
    Dwarf_Signed baseindex = 0;
    Dwarf_Signed file_count = 0;
    Dwarf_Signed endindex = 0;
    /*  Negative values not sensible. Leaving traditional
        signed interfaces in place. */
    Dwarf_Signed fileno = (Dwarf_Signed)fileno_in;
    unsigned linetab_version = context->lc_version_number;

    if (fileno < 0) {
        dwarfstring m;
        dwarfstring_constructor(&m);
        dwarfstring_append_printf_s(&m,
            "DW_DLE_LINE_COUNT_WRONG "
            "Call to %s "
            "finds an Impossible "
            "file number ",
            (char *)callername);
        _dwarf_error_string(dbg,error,
            DW_DLE_LINE_COUNT_WRONG,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
#if 0 /* erroneous correctness check. Ignore. */
    if (fileno_in >= context->lc_file_entry_count) {
        _dwarf_error_string(dbg,error, DW_DLE_NO_FILE_NAME,
            "DW_DLE_NO_FILE_NAME "
            "A file number is too larg. Corrupt dwarf");
        return DW_DLV_ERROR;
    }
#endif
    res =  dwarf_srclines_files_indexes(context, &baseindex,
        &file_count, &endindex, error);
    if (res != DW_DLV_OK) {
        return res;
    }
    {
        Dwarf_Signed bi =
            (Dwarf_Signed)context->lc_file_entry_baseindex;
        Dwarf_Signed fc =
            (Dwarf_Signed)context->lc_file_entry_count;
        Dwarf_Signed ei =
            (Dwarf_Signed)context->lc_file_entry_endindex;
        if (bi < 0) {
            dwarfstring m;
            dwarfstring_constructor(&m);
            dwarfstring_append_printf_s(&m,
                "DW_DLE_LINE_INDEX_WRONG "
                "Call to %s "
                "finds an Impossible "
                "base index ",
                (char *)callername);
            _dwarf_error_string(dbg,error,
                DW_DLE_LINE_COUNT_WRONG,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
        }
        if (fc < 0) {
            dwarfstring m;
            dwarfstring_constructor(&m);
            dwarfstring_append_printf_s(&m,
                "DW_DLE_LINE_COUNT_WRONG "
                "Call to %s "
                "finds an Impossible "
                "file entry count ",
                (char *)callername);
            _dwarf_error_string(dbg,error,
                DW_DLE_LINE_COUNT_WRONG,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
        }
        if (ei < 0) {
            dwarfstring m;
            dwarfstring_constructor(&m);
            dwarfstring_append_printf_s(&m,
                "DW_DLE_LINE_INDEX_WRONG "
                "Call to %s "
                "finds an Impossible "
                "end index ",
                (char *)callername);
            _dwarf_error_string(dbg,error,
                DW_DLE_LINE_COUNT_WRONG,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
        }
    }

    /*  FIXME: what about comparing to file_count? */
    if (fileno >= endindex) {
        dwarfstring m; /* ok constructor_fixed */

        dwarfstring_constructor_fixed(&m, 200);
        dwarfstring_append_printf_i(&m,
            "DW_DLE_NO_FILE_NAME: the file number is %d ",
            fileno);
        dwarfstring_append_printf_u(&m,
            "( this is a DWARF 0x%x linetable)",
            linetab_version);
        dwarfstring_append_printf_i(&m,
            " yet the highest allowed file name index is %d.",
            endindex-1);
        _dwarf_error_string(dbg, error, DW_DLE_NO_FILE_NAME,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    } else {
        if (linetab_version <= DW_LINE_VERSION4 ||
            linetab_version == EXPERIMENTAL_LINE_TABLES_VERSION) {
            if (!fileno) {
                return DW_DLV_NO_ENTRY;
            } else {
                /* else ok */
            }
        }  else {
            /*  Remember that 0xf006 is the version of
                the experimental line table */
            /* DW_LINE_VERSION5 so file index 0 is fine */
        }
    }

    file_entry = context->lc_file_entries;
    /*  zero fileno allowed for DWARF5 table. For DWARF4,
        zero fileno handled above. */
    for (i =  baseindex; i < fileno ; i++) {
        file_entry = file_entry->fi_next;
    }

    res = create_fullest_file_path(dbg,
        file_entry,context, ret_filename,error);
    return res;
}

int
dwarf_linesrc(Dwarf_Line line, char **ret_linesrc,
    Dwarf_Error * error)
{
    if (line == NULL) {
        _dwarf_error(NULL, error, DW_DLE_DWARF_LINE_NULL);
        return DW_DLV_ERROR;
    }
    if (line->li_context == NULL) {
        _dwarf_error(NULL, error, DW_DLE_LINE_CONTEXT_NULL);
        return DW_DLV_ERROR;
    }
    return _dwarf_filename(line->li_context,
        line->li_l_data.li_file, ret_linesrc,
        "dwarf_linesrc",error);
}

/*  Every line table entry potentially has the basic-block-start
    flag marked 'on'.   This returns thru *return_bool,
    the basic-block-start flag.
*/
int
dwarf_lineblock(Dwarf_Line line,
    Dwarf_Bool * return_bool, Dwarf_Error * error)
{
    if (line == NULL) {
        _dwarf_error(NULL, error, DW_DLE_DWARF_LINE_NULL);
        return DW_DLV_ERROR;
    }
    *return_bool = (line->li_l_data.li_basic_block);
    return DW_DLV_OK;
}

/*  We gather these into one call as it's likely one
    will want all or none of them.  */
int
dwarf_prologue_end_etc(Dwarf_Line  line,
    Dwarf_Bool  *    prologue_end,
    Dwarf_Bool  *    epilogue_begin,
    Dwarf_Unsigned * isa,
    Dwarf_Unsigned * discriminator,
    Dwarf_Error *    error)
{
    if (line == NULL) {
        _dwarf_error(NULL, error, DW_DLE_DWARF_LINE_NULL);
        return DW_DLV_ERROR;
    }
    *prologue_end = line->li_l_data.li_prologue_end;
    *epilogue_begin =
        line->li_l_data.li_epilogue_begin;
    *isa = line->li_l_data.li_isa;
    *discriminator = line->li_l_data.li_discriminator;
    return DW_DLV_OK;
}

int
dwarf_linelogical(Dwarf_Line line,
    Dwarf_Unsigned * logical,
    Dwarf_Error*     error)
{
    if (line == NULL) {
        _dwarf_error(NULL, error, DW_DLE_DWARF_LINE_NULL);
        return DW_DLV_ERROR;
    }
    *logical = line->li_l_data.li_line;
    return DW_DLV_OK;
}

int
dwarf_linecontext(Dwarf_Line line,
    Dwarf_Unsigned * context,
    Dwarf_Error*     error)
{
    if (line == NULL) {
        _dwarf_error(NULL, error, DW_DLE_DWARF_LINE_NULL);
        return DW_DLV_ERROR;
    }
    *context = (line->li_l_data.li_call_context);
    return DW_DLV_OK;
}

int
dwarf_line_subprogno(Dwarf_Line line,
    Dwarf_Unsigned * subprog,
    Dwarf_Error *    error)
{
    if (line == NULL) {
        _dwarf_error(NULL, error, DW_DLE_DWARF_LINE_NULL);
        return DW_DLV_ERROR;
    }
    *subprog = (line->li_l_data.li_subprogram);
    return DW_DLV_OK;
}

int
dwarf_line_subprog(Dwarf_Line line,
    char   **        subprog_name,
    char   **        decl_filename,
    Dwarf_Unsigned * decl_line,
    Dwarf_Error *    error)
{
    Dwarf_Unsigned subprog_no;
    Dwarf_Subprog_Entry subprog;
    Dwarf_Debug dbg;
    int res;

    if (line == NULL) {
        _dwarf_error(NULL, error, DW_DLE_DWARF_LINE_NULL);
        return DW_DLV_ERROR;
    }
    if (line->li_context == NULL) {
        _dwarf_error(NULL, error, DW_DLE_LINE_CONTEXT_NULL);
        return DW_DLV_ERROR;
    }
    dbg = line->li_context->lc_dbg;
    subprog_no = line->li_l_data.li_subprogram;
    if (subprog_no == 0) {
        *subprog_name = NULL;
        *decl_filename = NULL;
        *decl_line = 0;
        return DW_DLV_OK;
    }
    if (subprog_no > line->li_context->lc_subprogs_count) {
        _dwarf_error(dbg, error, DW_DLE_NO_FILE_NAME);
        return DW_DLV_ERROR;
    }
    /*  Adjusting for 1 origin subprog no */
    subprog = &line->li_context->lc_subprogs[subprog_no - 1];
    *subprog_name = (char *)subprog->ds_subprog_name;
    *decl_line = subprog->ds_decl_line;
    res = _dwarf_filename(line->li_context,
        subprog->ds_decl_file,
        decl_filename,
        "dwarf_line_subprog",error);
    if (res != DW_DLV_OK) {
        *decl_filename = NULL;
        return res;
    }
    return DW_DLV_OK;
}

/*  This is another line_context_destructor. */
static void
delete_line_context_itself(Dwarf_Line_Context context)
{

    Dwarf_Debug dbg = 0;
    Dwarf_File_Entry fe = 0;

    if (context->lc_magic != DW_CONTEXT_MAGIC) {
        /* Something is wrong. */
        return;
    }
    dbg = context->lc_dbg;
    fe = context->lc_file_entries;
    while (fe) {
        Dwarf_File_Entry fenext = fe->fi_next;
        fe->fi_next = 0;
        free(fe);
        fe = fenext;
    }
    context->lc_file_entries = 0;
    context->lc_file_entry_count = 0;
    context->lc_file_entry_baseindex = 0;
    context->lc_file_entry_endindex = 0;
    if (context->lc_subprogs) {
        free(context->lc_subprogs);
        context->lc_subprogs = 0;
    }
    free(context->lc_directory_format_values);
    context->lc_directory_format_values = 0;
    free(context->lc_file_format_values);
    context->lc_file_format_values = 0;
    if (context->lc_include_directories) {
        free(context->lc_include_directories);
        context->lc_include_directories = 0;
    }
    context->lc_magic = 0xdead;
    dwarf_dealloc(dbg, context, DW_DLA_LINE_CONTEXT);
}

/*  It's impossible for callers of dwarf_srclines() to get to and
    free all the resources (in particular, the li_context and its
    lc_file_entries).
    So this function, new July 2005, does it.

    Those using standard DWARF should use
    dwarf_srclines_b() and dwarf_srclines_dealloc_b()
    instead of dwarf_srclines and dwarf_srclines_dealloc()
    as that gives access to various bits of useful information.

    New October 2015.
    This should be used to deallocate all
    lines data that is
    set up by dwarf_srclines_b().
    This and dwarf_srclines_b() are now (October 2015)
    the preferred routine to use.  */
void
dwarf_srclines_dealloc_b(Dwarf_Line_Context line_context)
{
    Dwarf_Line *linestable = 0;
    Dwarf_Signed linescount = 0;
    Dwarf_Signed i = 0;
    Dwarf_Debug dbg = 0;

    if (!line_context) {
        return;
    }
    if (!line_context || line_context->lc_magic != DW_CONTEXT_MAGIC) {
        /*  Something is badly wrong here.*/
        return;
    }
    dbg = line_context->lc_dbg;
    linestable = line_context->lc_linebuf_logicals;
    if (linestable) {
        linescount = line_context->lc_linecount_logicals;
        if (linescount >= 0) {
            for (i = 0; i < linescount ; ++i) {
                dwarf_dealloc(dbg, linestable[i], DW_DLA_LINE);
            }
        } /* else bogus negative count. do not dealloc things. */
        dwarf_dealloc(dbg, linestable, DW_DLA_LIST);
    }
    line_context->lc_linebuf_logicals = 0;
    line_context->lc_linecount_logicals = 0;

    linestable = line_context->lc_linebuf_actuals;
    if (linestable) {
        linescount = line_context->lc_linecount_actuals;
        if (linescount >= 0) {
            for (i = 0; i <linescount ; ++i) {
                dwarf_dealloc(dbg, linestable[i], DW_DLA_LINE);
            }
        } /* else bogus negative count. do not dealloc things. */
        dwarf_dealloc(dbg, linestable, DW_DLA_LIST);
    }
    line_context->lc_linebuf_actuals = 0;
    line_context->lc_linecount_actuals = 0;
    delete_line_context_itself(line_context);
}

/*  There is an error, so count it. If we are printing
    errors by command line option, print the details.  */
void
_dwarf_print_header_issue(Dwarf_Debug dbg,
    const char *specific_msg,
    Dwarf_Small *data_start,
    Dwarf_Signed value,
    unsigned index,
    unsigned tabv,
    unsigned linetabv,
    int *err_count_out)
{
    if (!err_count_out) {
        return;
    }
    /* Are we in verbose mode */
    if (dwarf_cmdline_options.check_verbose_mode){
        dwarfstring m1;

        dwarfstring_constructor(&m1);
        dwarfstring_append(&m1,
            "\n*** DWARF CHECK: "
            ".debug_line: ");
        dwarfstring_append(&m1,(char *)specific_msg);
        dwarfstring_append_printf_i(&m1,
            " %" DW_PR_DSd,value);
        if (index || tabv || linetabv) {
            dwarfstring_append_printf_u(&m1,
                "; Mismatch index %u",index);
            dwarfstring_append_printf_u(&m1,
                " stdval %u",tabv);
            dwarfstring_append_printf_u(&m1,
                " linetabval %u",linetabv);
        }
        if (data_start >= dbg->de_debug_line.dss_data &&
            (data_start < (dbg->de_debug_line.dss_data +
            dbg->de_debug_line.dss_size))) {
            Dwarf_Unsigned off =
                data_start - dbg->de_debug_line.dss_data;

            dwarfstring_append_printf_u(&m1,
                " at offset 0x%" DW_PR_XZEROS DW_PR_DUx,off);
            dwarfstring_append_printf_u(&m1,
                "  ( %" DW_PR_DUu " ) ",off);
        } else {
            dwarfstring_append(&m1,
                " (unknown section location) ");
        }
        dwarfstring_append(&m1,"***\n");
        _dwarf_printf(dbg,dwarfstring_string(&m1));
        dwarfstring_destructor(&m1);
    }
    *err_count_out += 1;
}

void
_dwarf_report_bad_lnct( Dwarf_Debug dbg,
    Dwarf_Unsigned ltype,
    int dlecode,
    const char  *dlename,
    Dwarf_Error *error)
{
    dwarfstring m;  /* constructor_static ok */
    dwarfstring f2; /* constructor_static ok */
    const char *typename = 0;
    char tnbuf[48];
    char mnbuf[100];

    dwarfstring_constructor_static(&f2,tnbuf,sizeof(tnbuf));
    dwarf_get_LNCT_name((unsigned int)ltype,&typename);
    if (!typename) {
        dwarfstring_append_printf_u(&f2,
            "Invalid attribute "
            " 0x" DW_PR_DUx,ltype);
    } else {
        dwarfstring_append(&f2,(char *)typename);
    }
    dwarfstring_constructor_static(&m,mnbuf,sizeof(mnbuf));
    dwarfstring_append_printf_s(&m,
        "%s: Unexpected DW_LNCT type",(char *)dlename);
    dwarfstring_append_printf_s(&m,
        " %s ",
        dwarfstring_string(&f2));
    _dwarf_error_string(dbg, error, dlecode,
        dwarfstring_string(&m));
    dwarfstring_destructor(&m);
    dwarfstring_destructor(&f2);
}

static void
report_ltype_form_issue(Dwarf_Debug dbg,
    Dwarf_Half ltype,
    Dwarf_Half form,
    const char *splmsg,
    Dwarf_Error *error)
{
    dwarfstring m;  /* constructor_fixed ok */
    dwarfstring f2; /* construcot_static ok */
    dwarfstring f;  /* construcot_static ok */
    const char *formname = 0;
    const char *typename = 0;
    char fnbuf[32];
    char f2buf[32];
    char mbuf[120];

    dwarfstring_constructor_static(&f,fnbuf,sizeof(fnbuf));
    dwarfstring_constructor_static(&f2,f2buf,sizeof(f2buf));
    dwarf_get_LNCT_name(ltype,&typename);
    if (!typename) {
        dwarfstring_append_printf_u(&f2,
            "Invalid DW_LNCT "
            " 0x" DW_PR_DUx,ltype);
    } else {
        dwarfstring_append(&f2,(char *)typename);
    }
    dwarf_get_FORM_name(form,&formname);
    if (!formname) {
        dwarfstring_append_printf_u(&f,
            "Invalid Form Code "
            " 0x" DW_PR_DUx,form);
    } else {
        dwarfstring_append(&f,(char *)formname);
    }
    dwarfstring_constructor_static(&m,mbuf,sizeof(mbuf));
    dwarfstring_append_printf_s(&m,
        "DW_DLE_LNCT_FORM_CODE_NOT_HANDLED: form %s "
        "instead of a specifically "
        "allowed offset form",
        dwarfstring_string(&f));
    dwarfstring_append_printf_s(&m,
        " on line type %s",
        dwarfstring_string(&f2));
    if (splmsg) {
        dwarfstring_append(&m," ");
        dwarfstring_append(&m,(char *)splmsg);
    }
    _dwarf_error_string(dbg, error,
        DW_DLE_LNCT_FORM_CODE_NOT_HANDLED,
        dwarfstring_string(&m));
    dwarfstring_destructor(&m);
    dwarfstring_destructor(&f);
    dwarfstring_destructor(&f2);
}

int
_dwarf_decode_line_string_form(Dwarf_Debug dbg,
    Dwarf_Unsigned ltype,
    Dwarf_Unsigned form,
    Dwarf_Unsigned offset_size,
    Dwarf_Small **line_ptr,
    Dwarf_Small *line_ptr_end,
    char **return_str,
    Dwarf_Error * error)
{
    int res = 0;

    switch (form) {
    case DW_FORM_line_strp: {
        Dwarf_Small *secstart = 0;
        Dwarf_Small *secend = 0;
        Dwarf_Small *strptr = 0;
        Dwarf_Unsigned offset = 0;
        Dwarf_Small *offsetptr = *line_ptr;

        res = _dwarf_load_section(dbg,
            &dbg->de_debug_line_str,error);
        if (res != DW_DLV_OK) {
            return res;
        }

        secstart = dbg->de_debug_line_str.dss_data;
        secend = secstart + dbg->de_debug_line_str.dss_size;

        READ_UNALIGNED_CK(dbg, offset, Dwarf_Unsigned,
            offsetptr, offset_size,
            error,line_ptr_end);
        *line_ptr += offset_size;
        strptr = secstart + offset;
        res = _dwarf_check_string_valid(dbg,
            secstart,strptr,secend,
            DW_DLE_LINE_STRP_OFFSET_BAD,error);
        if (res != DW_DLV_OK) {
            return res;
        }
        *return_str = (char *) strptr;
        return DW_DLV_OK;
        }
    case DW_FORM_string: {
        Dwarf_Small *secend = line_ptr_end;
        Dwarf_Small *strptr = *line_ptr;

        res = _dwarf_check_string_valid(dbg,
            strptr ,strptr,secend,
            DW_DLE_LINE_STRING_BAD,error);
        if (res != DW_DLV_OK) {
            return res;
        }
        *return_str = (char *)strptr;
        *line_ptr += strlen((const char *)strptr) + 1;
        return DW_DLV_OK;
        }
    default:
        report_ltype_form_issue(dbg, (Dwarf_Half)ltype,
            (Dwarf_Half)form,0,error);
        return DW_DLV_ERROR;
    }
}

int
_dwarf_decode_line_udata_form(Dwarf_Debug dbg,
    Dwarf_Unsigned ltype,
    Dwarf_Unsigned form,
    Dwarf_Small **line_ptr,
    Dwarf_Unsigned *return_val,
    Dwarf_Small *line_end_ptr,
    Dwarf_Error * error)
{
    Dwarf_Unsigned val = 0;
    Dwarf_Small * lp = *line_ptr;
    const char *splmsg = 0;

    /* We will not get here for DW_LNCT_MD5,
        no need to consider DW_FORM_data16. */
    switch (form) {
    case DW_FORM_udata:
        if (ltype != DW_LNCT_directory_index &&
            ltype != DW_LNCT_timestamp &&
            ltype != DW_LNCT_GNU_decl_file &&
            ltype != DW_LNCT_GNU_decl_line &&
            ltype != DW_LNCT_size) {
            break;
        }
        DECODE_LEB128_UWORD_CK(lp, val,dbg,error,line_end_ptr);
        *return_val = val;
        *line_ptr = lp;
        return DW_DLV_OK;
    case DW_FORM_data1:
        if (ltype != DW_LNCT_directory_index &&
            ltype != DW_LNCT_GNU_decl_file &&
            ltype != DW_LNCT_GNU_decl_line &&
            ltype != DW_LNCT_size) {
            break;
        }
        *return_val = *lp;
        *line_ptr = lp+1;
        return DW_DLV_OK;
    case DW_FORM_data2:
        if (ltype != DW_LNCT_directory_index &&
            ltype != DW_LNCT_GNU_decl_file &&
            ltype != DW_LNCT_GNU_decl_line &&
            ltype != DW_LNCT_size) {
            break;
        }
        READ_UNALIGNED_CK(dbg, val, Dwarf_Unsigned,
            lp,DWARF_HALF_SIZE,
            error,line_end_ptr);
        *return_val = val;
        *line_ptr = lp + DWARF_HALF_SIZE;
        return DW_DLV_OK;
    case DW_FORM_data4:
        if (ltype != DW_LNCT_timestamp &&
            ltype != DW_LNCT_GNU_decl_file &&
            ltype != DW_LNCT_GNU_decl_line &&
            ltype != DW_LNCT_size) {
            break;
        }
        READ_UNALIGNED_CK(dbg, val, Dwarf_Unsigned,
            lp,DWARF_32BIT_SIZE,
            error,line_end_ptr);
        *return_val = val;
        *line_ptr = lp + DWARF_32BIT_SIZE;
        return DW_DLV_OK;
    case DW_FORM_block: {
        Dwarf_Unsigned leblen = 0;
        Dwarf_Unsigned length = 0;
        Dwarf_Small *dataptr = 0;

        if (ltype != DW_LNCT_timestamp) {
            break;
        }
        DECODE_LEB128_UWORD_LEN_CK(lp, length, leblen,
            dbg,error,line_end_ptr);
        dataptr = lp +leblen;
        if (length > sizeof(Dwarf_Unsigned)) {
            splmsg = "FORM_block length bigger than Dwarf_Unsigned";
            break;
        }
        if (dataptr >= line_end_ptr ) {
            splmsg = "FORM_block data starts past end of data";
            break;
        }
        if ((dataptr + length) > line_end_ptr) {
            splmsg = "FORM_block data runs past end of data";
            break;
        }
        READ_UNALIGNED_CK(dbg, val, Dwarf_Unsigned,
            dataptr,length,
            error,line_end_ptr);
        *return_val = val;
        *line_ptr = dataptr+length;
        return DW_DLV_OK;
        }

    case DW_FORM_data8:
        if (ltype != DW_LNCT_size &&
            ltype != DW_LNCT_timestamp) {
            break;
        }
        READ_UNALIGNED_CK(dbg, val, Dwarf_Unsigned,
            lp,DWARF_64BIT_SIZE,
            error,line_end_ptr);
        *return_val = val;
        *line_ptr = lp + DWARF_64BIT_SIZE;
        return DW_DLV_OK;
    default: break;
    }
    report_ltype_form_issue(dbg, (Dwarf_Half)ltype,
        (Dwarf_Half)form,splmsg,error);
    return DW_DLV_ERROR;
}

void
_dwarf_update_chain_list( Dwarf_Chain chain_line,
    Dwarf_Chain *head_chain, Dwarf_Chain *curr_chain)
{
    if (*head_chain == NULL) {
        *head_chain = chain_line;
    } else {
        (*curr_chain)->ch_next = chain_line;
    }
    *curr_chain = chain_line;
}

void
_dwarf_free_chain_entries(Dwarf_Debug dbg,Dwarf_Chain head,
    Dwarf_Unsigned count)
{
    Dwarf_Unsigned i = 0;
    Dwarf_Chain curr_chain = head;
    for (i = 0; i < count; i++) {
        Dwarf_Chain t = curr_chain;
        void *item = t->ch_item;
        int itype = t->ch_itemtype;

        if (item && itype) { /* valid DW_DLA types are never 0 */
            dwarf_dealloc(dbg,item,itype);
            t->ch_item = 0;
        }
        curr_chain = curr_chain->ch_next;
        dwarf_dealloc(dbg, t, DW_DLA_CHAIN);
    }
}

int
_dwarf_add_to_files_list(Dwarf_Line_Context context,
    Dwarf_File_Entry fe)
{
    unsigned int version = context->lc_version_number;
    if (!context->lc_file_entries) {
        context->lc_file_entries = fe;
    } else {
        context->lc_last_entry->fi_next = fe;
    }
    context->lc_last_entry = fe;
    context->lc_file_entry_count++;
    /*  Here we attempt to write code to make it easy to interate
        though source file names without having to code specially
        for DWARF2,3,4 vs DWARF5 */
    /*  Remember that 0xf006 is the version of
        the experimental line table */
    if (version == DW_LINE_VERSION5) {
        context->lc_file_entry_baseindex = 0;
        context->lc_file_entry_endindex =
            context->lc_file_entry_count;
    } else {
        context->lc_file_entry_baseindex = 1;
        context->lc_file_entry_endindex =
            context->lc_file_entry_count +1;
    }
    return DW_DLV_OK;
}

int
_dwarf_line_context_constructor(Dwarf_Debug dbg, void *m)
{
    Dwarf_Line_Context line_context = (Dwarf_Line_Context)m;
    /*  dwarf_get_alloc ensures the bytes are all zero
        when m is passed to us. */
    line_context->lc_magic = DW_CONTEXT_MAGIC;
    line_context->lc_dbg =  dbg;
    return DW_DLV_OK;
}

/*  This cleans up a contex record.
    The lines tables (actuals and logicals)
    are themselves items that will
    be dealloc'd either manually
    or, at closing the libdwarf dbg,
    automatically.  So we DO NOT
    touch the lines tables here
    See also: delete_line_context_itself()
*/
void
_dwarf_line_context_destructor(void *m)
{
    Dwarf_Line_Context line_context = (Dwarf_Line_Context)m;
    if (line_context->lc_magic != DW_CONTEXT_MAGIC) {
        /* Nothing is safe, do nothing. */
        return;
    }
    if (line_context->lc_include_directories) {
        free(line_context->lc_include_directories);
        line_context->lc_include_directories = 0;
        line_context->lc_include_directories_count = 0;
    }
    if (line_context->lc_file_entries) {
        Dwarf_File_Entry fe = line_context->lc_file_entries;
        while(fe) {
            Dwarf_File_Entry t = fe;
            fe = t->fi_next;
            t->fi_next = 0;
            free(t);
        }
        line_context->lc_file_entries     = 0;
        line_context->lc_last_entry       = 0;
        line_context->lc_file_entry_count = 0;
        line_context->lc_file_entry_baseindex   = 0;
        line_context->lc_file_entry_endindex    = 0;
    }
    free(line_context->lc_directory_format_values);
    line_context->lc_directory_format_values = 0;
    free(line_context->lc_file_format_values);
    line_context->lc_file_format_values = 0;

    if (line_context->lc_subprogs) {
        free(line_context->lc_subprogs);
        line_context->lc_subprogs = 0;
        line_context->lc_subprogs_count = 0;
    }
    line_context->lc_magic = 0;
    return;
}
