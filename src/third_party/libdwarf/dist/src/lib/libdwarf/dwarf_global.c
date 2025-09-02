/*

  Copyright (C) 2000-2005 Silicon Graphics, Inc.  All Rights Reserved.
  Portions Copyright (C) 2007-2011 David Anderson. All Rights Reserved.

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
#include <stdio.h>

#include <string.h> /* strlen() */
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
#include "dwarf_string.h"
#include "dwarf_global.h"

#ifdef __sgi  /* __sgi should only be defined for IRIX/MIPS. */
/* The 'fixup' here intended for IRIX targets only.
   With a  2+GB Elf64 IRIX executable (under 4GB in size),
   some DIE offsets wrongly
   got the 32bit upper bit sign extended.  For the cu-header
   offset in the .debug_pubnames section  and in the
   .debug_aranges section.
   the 'varp' here is a pointer to an offset into .debug_info.
   We fix up the offset here if it seems advisable..

   As of June 2005 we have identified a series of mistakes
   in ldx64 that can cause this (64 bit values getting passed
   thru 32-bit signed knothole).
*/
void
_dwarf_fix_up_offset_irix(Dwarf_Debug dbg,
    Dwarf_Unsigned * varp, char *caller_site_name)
{

    Dwarf_Unsigned var = *varp;

#define UPPER33 0xffffffff80000000LL
#define LOWER32         0xffffffffLL
    /*  Restrict the hack to the known case. Upper 32 bits erroneously
        sign extended from lower 32 upper bit. */
    if ((var & UPPER33) == UPPER33) {
        var &= LOWER32;
        /* Apply the fix. Dreadful hack. */
        *varp = var;
    }
#undef UPPER33
#undef LOWER32
    return;
}
#endif /* __sgi */

#if 0 /* debug_print_range (debugging) */
/*  Debugging only. Requires start. can calulate one of len, end */
static void
debug_print_range(const char *msg,
    int lineno,
    void *start, signed long len,
    void *end)
{

    char *st = (char *)start;
    char *en = (char *)end;
    signed long le = len;

    if (len) {
        if (en) {
            le = (long)(en-st);
        } else {
            en= start+len;
        }
    } else if (en) {
        le = (long)(en-st);
    }
    printf("RANGEdebug %s  st=0x%lx le=%ld en=0x%lx line %d\n",
        msg,(unsigned long)st,le,(unsigned long)en,lineno);
}
#endif /* 0 */

static void
dealloc_globals_chain(Dwarf_Debug dbg,
    Dwarf_Chain head_chain)
{
    Dwarf_Chain curr_chain = 0;
    int chaintype = DW_DLA_CHAIN;
    Dwarf_Global_Context lastcontext = 0;
    Dwarf_Global_Context curcontext = 0;

    curr_chain = head_chain;
    for (; curr_chain; ) {
        Dwarf_Global item = 0;
        int itemtype = 0;
        Dwarf_Chain prev = 0;

        item = (Dwarf_Global)curr_chain->ch_item;
        itemtype = curr_chain->ch_itemtype;
        curcontext = item->gl_context;
        if (curcontext && curcontext != lastcontext) {
            /* First time we see a context, dealloc it. */
            lastcontext = curcontext;
            dwarf_dealloc(dbg,curcontext,curcontext->pu_alloc_type);
        }
        prev = curr_chain;
        dwarf_dealloc(dbg, item,itemtype);
        prev->ch_item = 0;
        curr_chain = curr_chain->ch_next;
        dwarf_dealloc(dbg, prev, chaintype);
    }
}

/*  INVARIANTS:
    1) on error does not leak Dwarf_Global
    2) glname is not malloc space. Never free.
*/
static int
_dwarf_make_global_add_to_chain(Dwarf_Debug dbg,
    Dwarf_Global_Context pubnames_context,
    Dwarf_Off            die_offset_in_cu,
    unsigned char       *glname,
    Dwarf_Signed        *global_count,
    Dwarf_Bool          *pubnames_context_on_list,
    Dwarf_Unsigned       global_DLA_code,
    Dwarf_Chain        **plast_chain,
    Dwarf_Half           tag,
    Dwarf_Error         *error)
{
    Dwarf_Chain  curr_chain = 0;
    Dwarf_Global global = 0;

    global = (Dwarf_Global)
        _dwarf_get_alloc(dbg, (Dwarf_Small)global_DLA_code, 1);
    if (!global) {
        _dwarf_error_string(dbg, error, DW_DLE_ALLOC_FAIL,
            "DW_DLE_ALLOC_FAIL: Allocating Dwarf_Global");
        return DW_DLV_ERROR;
    }
    (*global_count)++;
    /*  Recording the same context in another Dwarf_Global */
    global->gl_context = pubnames_context;
    global->gl_alloc_type = (Dwarf_Small)global_DLA_code;
    global->gl_named_die_offset_within_cu = die_offset_in_cu;
    global->gl_name = glname;
    global->gl_tag = tag;
    /* Finish off current entry chain */
    curr_chain = (Dwarf_Chain) _dwarf_get_alloc(dbg,
        (Dwarf_Small)DW_DLA_CHAIN, 1);
    if (!curr_chain) {
        dwarf_dealloc(dbg,global,pubnames_context->pu_alloc_type);
        _dwarf_error_string(dbg, error, DW_DLE_ALLOC_FAIL,
            "DW_DLE_ALLOC_FAIL: allocating a Dwarf_Chain"
            " internal structure.");
        return DW_DLV_ERROR;
    }
    /* Put current global on singly_linked list. */
    curr_chain->ch_item = (Dwarf_Global) global;
    curr_chain->ch_itemtype = (int)global_DLA_code;
    **plast_chain = curr_chain;
    *plast_chain = &(curr_chain->ch_next);
    *pubnames_context_on_list = TRUE;
    return DW_DLV_OK;
}

static int
_dwarf_chain_to_array(Dwarf_Debug dbg,
    Dwarf_Chain head_chain,
    Dwarf_Signed global_count,
    Dwarf_Global **globals,
    Dwarf_Error *error)
{
    Dwarf_Global *ret_globals = 0;

    if (!head_chain ) {
        /* ASSERT: global_count == 0 */
        return DW_DLV_NO_ENTRY;
    }
    /*  Now turn list into a block */
    /*  Points to contiguous block of Dwarf_Global. */
    ret_globals = (Dwarf_Global *)
        _dwarf_get_alloc(dbg, (Dwarf_Small)DW_DLA_LIST,
            (Dwarf_Unsigned)global_count);
    if (!ret_globals) {
        dealloc_globals_chain(dbg,head_chain);
        _dwarf_error_string(dbg, error, DW_DLE_ALLOC_FAIL,
            "DW_DLE_ALLOC_FAIL: Allocating a Dwarf_Global");
        return DW_DLV_ERROR;
    }

    /*  Store pointers to Dwarf_Global_s structs in contiguous block,
        and deallocate the chain.  This ignores the various
        headers, since they are not involved. */
    {
        Dwarf_Signed i = 0;
        Dwarf_Chain curr_chain = 0;

        curr_chain = head_chain;
        for ( ; i < global_count; i++) {
            Dwarf_Chain prev = 0;

            *(ret_globals + i) = curr_chain->ch_item;
            prev = curr_chain;
            curr_chain = curr_chain->ch_next;
            prev->ch_item = 0; /* Not actually necessary. */
            dwarf_dealloc(dbg, prev, DW_DLA_CHAIN);
        }
    }
    head_chain = 0; /* Unneccesary, but showing intent. */
    *globals = ret_globals;
    return DW_DLV_OK;
}

static void
pubnames_error_length(Dwarf_Debug dbg,
    Dwarf_Error *error,
    Dwarf_Unsigned spaceneeded,
    const char *secname,
    const char *specificloc)
{
    dwarfstring m;

    dwarfstring_constructor(&m);
    dwarfstring_append(&m,"DW_DLE_PUBNAMES_LENGTH_BAD: "
        " In section ");
    dwarfstring_append(&m,(char *)secname);
    dwarfstring_append_printf_u(&m,
        " %u bytes of space needed "
        "but the section is out of space ",
        spaceneeded);
    dwarfstring_append(&m, "reading ");
    dwarfstring_append(&m, (char *)specificloc);
    dwarfstring_append(&m, ".");
    _dwarf_error_string(dbg,error,DW_DLE_PUBNAMES_LENGTH_BAD,
        dwarfstring_string(&m));
    dwarfstring_destructor(&m);
}

/*  There are only 6 DW_IDX values defined in DWARF5
    so 7 would suffice, but lets allow for future DW_IDX too.

    On returning DW_DLV_ERROR the caller will free the
    chain, we do not need to here.  */
#define  IDX_ARRAY_SIZE 12
static int
_dwarf_internal_get_debug_names_globals(Dwarf_Debug dbg,
    Dwarf_Chain  **pplast_chain,
    Dwarf_Signed  *total_count,
    Dwarf_Error   *error,
    int            context_DLA_code,
    int            global_DLA_code)
{
    int                  res = 0;
    Dwarf_Off            cur_offset = 0;
    Dwarf_Off            next_table_offset = 0;
    Dwarf_Dnames_Head    dn_head = 0;
    Dwarf_Bool           pubnames_context_on_list = FALSE;
    Dwarf_Global_Context pubnames_context = 0;

    res = _dwarf_load_section(dbg, &dbg->de_debug_names,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    if (!dbg->de_debug_names.dss_size) {
        return DW_DLV_NO_ENTRY;
    }

    for (  ; ; cur_offset = next_table_offset) {
        Dwarf_Unsigned comp_unit_count = 0;
        Dwarf_Unsigned name_count = 0;
        Dwarf_Unsigned section_size = 0;
        Dwarf_Half     table_version = 0;
        Dwarf_Half     offset_size = 0;
        Dwarf_Unsigned n = 0;

        res = dwarf_dnames_header(dbg,cur_offset,&dn_head,
            &next_table_offset,error);
        if (res == DW_DLV_NO_ENTRY) {
            /*  Detected the end point */
            break;
        }
        if (res == DW_DLV_ERROR) {
            return res;
        }
        /* DW_DLV_NO_ENTRY impossible here */
        res = dwarf_dnames_sizes(dn_head,&comp_unit_count,
            0,0,0,&name_count,0,0,0,0,&section_size,&table_version,
            &offset_size,error);
        if (res != DW_DLV_OK) {
            dwarf_dealloc_dnames(dn_head);
            return res;
        }

        for (n = 1 ; n <= name_count; ++n) {
            Dwarf_Unsigned aindex = 0;
            Dwarf_Unsigned bucket_number = 0;
            Dwarf_Unsigned hash_value = 0;
            Dwarf_Unsigned offset_to_debug_str = 0;
            char *ptrtostr = 0;
            Dwarf_Unsigned abbrev_code = 0;
            Dwarf_Unsigned offset_in_entrypool = 0;
            Dwarf_Unsigned offset_of_next_entrypool = 0;
            Dwarf_Half     abbrev_tag = 0;
            Dwarf_Half     idxattr_array[IDX_ARRAY_SIZE];
            Dwarf_Half     form_array[IDX_ARRAY_SIZE];
            Dwarf_Unsigned attr_count = 0;

            Dwarf_Unsigned epool_abbrev_code = 0;
            Dwarf_Half     epool_abbrev_tag = 0;
            Dwarf_Unsigned epool_value_count = 0;
            Dwarf_Unsigned epool_index_of_abbrev = 0;
            Dwarf_Unsigned epool_offset_of_initial_value = 0;

            Dwarf_Unsigned offset_array[IDX_ARRAY_SIZE];
            Dwarf_Sig8     signature_array[IDX_ARRAY_SIZE];
            Dwarf_Bool     single_cu = FALSE;
            Dwarf_Unsigned single_cu_hdr_offset = 0;
            Dwarf_Unsigned die_local_offset = 0;
            Dwarf_Unsigned cu_header_global_offset = 0;
            Dwarf_Unsigned cu_header_index = 0;
            Dwarf_Bool have_die_local_offset = FALSE;
            Dwarf_Bool have_cu_header_index = FALSE;
            Dwarf_Bool have_cu_header_global_offset = 0;
            Dwarf_Bool have_cu_header_offset = FALSE;

            memset(idxattr_array,0,sizeof(Dwarf_Half)*IDX_ARRAY_SIZE);
            memset(form_array,0,sizeof(Dwarf_Half)*IDX_ARRAY_SIZE);
            memset(offset_array,0,sizeof(Dwarf_Unsigned)*
                IDX_ARRAY_SIZE);
            memset(signature_array,0,sizeof(Dwarf_Sig8)*
                IDX_ARRAY_SIZE);
            res = dwarf_dnames_name(dn_head,n,&bucket_number,
                &hash_value,&offset_to_debug_str,&ptrtostr,
                &offset_in_entrypool,
                &abbrev_code,&abbrev_tag,IDX_ARRAY_SIZE,
                idxattr_array,form_array,&attr_count,error);
            if (res == DW_DLV_ERROR) {
                dwarf_dealloc_dnames(dn_head);
                return res;
            }
            if (res == DW_DLV_NO_ENTRY) {
                /*  internal error or corruption  or simply past
                    the end of section.  Normal.*/
                dwarf_dealloc_dnames(dn_head);
                return res;
            }
            switch (abbrev_tag) {
            case DW_TAG_subprogram:
            case DW_TAG_variable:
            case DW_TAG_label:
            case DW_TAG_member:
            case DW_TAG_common_block:
            case DW_TAG_enumerator:
            case DW_TAG_namelist:
            case DW_TAG_module:
                break;
            default:
                continue;
            }
            if (attr_count >= IDX_ARRAY_SIZE) {
                dwarf_dealloc_dnames(dn_head);
                _dwarf_error_string(dbg,error,
                    DW_DLE_DEBUG_NAMES_ERROR,
                    "DW_DLE_DEBUG_NAMES_ERROR: "
                    "a .debug_names index attribute count "
                    "is unreasonable. "
                    "Corrupt data.");
                return DW_DLV_ERROR;
            }

            res = dwarf_dnames_entrypool(dn_head,
                offset_in_entrypool,&epool_abbrev_code,
                &epool_abbrev_tag,&epool_value_count,
                &epool_index_of_abbrev,
                &epool_offset_of_initial_value,error);
            if (res == DW_DLV_ERROR) {
                dwarf_dealloc_dnames(dn_head);
                return DW_DLV_ERROR;
            }
            if (res == DW_DLV_NO_ENTRY) {
                dwarf_dealloc_dnames(dn_head);
                _dwarf_error_string(dbg,error,
                    DW_DLE_DEBUG_NAMES_ERROR,
                    "DW_DLE_DEBUG_NAMES_ERROR: "
                    "a .debug_names entry has no entrypool. "
                    "Unreasonable. "
                    "Corrupt data.");
                return DW_DLV_ERROR;
            }
            res = dwarf_dnames_entrypool_values(dn_head,
                epool_index_of_abbrev,
                epool_offset_of_initial_value,IDX_ARRAY_SIZE,
                idxattr_array,form_array,
                offset_array, signature_array,
                &single_cu,&single_cu_hdr_offset,
                &offset_of_next_entrypool,
                error);

            if (res == DW_DLV_ERROR) {
                dwarf_dealloc_dnames(dn_head);
                return res;
            }
            if (res == DW_DLV_NO_ENTRY) {
                dwarf_dealloc_dnames(dn_head);
                _dwarf_error_string(dbg,error,
                    DW_DLE_DEBUG_NAMES_ERROR,
                    "DW_DLE_DEBUG_NAMES_ERROR: "
                    "a .debug_names entry entrypool value "
                    " improperly empty. "
                    "Unreasonable. "
                    "Corrupt data.");
                return DW_DLV_ERROR;
            }
            for (aindex = 0; aindex < epool_value_count; aindex++) {
                Dwarf_Half idx = idxattr_array[aindex];

                switch(idx) {
                case DW_IDX_type_unit:
                case DW_IDX_parent:
                case DW_IDX_type_hash:
                    break;
                case DW_IDX_compile_unit:
                    cu_header_index =  offset_array[aindex];
                    have_cu_header_index = TRUE;
                    break;
                case DW_IDX_die_offset:
                    die_local_offset = offset_array[aindex];
                    have_die_local_offset = TRUE;
                    break;
                default:
                    /*  Non-standard DW_IDX. */
                    break;
                }
            }
            if (!have_cu_header_index) {
                if (single_cu) {
                    have_cu_header_global_offset = TRUE;
                    cu_header_global_offset = single_cu_hdr_offset;
                } else {
                    /* Ignore this entry, not global? */
                    continue;
                }
            }

            if (!have_die_local_offset) {
                /* Ignore this entry */
                continue;
            }
            if (!have_cu_header_offset) {
                int ores = 0;
                Dwarf_Unsigned offset = 0;
                Dwarf_Sig8     signature;
                Dwarf_Error    cuterr = 0;

                ores = dwarf_dnames_cu_table(dn_head,
                    "cu", cu_header_index,
                    &offset,&signature,&cuterr);
                if (ores != DW_DLV_OK) {
                }  else {
                    cu_header_global_offset = offset;
                    have_cu_header_global_offset = TRUE;
                }
            }
            if (!have_cu_header_global_offset) {
                /* Ignore this entry */
                continue;
            }

            if (!pubnames_context ||
                (pubnames_context->pu_offset_of_cu_header !=
                cu_header_global_offset)) {
                pubnames_context_on_list = FALSE;
                pubnames_context = (Dwarf_Global_Context)
                    _dwarf_get_alloc(dbg,
                        (Dwarf_Small)context_DLA_code, 1);
                if (!pubnames_context) {
                    dwarf_dealloc_dnames(dn_head);
                    _dwarf_error_string(dbg, error, DW_DLE_ALLOC_FAIL,
                        "DW_DLE_ALLOC_FAIL: allocating "
                        "Dwarf_Global_Context");
                    return DW_DLV_ERROR;
                }
                /*  Dwarf_Global_Context initialization. */
                pubnames_context->pu_dbg = dbg;
                pubnames_context->pu_alloc_type = context_DLA_code;
                pubnames_context->pu_is_debug_names = TRUE;
                pubnames_context->pu_offset_of_cu_header =
                    cu_header_global_offset;
                /*  For .debug_names we don't need to
                    set the rest of the fields.
                    All the translations from disk
                    form to libdwarf types and the sanity
                    chacking are already done. */
            }
            /* we have an entry to set up Dwarf_Global */
            res = _dwarf_make_global_add_to_chain(dbg,
                pubnames_context,
                die_local_offset,
                (unsigned char *)ptrtostr,
                total_count,
                &pubnames_context_on_list,
                global_DLA_code,
                pplast_chain,
                abbrev_tag,
                error);
            if (res == DW_DLV_ERROR) {
                if (!pubnames_context_on_list) {
                    dwarf_dealloc(dbg,pubnames_context,
                        context_DLA_code);
                }
                dwarf_dealloc_dnames(dn_head);
                return res;
            }
        }
        dwarf_dealloc_dnames(dn_head);
        dn_head = 0;
    }
    return DW_DLV_OK;
}
#undef IDX_ARRAY_SIZE
static void
_dwarf_get_DLE_name(int errnum,dwarfstring *out)
{
    char        * basemsg = 0;
    char        * curp = 0;
    unsigned long count = 0;

    basemsg = dwarf_errmsg_by_number(errnum);
    curp = basemsg;
    while (*curp) {
        if (*curp == ' ') {
            break;
        }
        if (*curp == '(') {
            break;
        }
        ++count;
        ++curp;
    }
    dwarfstring_append_length(out,basemsg,count);
}

static void
_dwarf_global_cu_len_error_msg(Dwarf_Debug dbg,
    int errornumber,
    const char * section_name,
    Dwarf_Unsigned section_length,
    Dwarf_Unsigned cu_number,
    Dwarf_Unsigned length_section_offset,
    Dwarf_Unsigned length_field,
    Dwarf_Error *error)
{
    dwarfstring m;
    Dwarf_Unsigned remaining = 0;

    remaining = section_length - length_section_offset;
    dwarfstring_constructor(&m);
    _dwarf_get_DLE_name(errornumber,&m);
    dwarfstring_append_printf_u(&m,
        ": For cu context %u ",
        cu_number);
    dwarfstring_append_printf_s(&m,"of section %s ",
        (char *)section_name);
    dwarfstring_append_printf_u(&m,"the length field at "
        "offset %u ",length_section_offset);
    dwarfstring_append_printf_u(&m,"has value %u ",
        length_field);
    dwarfstring_append_printf_u(&m,"though just %u bytes "
        "remain in the section. Corrupt DWARF",remaining);
    _dwarf_error_string(dbg, error,errornumber,
        dwarfstring_string(&m));
    dwarfstring_destructor(&m);
}

/*  Sweeps the complete  section.
    On error it frees the head_chain,
    and the caller never sees the head_chain data.
    On success, if the out*chain data exists
    it updates the caller head_chain through
    the pointers.
*/
static int
_dwarf_internal_get_pubnames_like(Dwarf_Debug dbg,
    int         category, /* DW_GL_GLOBAL or ... */
    const char *secname,
    Dwarf_Small * section_data_ptr,
    Dwarf_Unsigned section_length,
    Dwarf_Chain  *  out_phead_chain,
    Dwarf_Chain  **  out_pplast_chain,
    Dwarf_Signed * return_count,
    Dwarf_Error * error,
    int length_err_num,
    int version_err_num)
{
    Dwarf_Small   *pubnames_like_ptr = 0;
    /*  Section offset to the above pointer. */
    Dwarf_Unsigned pubnames_like_offset = 0;

    Dwarf_Small *section_end_ptr = section_data_ptr +section_length;

    /*  Points to the context for the current set of global names,
        and contains information to identify the compilation-unit
        that the set refers to. */
    Dwarf_Global_Context pubnames_context = 0;
    Dwarf_Bool           pubnames_context_on_list = FALSE;
    Dwarf_Unsigned       context_DLA_code = DW_DLA_GLOBAL_CONTEXT;
    Dwarf_Unsigned       global_DLA_code = DW_DLA_GLOBAL;

    Dwarf_Unsigned version = 0;

    /*  Offset from the start of compilation-unit for the current
        global. */
    Dwarf_Off die_offset_in_cu = 0;
    Dwarf_Signed global_count = 0;

    /*  The count is just to improve the error message
        a few lines above. */
    Dwarf_Unsigned context_count = 0;

    if (IS_INVALID_DBG(dbg)) {
        _dwarf_error_string(NULL, error, DW_DLE_DBG_NULL,
            "DW_DLE_DBG_NULL: "
            "calling for pubnames-like data Dwarf_Debug "
            "either null or it contains"
            "a stale Dwarf_Debug pointer");
        return DW_DLV_ERROR;
    }
    /* We will eventually need the .debug_info data. Load it now. */
    if (!dbg->de_debug_info.dss_data) {
        int res = _dwarf_load_debug_info(dbg, error);

        if (res != DW_DLV_OK) {
            return res;
        }
    }
    if (section_data_ptr == NULL) {
        return DW_DLV_NO_ENTRY;
    }
    pubnames_like_ptr = section_data_ptr;
    pubnames_like_offset = 0;
    do {
        int mres = 0;
        Dwarf_Unsigned length = 0;
        int local_extension_size = 0;
        int local_length_size = 0;
        Dwarf_Off pubnames_section_cu_offset =
            pubnames_like_offset;

        /*  Some compilers emit padding at the end of each cu's area.
            pubnames_ptr_past_end_cu records the true area end for the
            pubnames(like) content of a cu.
            Essentially the length in the header and the 0
            terminator of the data are redundant information. The
            dwarf2/3 spec does not mention what to do if the length is
            past the 0 terminator. So we take any bytes left
            after the 0 as padding and ignore them. */
        Dwarf_Small *pubnames_ptr_past_end_cu = 0;

        pubnames_context_on_list = FALSE;
        pubnames_context = (Dwarf_Global_Context)
            _dwarf_get_alloc(dbg,
                (Dwarf_Small)context_DLA_code, 1);
        if (!pubnames_context) {
            dealloc_globals_chain(dbg,*out_phead_chain);
            *out_phead_chain = 0;
            _dwarf_error_string(dbg, error, DW_DLE_ALLOC_FAIL,
                "DW_DLE_ALLOC_FAIL: Allocating a"
                " Dwarf_Global_Context for a pubnames entry.");
            return DW_DLV_ERROR;
        }
        /*  ========pubnames_context not recorded anywhere yet. */
        /*  READ_AREA_LENGTH updates pubnames_like_ptr for consumed
            bytes. */
        if ((pubnames_like_ptr + DWARF_32BIT_SIZE +
            DWARF_HALF_SIZE + DWARF_32BIT_SIZE) >
            /* A minimum size needed */
            section_end_ptr) {
            pubnames_error_length(dbg,error,
                DWARF_32BIT_SIZE + DWARF_HALF_SIZE + DWARF_32BIT_SIZE,
                secname,
                "header-record");
            dealloc_globals_chain(dbg,*out_phead_chain);
            *out_phead_chain = 0;
            if (!pubnames_context_on_list) {
                dwarf_dealloc(dbg,pubnames_context,
                    context_DLA_code);
                pubnames_context = 0;
            }
            return DW_DLV_ERROR;
        }
        mres = _dwarf_read_area_length_ck_wrapper(dbg,
            &length,&pubnames_like_ptr,&local_length_size,
            &local_extension_size,section_length,section_end_ptr,
            error);
        if (mres != DW_DLV_OK) {
            dealloc_globals_chain(dbg,*out_phead_chain);
            *out_phead_chain = 0;
            if (!pubnames_context_on_list) {
                dwarf_dealloc(dbg,pubnames_context,context_DLA_code);
                pubnames_context = 0;
            }
            return mres;
        }
        {
            Dwarf_Small * localend =pubnames_like_ptr + length;
            if ((length > section_length) ||
                (localend > section_end_ptr)){
                _dwarf_global_cu_len_error_msg(dbg,
                    length_err_num,
                    secname, section_length,
                    context_count,
                    (Dwarf_Unsigned)pubnames_like_offset,
                    (Dwarf_Unsigned)length,
                    error);
                dealloc_globals_chain(dbg,*out_phead_chain);
                *out_phead_chain = 0;
                if (!pubnames_context_on_list) {
                    dwarf_dealloc(dbg,pubnames_context,
                        context_DLA_code);
                    pubnames_context = 0;
                }
                return DW_DLV_ERROR;
            }
        }
        pubnames_like_offset += local_length_size +
            local_extension_size;
        /*  The count is just to improve the error message
            a few lines above. */
        ++context_count;
        /*  Dwarf_Global_Context initialization. */
        pubnames_context->pu_global_category  = category;
        pubnames_context->pu_alloc_type =
            (unsigned)context_DLA_code;
        pubnames_context->pu_length_size =
            (unsigned char)local_length_size;
        pubnames_context->pu_length = (unsigned char)length;
        pubnames_context->pu_extension_size =
            (unsigned char)local_extension_size;
        pubnames_context->pu_dbg = dbg;
        pubnames_context->pu_pub_offset = pubnames_section_cu_offset;
        pubnames_ptr_past_end_cu = pubnames_like_ptr + length;
        pubnames_context->pu_pub_entries_end_ptr =
            pubnames_ptr_past_end_cu;
        if ((pubnames_like_ptr + (DWARF_HALF_SIZE) ) >=
            /* A minimum size needed */
            section_end_ptr) {
            pubnames_error_length(dbg,error,
                DWARF_HALF_SIZE,
                secname,"version-number");
            dealloc_globals_chain(dbg,*out_phead_chain);
            *out_phead_chain = 0;
            if (!pubnames_context_on_list) {
                dwarf_dealloc(dbg,pubnames_context,context_DLA_code);
                pubnames_context = 0;
            }
            return DW_DLV_ERROR;
        }
        mres = _dwarf_read_unaligned_ck_wrapper(dbg,
            &version,pubnames_like_ptr,DWARF_HALF_SIZE,
            section_end_ptr,error);
        if (mres != DW_DLV_OK) {
            dealloc_globals_chain(dbg,*out_phead_chain);
            *out_phead_chain = 0;
            if (!pubnames_context_on_list) {
                dwarf_dealloc(dbg,pubnames_context,context_DLA_code);
                pubnames_context = 0;
            }
            return mres;
        }
        pubnames_context->pu_version = (Dwarf_Half)version;
        pubnames_like_ptr += DWARF_HALF_SIZE;
        pubnames_like_offset += DWARF_HALF_SIZE;
        /* ASSERT: DW_PUBNAMES_VERSION2 == DW_PUBTYPES_VERSION2 */
        if (version != DW_PUBNAMES_VERSION2) {
            dealloc_globals_chain(dbg,*out_phead_chain);
            *out_phead_chain = 0;
            if (!pubnames_context_on_list) {
                dwarf_dealloc(dbg,pubnames_context,context_DLA_code);
                pubnames_context = 0;
            }
            _dwarf_error(dbg, error, version_err_num);
            return DW_DLV_ERROR;
        }

        /* Offset of CU header in debug section. */
        if ((pubnames_like_ptr + 3*pubnames_context->pu_length_size)>
            section_end_ptr) {
            pubnames_error_length(dbg,error,
                3*pubnames_context->pu_length_size,
                secname,
                "header/DIE offsets");
            dealloc_globals_chain(dbg,*out_phead_chain);
            *out_phead_chain = 0;
            if (!pubnames_context_on_list) {
                dwarf_dealloc(dbg,pubnames_context,context_DLA_code);
                pubnames_context = 0;
            }
            return DW_DLV_ERROR;
        }
        mres = _dwarf_read_unaligned_ck_wrapper(dbg,
            &pubnames_context->pu_offset_of_cu_header,
            pubnames_like_ptr,
            pubnames_context->pu_length_size,
            section_end_ptr,error);
        if (mres != DW_DLV_OK) {
            dealloc_globals_chain(dbg,*out_phead_chain);
            *out_phead_chain = 0;
            if (!pubnames_context_on_list) {
                dwarf_dealloc(dbg,pubnames_context,context_DLA_code);
                pubnames_context = 0;
            }
            return mres;
        }

        pubnames_like_ptr += pubnames_context->pu_length_size;
        pubnames_like_offset += pubnames_context->pu_length_size;

        FIX_UP_OFFSET_IRIX_BUG(dbg,
            pubnames_context->pu_offset_of_cu_header,
            "pubnames cu header offset");
        mres = _dwarf_read_unaligned_ck_wrapper(dbg,
            &pubnames_context->pu_info_length,
            pubnames_like_ptr,
            pubnames_context->pu_length_size,
            section_end_ptr,error);
        if (mres != DW_DLV_OK) {
            dealloc_globals_chain(dbg,*out_phead_chain);
            *out_phead_chain = 0;
            if (!pubnames_context_on_list) {
                dwarf_dealloc(dbg,pubnames_context,context_DLA_code);
                pubnames_context = 0;
            }
            return mres;
        }
        pubnames_like_ptr += pubnames_context->pu_length_size;
        pubnames_like_offset += pubnames_context->pu_length_size;

        if (pubnames_like_ptr > (section_data_ptr + section_length)) {
            dealloc_globals_chain(dbg,*out_phead_chain);
            *out_phead_chain = 0;
            if (!pubnames_context_on_list) {
                dwarf_dealloc(dbg,pubnames_context,context_DLA_code);
                pubnames_context = 0;
            }
            _dwarf_error(dbg, error, length_err_num);
            return DW_DLV_ERROR;
        }

        /*  Read initial offset (of DIE within CU) of a pubname, final
            entry is not a pair, just a zero offset. */
        mres = _dwarf_read_unaligned_ck_wrapper(dbg,
            &die_offset_in_cu,
            pubnames_like_ptr,
            pubnames_context->pu_length_size,
            pubnames_context->pu_pub_entries_end_ptr,error);
        if (mres != DW_DLV_OK) {
            dealloc_globals_chain(dbg,*out_phead_chain);
            *out_phead_chain = 0;
            if (!pubnames_context_on_list) {
                dwarf_dealloc(dbg,pubnames_context,context_DLA_code);
                pubnames_context = 0;
            }
            return mres;
        }
        pubnames_like_ptr += pubnames_context->pu_length_size;
        pubnames_like_offset += pubnames_context->pu_length_size;
        FIX_UP_OFFSET_IRIX_BUG(dbg,
            die_offset_in_cu, "offset of die in cu");
        if (pubnames_like_ptr > (section_data_ptr + section_length)) {
            dealloc_globals_chain(dbg,*out_phead_chain);
            *out_phead_chain = 0;
            if (!pubnames_context_on_list) {
                dwarf_dealloc(dbg,pubnames_context,context_DLA_code);
                pubnames_context = 0;
            }
            _dwarf_error(dbg, error, length_err_num);
            return DW_DLV_ERROR;
        }

        /*  Check if empty section */
        if (!die_offset_in_cu) {
            if (dbg->de_return_empty_pubnames) {
                int res = 0;

                /*  Here we have a pubnames CU with no actual
                    entries so we fake up an entry to hold the
                    header data.  There are no 'pairs' here,
                    just the end of list zero value.  We do this
                    only if de_return_empty_pubnames is set
                    so that we by default return exactly the same
                    data this always returned, yet dwarfdump can
                    request the empty-cu records get created
                    to test that feature.
                    see dwarf_get_globals_header()  */
                res = _dwarf_make_global_add_to_chain(dbg,
                    pubnames_context,
                    die_offset_in_cu,
                    /*  It is a fake global, so empty name */
                    (unsigned char *)"",
                    &global_count,
                    &pubnames_context_on_list,
                    global_DLA_code,
                    out_pplast_chain,
                    0,
                    error);
                if (res != DW_DLV_OK) {
                    dealloc_globals_chain(dbg,*out_phead_chain);
                    *out_phead_chain = 0;
                    if (!pubnames_context_on_list) {
                        dwarf_dealloc(dbg,pubnames_context,
                            context_DLA_code);
                        pubnames_context = 0;
                    }
                    return res;
                }
                /*  ========pubnames_context recorded in chain. */
            } else {
                /*  The section is empty.
                    Nowhere to record pubnames_context); */
                dwarf_dealloc(dbg,pubnames_context,context_DLA_code);
                /* pubnames_context = 0; reset at top of loop */
                continue;
            }
        }
        /* Loop thru pairs. DIE off with CU followed by string. */
        /*  Now read pairs of entries */
        while (die_offset_in_cu) {
            int res = 0;
            unsigned char *glname = 0;
            Dwarf_Unsigned nstrlen = 0;

            /*  non-zero die_offset_in_cu already read, so
                pubnames_like_ptr points to a string.  */
            res = _dwarf_check_string_valid(dbg,section_data_ptr,
                pubnames_like_ptr,
                pubnames_context->pu_pub_entries_end_ptr,
                DW_DLE_STRING_OFF_END_PUBNAMES_LIKE,error);
            if (res != DW_DLV_OK) {
                dealloc_globals_chain(dbg,*out_phead_chain);
                *out_phead_chain = 0;
                if (!pubnames_context_on_list) {
                    dwarf_dealloc(dbg,pubnames_context,
                        context_DLA_code);
                    pubnames_context = 0;
                }
                return res;
            }
            glname = (unsigned char *)pubnames_like_ptr;
            nstrlen = strlen((char *)pubnames_like_ptr);
            pubnames_like_ptr += nstrlen + 1;
            pubnames_like_offset += nstrlen + 1;
            /*  Already read offset and verified string, glname
                now points to the string. */
            res = _dwarf_make_global_add_to_chain(dbg,
                pubnames_context,
                die_offset_in_cu,
                glname,
                &global_count,
                &pubnames_context_on_list,
                global_DLA_code,
                out_pplast_chain,
                0,
                error);
            if (res != DW_DLV_OK) {
                dealloc_globals_chain(dbg,*out_phead_chain);
                *out_phead_chain = 0;
                if (!pubnames_context_on_list) {
                    dwarf_dealloc(dbg,pubnames_context,
                        context_DLA_code);
                    pubnames_context = 0;
                }
                return res;
            }
            /*  ========pubnames_context recorded in chain. */
            /*  Ensure room for a next entry  to exist. */
            if ((pubnames_like_ptr +
                pubnames_context->pu_length_size ) >
                section_end_ptr) {
                pubnames_error_length(dbg,error,
                    2*pubnames_context->pu_length_size,
                    secname,
                    "global record offset");
                dealloc_globals_chain(dbg,*out_phead_chain);
                *out_phead_chain = 0;
                if (!pubnames_context_on_list) {
                    dwarf_dealloc(dbg,pubnames_context,
                        context_DLA_code);
                    pubnames_context = 0;
                }
                return DW_DLV_ERROR;
            }
            /* Read die offset for the *next* entry */
            mres = _dwarf_read_unaligned_ck_wrapper(dbg,
                &die_offset_in_cu,
                pubnames_like_ptr,
                pubnames_context->pu_length_size,
                pubnames_context->pu_pub_entries_end_ptr,
                error);
            if (mres != DW_DLV_OK) {
                if (!pubnames_context_on_list) {
                    dwarf_dealloc(dbg,pubnames_context,
                        context_DLA_code);
                    pubnames_context = 0;
                }
                dealloc_globals_chain(dbg,*out_phead_chain);
                *out_phead_chain = 0;
                return mres;
            }
            /*  die_offset_in_cu may now be zero, meaing
                end of the pairs list */
            pubnames_like_ptr += pubnames_context->pu_length_size;
            pubnames_like_offset += pubnames_context->pu_length_size;
            FIX_UP_OFFSET_IRIX_BUG(dbg,
                die_offset_in_cu, "offset of next die in cu");
            if (pubnames_like_ptr >
                (section_data_ptr + section_length)) {
                if (!pubnames_context_on_list) {
                    dwarf_dealloc(dbg,pubnames_context,
                        context_DLA_code);
                }
                dealloc_globals_chain(dbg,*out_phead_chain);
                *out_phead_chain = 0;
                _dwarf_error(dbg, error, length_err_num);
                return DW_DLV_ERROR;
            }
        }
        /* ASSERT: die_offset_in_cu == 0 */
        if (pubnames_like_ptr > pubnames_ptr_past_end_cu) {
            /* This is some kind of error. This simply cannot happen.
            The encoding is wrong or the length in the header for
            this cu's contribution is wrong. */
            _dwarf_error(dbg, error, length_err_num);
            if (!pubnames_context_on_list) {
                dwarf_dealloc(dbg,pubnames_context,context_DLA_code);
                pubnames_context = 0;
            }
            dealloc_globals_chain(dbg,*out_phead_chain);
            *out_phead_chain = 0;
            return DW_DLV_ERROR;
        }
        /*  If there is some kind of padding at the end of
            the section, following a pairs terminator,
            as emitted by some compilers, skip over that padding and
            simply ignore the bytes thus passed-over. */
        {
            Dwarf_Unsigned finaloffset =
                pubnames_section_cu_offset+
                pubnames_context->pu_length_size +
                pubnames_context->pu_length +
                pubnames_context->pu_extension_size;
            if (finaloffset > pubnames_like_offset) {
                pubnames_like_offset = finaloffset;
            }
        }
        pubnames_like_ptr = pubnames_ptr_past_end_cu;
    } while (pubnames_like_ptr < section_end_ptr);
    *return_count = global_count;
    return DW_DLV_OK;
}

static const int err3[]=
{
DW_DLE_PUBNAMES_LENGTH_BAD,
DW_DLE_DEBUG_PUBTYPES_LENGTH_BAD,
DW_DLE_DEBUG_FUNCNAMES_LENGTH_BAD,
DW_DLE_DEBUG_TYPENAMES_LENGTH_BAD,
DW_DLE_DEBUG_VARNAMES_LENGTH_BAD,
DW_DLE_DEBUG_WEAKNAMES_LENGTH_BAD
};
static const int err4[]=
{
DW_DLE_PUBNAMES_VERSION_ERROR,
DW_DLE_DEBUG_PUBTYPES_VERSION_ERROR,
DW_DLE_DEBUG_FUNCNAMES_VERSION_ERROR,
DW_DLE_DEBUG_TYPENAMES_VERSION_ERROR,
DW_DLE_DEBUG_VARNAMES_VERSION_ERROR,
DW_DLE_DEBUG_WEAKNAMES_VERSION_ERROR
};
static const char * secna[] =
{
".debug_pubnames",
".debug_pubtypes",
".debug_funcnames",
".debug_typenames",
".debug_varnames",
".debug_weaknames",
};

/*  New in 0.6.0, unifies all the access routines
    for the sections like .debug_pubtypes.
*/
int
dwarf_globals_by_type(Dwarf_Debug dbg,
    int            requested_section,
    Dwarf_Global **contents,
    Dwarf_Signed  *ret_count,
    Dwarf_Error   *error)
{
    struct Dwarf_Section_s *section = 0;
    Dwarf_Chain  head_chain = 0;
    Dwarf_Chain *plast_chain = &head_chain;
    Dwarf_Bool   have_base_sec = FALSE;
    Dwarf_Bool   have_second_sec = FALSE;
    int          res = 0;

    /*  Zero caller's fields in case caller
        failed to do so. Bad input here causes
        segfault!  */
    *contents = 0;
    *ret_count = 0;
    CHECK_DBG(dbg,error,"dwarf_globals_by_type()");
    switch(requested_section){
    case  DW_GL_GLOBALS:
        section = &dbg->de_debug_pubnames;
        break;
    case  DW_GL_PUBTYPES:
        section = &dbg->de_debug_pubtypes;
        break;
    /*  The Following are IRIX only. */
    case  DW_GL_FUNCS:
        section = &dbg->de_debug_funcnames;
        break;
    case  DW_GL_TYPES:
        section = &dbg->de_debug_typenames;
        break;
    case  DW_GL_VARS:
        section = &dbg->de_debug_varnames;
        break;
    case  DW_GL_WEAKS:
        section = &dbg->de_debug_weaknames;
        break;
    default: {
        dwarfstring m;
        char buf[100];

        dwarfstring_constructor_static(&m,buf,sizeof(buf));
        dwarfstring_append_printf_u(&m,
            "ERROR DW_DLE_GLOBAL_NULL: Passed in Dwarf_Global "
            "requested section "
            "%u which is unknown to dwarf_globals_by_type().",
            requested_section);
        _dwarf_error_string(dbg, error, DW_DLE_GLOBAL_NULL,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    }
    res = _dwarf_load_section(dbg, section, error);
    if (res == DW_DLV_ERROR) {
        return res;
    }
    if (section->dss_size) {
        have_base_sec = TRUE;
    }

    if (have_base_sec) {
        res = _dwarf_internal_get_pubnames_like(dbg,
            requested_section,
            secna[requested_section],
            section->dss_data,
            section->dss_size,
            &head_chain,
            &plast_chain,
            ret_count,
            error,
            err3[requested_section],
            err4[requested_section]);
        if (res == DW_DLV_ERROR) {
            dealloc_globals_chain(dbg,head_chain);
            return res;
        }
    }
    if (0 == requested_section) {
        res = _dwarf_load_section(dbg, &dbg->de_debug_names,error);
        if (res == DW_DLV_ERROR) {
            return res;
        } else if (dbg->de_debug_names.dss_size) {
            have_second_sec = TRUE;
        }
    }
    if (have_second_sec) {
        res = _dwarf_internal_get_debug_names_globals(dbg,
            &plast_chain,
            ret_count,
            error,
            DW_DLA_GLOBAL_CONTEXT,
            DW_DLA_GLOBAL);
        if (res == DW_DLV_ERROR) {
            dealloc_globals_chain(dbg,head_chain);
            head_chain = 0;
            return res;
        }
    }
    res = _dwarf_chain_to_array(dbg,head_chain,
        *ret_count, contents, error);
    if (res == DW_DLV_ERROR) {
        /*  head chain maybe zero. Is ok. */
        dealloc_globals_chain(dbg,head_chain);
        return res;
    }
    /*  Must not return DW_DLV_NO_ENTRY. Count
        is set zero in caller so no need for NO_ENTRY. */
    return DW_DLV_OK;
}

int
dwarf_get_globals(Dwarf_Debug dbg,
    Dwarf_Global **ret_globals,
    Dwarf_Signed  *return_count,
    Dwarf_Error   *error)
{
    int res = 0;

    CHECK_DBG(dbg,error,"dwarf_get_globals()");
    res = dwarf_globals_by_type(dbg,
        DW_GL_GLOBALS,ret_globals,return_count,error);
    return res;
}
/* This now returns Dwarf_Global for types so
   all the dwarf_global data retrieval calls work.
   This is just a shorthand.

   Before 0.6.0 this would return Dwarf_Type.
*/
int
dwarf_get_pubtypes(Dwarf_Debug dbg,
    Dwarf_Global **types,
    Dwarf_Signed  *return_count,
    Dwarf_Error   *error)
{
    int res = 0;
    CHECK_DBG(dbg,error,"dwarf_get_pubtypes()");
    res = dwarf_globals_by_type(dbg,
        DW_GL_PUBTYPES,types,return_count,error);
    return res;
}

/* Deallocating fully requires deallocating the list
   and all entries.  But some internal data is
   not exposed, so we need a function with internal knowledge.
*/
void
dwarf_globals_dealloc(Dwarf_Debug dbg, Dwarf_Global * dwgl,
    Dwarf_Signed count)
{
    _dwarf_internal_globals_dealloc(dbg, dwgl, count);
    return;
}

void
_dwarf_internal_globals_dealloc(Dwarf_Debug dbg,
    Dwarf_Global * dwgl,
    Dwarf_Signed count)
{
    Dwarf_Signed i = 0;
    struct Dwarf_Global_Context_s *glcp = 0;
    struct Dwarf_Global_Context_s *lastglcp = 0;

    if (IS_INVALID_DBG(dbg)) {
        return;
    }
    if (!dwgl) {
        return;
    }
    for (i = 0; i < count; i++) {
        Dwarf_Global dgd = dwgl[i];

        if (!dgd) {
            continue;
        }
        /*  Avoids duplicate frees of repeated
            use of contexts (while assuming that
            all uses of a particular gl_context
            will appear next to each other. */
        glcp = dgd->gl_context;
        if (glcp && lastglcp != glcp) {
            lastglcp = glcp;
            dwarf_dealloc(dbg, glcp, glcp->pu_alloc_type);
        }
        dwarf_dealloc(dbg, dgd, dgd->gl_alloc_type);
    }
    dwarf_dealloc(dbg, dwgl, DW_DLA_LIST);
    return;
}

/*  Given a pubnames entry (or other like section entry)
    return thru the ret_name pointer
    a pointer to the string which is the entry name.  */
int
dwarf_globname(Dwarf_Global glob,
    char **ret_name,
    Dwarf_Error * error)
{
    if (glob == NULL) {
        _dwarf_error(NULL, error, DW_DLE_GLOBAL_NULL);
        return DW_DLV_ERROR;
    }

    *ret_name = (char *) (glob->gl_name);
    return DW_DLV_OK;
}

/*  Given a pubnames entry (or other like section entry)
    return thru the ret_off pointer the
    global offset of the DIE for this entry.
    The global offset is the offset within the .debug_info
    section as a whole.  */
int
dwarf_global_die_offset(Dwarf_Global global,
    Dwarf_Off * ret_off, Dwarf_Error * error)
{
    if (global == NULL) {
        _dwarf_error(NULL, error, DW_DLE_GLOBAL_NULL);
        return DW_DLV_ERROR;
    }

    if (global->gl_context == NULL) {
        _dwarf_error(NULL, error, DW_DLE_GLOBAL_CONTEXT_NULL);
        return DW_DLV_ERROR;
    }

    *ret_off = (global->gl_named_die_offset_within_cu +
        global->gl_context->pu_offset_of_cu_header);
    return DW_DLV_OK;
}

/*  Given a pubnames entry (or other like section entry)
    return thru the ret_off pointer the
    offset of the compilation unit header of the
    compilation unit the global is part of.
*/
int
dwarf_global_cu_offset(Dwarf_Global global,
    Dwarf_Off * cu_header_offset,
    Dwarf_Error * error)
{
    Dwarf_Global_Context con = 0;

    if (global == NULL) {
        _dwarf_error(NULL, error, DW_DLE_GLOBAL_NULL);
        return DW_DLV_ERROR;
    }
    con = global->gl_context;
    if (con == NULL) {
        _dwarf_error_string(NULL, error, DW_DLE_GLOBAL_CONTEXT_NULL,
            "DW_DLE_GLOBAL_CONTEXT_NULL in call of "
            "dwarf_global_cu_offset()");
        return DW_DLV_ERROR;
    }
    *cu_header_offset = con->pu_offset_of_cu_header;
    return DW_DLV_OK;
}

static void
build_off_end_msg(Dwarf_Unsigned offval,
    Dwarf_Unsigned withincr,
    Dwarf_Unsigned secsize,
    dwarfstring *m)
{
    const char *msg = "past";
    if (offval < secsize){
        msg = "too near";
    }
    dwarfstring_append_printf_u(m,"DW_DLE_OFFSET_BAD: "
        "The CU header offset of %u in a pubnames-like entry ",
        withincr);
    dwarfstring_append_printf_s(m,
        "would put us %s the end of .debug_info. "
        "No room for a DIE there... "
        "Corrupt Dwarf.",(char *)msg);
    return;
}

/*
  Give back the pubnames entry (or any other like section)
  name, symbol DIE offset, and the cu-DIE offset.

  This provides all the information that
  dwarf_globname(), dwarf_global_die_offset()
  and dwarf_global_cu_offset() do, but do it
  in one call.

  The string pointer returned thru ret_name is not
  dwarf_get_alloc()ed, so no dwarf_dealloc()
  DW_DLA_STRING should be applied to it.

*/
int
dwarf_global_name_offsets(Dwarf_Global global,
    char **ret_name,
    Dwarf_Off * die_offset,
    Dwarf_Off * cu_die_offset,
    Dwarf_Error * error)
{
    Dwarf_Global_Context con = 0;
    Dwarf_Debug dbg = 0;
    Dwarf_Off cuhdr_off = 0;

    if (global == NULL) {
        _dwarf_error(NULL, error, DW_DLE_GLOBAL_NULL);
        return DW_DLV_ERROR;
    }

    con = global->gl_context;
    if (con == NULL) {
        _dwarf_error_string(NULL, error, DW_DLE_GLOBAL_CONTEXT_NULL,
            "DW_DLE_GLOBAL_CONTEXT_NULL in call of "
            "dwarf_global_name_offsets()");
        return DW_DLV_ERROR;
    }

    cuhdr_off = con->pu_offset_of_cu_header;
    /*  The offset had better not be too close to the end. If it is,
        _dwarf_length_of_cu_header() will step off the end
        and therefore
        must not be used. 10 is a meaningless heuristic, but no CU
        header is that small so it is safe. An erroneous offset is due
        to a bug in the tool chain. A bug like this has been seen on
        IRIX with MIPSpro 7.3.1.3 and an executable > 2GB in size and
        with 2 million pubnames entries. */
#define MIN_CU_HDR_SIZE 10
    dbg = con->pu_dbg;
    CHECK_DBG(dbg,error,"dwarf_global_name_offsets()");
    /*  Cannot refer to debug_types, see p141 of
        DWARF4 Standard */
    if (dbg->de_debug_info.dss_size &&
        ((cuhdr_off + MIN_CU_HDR_SIZE) >=
        dbg->de_debug_info.dss_size)) {
        dwarfstring m;

        dwarfstring_constructor(&m);
        build_off_end_msg(cuhdr_off,cuhdr_off,
            dbg->de_debug_info.dss_size,&m);
        _dwarf_error_string(dbg, error, DW_DLE_OFFSET_BAD,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    /*  If global->gl_named_die_offset_within_cu
        is zero then this is a fake global for
        a pubnames CU with no pubnames. The offset is from the
        start of the CU header, so no die can have a zero
        offset, all valid offsets are positive numbers */
    if (die_offset) {
        if (global->gl_named_die_offset_within_cu) {
            *die_offset = global->gl_named_die_offset_within_cu +
                cuhdr_off;
        } else {
            *die_offset = 0;
        }
    }
#undef MIN_CU_HDR_SIZE
    *ret_name = (char *) global->gl_name;
    if (cu_die_offset) {
        /* Global cannot refer to debug_types */
        int cres = 0;
        Dwarf_Unsigned headerlen = 0;
        int res = _dwarf_load_debug_info(dbg, error);

        if (res != DW_DLV_OK) {
            return res;
        }
        /* We already checked to make sure enough room
            with MIN_CU_HDR_SIZE */
#if 0   /* Heuristic check for about to pass end, not usable. */
        /*  The offset had better not be too close to the end.
            If it is,
            _dwarf_length_of_cu_header() will step off the end and
            therefore must not be used. 10 is a meaningless heuristic,
            but no CU header is that small so it is safe. */
        /* Global cannot refer to debug_types */
        if ((cuhdr_off + MIN_CU_HDR_SIZE)
            >= dbg->de_debug_info.dss_size) {
            dwarfstring m;

            dwarfstring_constructor(&m);
            build_off_end_msg(cuhdr_off,cuhdr_off,
                dbg->de_debug_info.dss_size,&m);
            _dwarf_error_string(dbg, error, DW_DLE_OFFSET_BAD,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
        }
#endif /* 0 */
        cres = _dwarf_length_of_cu_header(dbg, cuhdr_off,TRUE,
            &headerlen,error);
        if (cres != DW_DLV_OK) {
            return cres;
        }
        *cu_die_offset = cuhdr_off + headerlen;
    }
    return DW_DLV_OK;
}

/*  New February 2019 from better dwarfdump printing
    of debug_pubnames and pubtypes.
    For all the Dwarf_Global records in one pubnames
    CU group exactly the same data will be returned.

*/
int
dwarf_get_globals_header(Dwarf_Global global,
    int            *category,/* DW_GL_GLOBAL for example*/
    Dwarf_Off      *pub_section_hdr_offset,
    Dwarf_Unsigned *pub_offset_size,
    Dwarf_Unsigned *pub_cu_length,
    Dwarf_Unsigned *version,
    Dwarf_Off      *info_header_offset,
    Dwarf_Unsigned *info_length,
    Dwarf_Error*   error)
{
    Dwarf_Global_Context con = 0;
    Dwarf_Debug dbg = 0;

    if (global == NULL) {
        _dwarf_error(NULL, error, DW_DLE_GLOBAL_NULL);
        return DW_DLV_ERROR;
    }
    con = global->gl_context;
    if (con == NULL) {
        _dwarf_error(NULL, error, DW_DLE_GLOBAL_CONTEXT_NULL);
        return DW_DLV_ERROR;
    }
    dbg = con->pu_dbg;
    CHECK_DBG(dbg,error,"dwarf_get_globals_header()");
    if (category) {
        *category = con->pu_global_category;
    }
    if (pub_section_hdr_offset) {
        *pub_section_hdr_offset = con->pu_pub_offset;
    }
    if (pub_offset_size) {
        *pub_offset_size = con->pu_length_size;
    }
    if (pub_cu_length) {
        *pub_cu_length = con->pu_length;
    }
    if (version) {
        *version = con->pu_version;
    }
    if (info_header_offset) {
        *info_header_offset = con->pu_offset_of_cu_header;
    }
    if (info_length) {
        *info_length = con->pu_info_length;
    }
    return DW_DLV_OK;
}

/*  We have the offset to a CU header.
    Return thru outFileOffset the offset of the CU DIE.

    New June, 2001.
    Used by SGI IRIX debuggers.
    No error used to be possible.
    As of May 2016 an error is possible if the DWARF is
    corrupted! (IRIX debuggers are no longer built ...)

    See also dwarf_CU_dieoffset_given_die().

    This is assumed to never apply to data in .debug_types, it
    only refers to .debug_info.

*/

/* ARGSUSED */
/*  The following version new in October 2011, does allow finding
    the offset if one knows whether debug_info or debug_types
    or any .debug_info type including the DWARF5 flavors.

    It indirectly calls _dwarf_length_of_cu_header()
    which knows all the varieties of header.  */
int
dwarf_get_cu_die_offset_given_cu_header_offset_b(Dwarf_Debug dbg,
    Dwarf_Off in_cu_header_offset,
    Dwarf_Bool is_info,
    Dwarf_Off * out_cu_die_offset,
    Dwarf_Error * error)
{
    Dwarf_Off headerlen = 0;
    int cres = 0;

    if (IS_INVALID_DBG(dbg)) {
        _dwarf_error_string(NULL, error, DW_DLE_DBG_NULL,
            "DW_DLE_DBG_NULL: "
            "calling dwarf_get_cu_die_offset_given"
            "cu_header_offset_b Dwarf_Debug is"
            "either null or it is"
            "a stale Dwarf_Debug pointer");
        return DW_DLV_ERROR;
    }
    cres = _dwarf_length_of_cu_header(dbg,
        in_cu_header_offset,is_info, &headerlen,error);
    if (cres != DW_DLV_OK) {
        return cres;
    }
    *out_cu_die_offset = in_cu_header_offset + headerlen;
    return DW_DLV_OK;
}
/*  dwarf_CU_dieoffset_given_die returns
    the global debug_info section offset of the CU die
    that is the CU containing the given (passed-in) die.
    This information makes it possible for a consumer to
    find and print context information for any die.

    Use dwarf_offdie_b() passing in the offset this returns
    to get a die pointer to the CU die.  */
int
dwarf_CU_dieoffset_given_die(Dwarf_Die die,
    Dwarf_Off*       return_offset,
    Dwarf_Error*     error)
{
    Dwarf_Off  dieoff = 0;
    Dwarf_CU_Context cucontext = 0;

    CHECK_DIE(die, DW_DLV_ERROR);
    cucontext = die->di_cu_context;
    dieoff =  cucontext->cc_debug_offset;
    /*  The following call cannot fail, so no error check. */
    dwarf_get_cu_die_offset_given_cu_header_offset_b(
        cucontext->cc_dbg, dieoff,
        die->di_is_info, return_offset,error);
    return DW_DLV_OK;
}

/*  Just sets a flag in dbg record if it can. */
int
dwarf_return_empty_pubnames(Dwarf_Debug dbg, int flag)
{
    if (IS_INVALID_DBG(dbg)) {
        return DW_DLV_OK;
    }
    if (flag && flag != 1) {
        return DW_DLV_OK;
    }
    dbg->de_return_empty_pubnames = (unsigned char)flag;
    return DW_DLV_OK;
}

Dwarf_Half
dwarf_global_tag_number(Dwarf_Global dw_global)
{
    if (!dw_global) {
        return 0;
    }
    return dw_global->gl_tag;
}
