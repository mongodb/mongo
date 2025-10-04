/*
Copyright (c) 2020, David Anderson
All rights reserved.

Redistribution and use in source and binary forms, with
or without modification, are permitted provided that the
following conditions are met:

    Redistributions of source code must retain the above
    copyright notice, this list of conditions and the following
    disclaimer.

    Redistributions in binary form must reproduce the above
    copyright notice, this list of conditions and the following
    disclaimer in the documentation and/or other materials
    provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <config.h>

#include <stdlib.h> /* free() malloc() */
#include <stdio.h> /* printf */
#include <string.h> /* memset() */

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
#include "dwarf_rnglists.h"

#undef DEBUG_RNGLIST

#define SIZEOFT8 1
#define SIZEOFT16 2
#define SIZEOFT32 4
#define SIZEOFT64 8

#ifdef DEBUG_RNGLIST /* dump_bytes */
static void
dump_bytes(const char *msg,Dwarf_Small * start, long len)
{
    Dwarf_Small *end = start + len;
    Dwarf_Small *cur = start;
    printf("%s (0x%lx) ",msg,(unsigned long)start);
    for (; cur < end; cur++) {
        printf("%02x", *cur);
    }
    printf("\n");
    fflush(stdout);
}
#endif /*0*/
#if 0 /* dump_rh */
static void
dump_rh(const char *msg,
    int line,
    struct Dwarf_Rnglists_Head_s *head)
{
    printf("Rnglists_Head: %s  line %d\n",
        msg,line);
    printf("  count of entries: %"
        DW_PR_DUu
        "  (0x%" DW_PR_DUx ")\n",
        head->rh_count,head->rh_count);
    printf("  rh__offset_size  : %p\n",
        (void *) head->rh_offset_size);
    printf("  rh_rnglists     : %p\n",(void *) head->rh_rnglists);
    printf("  rh_index    : %lu\n",
        (unsigned long)head->rh_index);
    printf("  rh_first    : %lu\n",
        (unsigned long)head->rh_first);
    printf("  rh_last     :  %lu\n",
        (unsigned long)head->rh_last);
    printf("  rh_bytes total  : %lu (0x%lx)\n",
        (unsigned long)head->rh_bytes_total,
        (unsigned long)head->rh_bytes_total);
    printf("  CU Context      : %p",(void *)head->rh_context);
    printf("  Rnglists Context:  %p",(void *)head->rh_localcontext);
}
#endif
#if 0 /* debug print struct */
static void
dump_rc(const char *msg,
    int line,
    Dwarf_Rnglists_Context rc)
{
    printf("Rnglists_Context %s line %d index %lu\n", msg,
        line,
        (unsigned long)rc->rc_index);
    printf(" rc_header_offset  0x%lx\n",
        (unsigned long)rc->rc_header_offset);
    printf(" rc_length  0x%lx\n",
        (unsigned long)rc->rc_length);
    printf(" rc_offset_size  0x%lu\n",
        (unsigned long)rc->rc_offset_size);
    printf(" rc_version  0x%lu\n",
        (unsigned long)rc->rc_version);
    printf(" rc_address_size  0x%lu\n",
        (unsigned long)rc->rc_address_size);
    printf(" rc_address_size  0x%lu\n",
        (unsigned long)rc->rc_address_size);
    printf(" rc_offset_entry_count  0x%lu\n",
        (unsigned long)rc->rc_offset_entry_count);
    printf(" rc_offsets_off_in_sect  0x%lu\n",
        (unsigned long)rc->rc_offsets_off_in_sect);
    printf(" rc_first_rnglist_offset  0x%lx\n",
        (unsigned long)rc->rc_first_rnglist_offset);
    printf(" rc_past_last_rnglist_offset  0x%lx\n",
        (unsigned long)rc->rc_past_last_rnglist_offset);
}
#endif

static void
free_rnglists_context(Dwarf_Rnglists_Context cx)
{
    if (cx) {
        free(cx->rc_offset_value_array);
        cx->rc_offset_value_array = 0;
        cx->rc_magic = 0;
        free(cx);
    }
}

/*  Used in case of error reading the
    rnglists headers (not referring to Dwarf_Rnglists_Head
    here), to clean up. */
static void
free_rnglists_chain(Dwarf_Debug dbg, Dwarf_Chain head)
{
    Dwarf_Chain cur = head;
    Dwarf_Chain next = 0;

    if (!head || IS_INVALID_DBG(dbg)) {
        return;
    }
    for ( ;cur; cur = next) {
        next = cur->ch_next;
        if (cur->ch_item) {
            /* ch_item is Dwarf_Rnglists_Context */
            Dwarf_Rnglists_Context cx =
                (Dwarf_Rnglists_Context)cur->ch_item;
            free_rnglists_context(cx);
            cur->ch_item = 0;
            dwarf_dealloc(dbg,cur,DW_DLA_CHAIN);
        }
    }
}
/*  See also read_single_lle_entry() for similar code */
static int
read_single_rle_entry(Dwarf_Debug dbg,
    Dwarf_Small   *data,
    Dwarf_Unsigned dataoffset,
    Dwarf_Small   *enddata,
    unsigned       address_size,
    unsigned       *bytes_count_out,
    unsigned       *entry_kind,
    Dwarf_Unsigned *entry_operand1,
    Dwarf_Unsigned *entry_operand2,
    Dwarf_Error* error)
{
    Dwarf_Unsigned count = 0;
    Dwarf_Unsigned leblen = 0;
    unsigned code = 0;
    Dwarf_Unsigned val1 = 0;
    Dwarf_Unsigned val2 = 0;
    Dwarf_Small *  startdata = 0;

    if (data >= enddata) {
        _dwarf_error_string(dbg,error,DW_DLE_RNGLISTS_ERROR,
            "DW_DLE_RNGLISTS_ERROR: "
            "An rle entry begins past the end of "
            "its allowed space. Corrupt DWARF.");
        return DW_DLV_ERROR;
    }
    startdata = data;
    code = *data;
    ++data;
    ++count;
    switch(code) {
    case DW_RLE_end_of_list: break;
    case DW_RLE_base_addressx:{
        DECODE_LEB128_UWORD_LEN_CK(data,val1,leblen,
            dbg,error,enddata);
        count += leblen;
        }
        break;
    case DW_RLE_startx_endx:
    case DW_RLE_startx_length:
    case DW_RLE_offset_pair: {
        DECODE_LEB128_UWORD_LEN_CK(data,val1,leblen,
            dbg,error,enddata);
        count += leblen;
        DECODE_LEB128_UWORD_LEN_CK(data,val2,leblen,
            dbg,error,enddata);
        count += leblen;
        }
        break;
    case DW_RLE_base_address: {
        READ_UNALIGNED_CK(dbg,val1, Dwarf_Unsigned,
            data,address_size,error,enddata);
        data += address_size;
        count += address_size;
        }
        break;
    case DW_RLE_start_end: {
        READ_UNALIGNED_CK(dbg,val1, Dwarf_Unsigned,
            data,address_size,error,enddata);
        data += address_size;
        count += address_size;
        READ_UNALIGNED_CK(dbg,val2, Dwarf_Unsigned,
            data,address_size,error,enddata);
        data += address_size;
        count += address_size;
        }
        break;
    case DW_RLE_start_length: {
        READ_UNALIGNED_CK(dbg,val1, Dwarf_Unsigned,
            data,address_size,error,enddata);
        data += address_size;
        count += address_size;
        DECODE_LEB128_UWORD_LEN_CK(data,val2,leblen,
            dbg,error,enddata);
        count += leblen;
        }
        break;
    default: {
        dwarfstring m;

        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            "DW_DLE_RNGLISTS_ERROR: "
            "The rangelists entry at .debug_rnglists[.dwo]"
            " offset 0x%x" ,dataoffset);
        dwarfstring_append_printf_u(&m,
            " has code 0x%x which is unknown",code);
        _dwarf_error_string(dbg,error,DW_DLE_RNGLISTS_ERROR,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
        }
        break;
    }
    {
        /*  We want to avoid overflow in additions, and
            the overall section size is a reasonable check
            on count.  The sequence of tests is to
            preserve a testing baseline:
            baselines/hongg2024-02-18-m.base
            otherwise we would test against sectionsize first.*/
        Dwarf_Unsigned sectionsize = 0;

        if (data > enddata || data < startdata ) {
            /*  Corrupt data being read. */
            _dwarf_error_string(dbg,error,DW_DLE_RNGLISTS_ERROR,
                "DW_DLE_RNGLISTS_ERROR: "
                "The end of an rle entry is past the end "
                "of its allowed space");
            return DW_DLV_ERROR;
        }
        sectionsize = dbg->de_debug_rnglists.dss_size;
        if (count > sectionsize) {
            /*  Corrupt data being read. */
            _dwarf_error_string(dbg,error,DW_DLE_RNGLISTS_ERROR,
                "DW_DLE_RNGLISTS_ERROR: "
                "The number of bytes in a single "
                "rnglist entry is "
                "too large to be reasonable");
            return DW_DLV_ERROR;
        }
    }

    *bytes_count_out = (unsigned int)count;
    *entry_kind = code;
    *entry_operand1 = val1;
    *entry_operand2 = val2;
    return DW_DLV_OK;
}

/*  Reads the header. Determines the
    various offsets, including offset
    of the next header. ALlocates
    an array pointed to by rc_offset_value_array
    here. */
int
_dwarf_internal_read_rnglists_header(Dwarf_Debug dbg,
    Dwarf_Bool build_offset_array,
    Dwarf_Unsigned contextnum,
    Dwarf_Unsigned sectionlength,
    Dwarf_Small *data,
    Dwarf_Small *end_data,
    Dwarf_Unsigned starting_offset,
    Dwarf_Rnglists_Context  buildhere,
    Dwarf_Unsigned *next_offset,
    Dwarf_Error *error)
{
    Dwarf_Small   *startdata = data;
    Dwarf_Unsigned arealen = 0;
    int            offset_size = 0;
    int            exten_size = 0;
    Dwarf_Unsigned version = 0;
    unsigned       address_size = 0;
    unsigned       segment_selector_size=  0;
    Dwarf_Unsigned offset_entry_count = 0;
    Dwarf_Unsigned localoff = 0;
    Dwarf_Unsigned lists_byte_len = 0;
    Dwarf_Unsigned secsize_dbg = 0;
    Dwarf_Unsigned sum_size = 0;

    secsize_dbg = dbg->de_debug_rnglists.dss_size;
    /*  Sanity checks */
    if (sectionlength > secsize_dbg) {
        dwarfstring m;
        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            "DW_DLE_RNGLISTS_ERROR: "
            " section_length argument (%lu) mismatch vs.",
            sectionlength);
        dwarfstring_append_printf_u(&m,
            ".debug_rnglists[.dwo]"
            " section length",secsize_dbg);
        _dwarf_error_string(dbg,error,DW_DLE_RNGLISTS_ERROR,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    buildhere->rc_startaddr = data;
    READ_AREA_LENGTH_CK(dbg,arealen,Dwarf_Unsigned,
        data,offset_size,exten_size,
        error,
        sectionlength,end_data);
    localoff = offset_size+exten_size;
    /* local off is bytes including length field */
    sum_size = arealen+localoff;
    if (arealen > sectionlength ||
        sum_size < arealen ||
        sum_size > sectionlength) {
        dwarfstring m;
        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            "DW_DLE_RNGLISTS_ERROR: A .debug_rnglists[.dwo] "
            "area size of 0x%x ",arealen);
        dwarfstring_append_printf_u(&m,
            "at offset 0x%x ",starting_offset);
        dwarfstring_append_printf_u(&m,
            "is larger than the entire section size of "
            "0x%x. Corrupt DWARF.",sectionlength);
        _dwarf_error_string(dbg,error,DW_DLE_RNGLISTS_ERROR,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    buildhere->rc_length = sum_size;
    buildhere->rc_dbg = dbg;
    buildhere->rc_index = contextnum;
    buildhere->rc_header_offset = starting_offset;
    buildhere->rc_offset_size = offset_size;
    buildhere->rc_extension_size = exten_size;
    buildhere->rc_magic = RNGLISTS_MAGIC;
    READ_UNALIGNED_CK(dbg,version,Dwarf_Unsigned,data,
        SIZEOFT16,error,end_data);
    if (version != DW_CU_VERSION5) {
        dwarfstring m;
        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            "DW_DLE_RNGLISTS_ERROR: The version should be 5 "
            "but we find %u instead.",version);
        _dwarf_error_string(dbg,error,DW_DLE_RNGLISTS_ERROR,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    buildhere->rc_version = (Dwarf_Half)version;
    data += SIZEOFT16;
    localoff += SIZEOFT16;
    READ_UNALIGNED_CK(dbg,address_size,unsigned,data,
        SIZEOFT8,error,end_data);
    if (address_size != 4 && address_size != 8 &&
        address_size != 2) {
        dwarfstring m;
        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            " DW_DLE_RNGLISTS_ERROR: .debug_rnglists[.dwo] "
            "The address size "
            "of %u is not supported.",address_size);
        _dwarf_error_string(dbg,error,DW_DLE_RNGLISTS_ERROR,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    buildhere->rc_address_size = address_size;
    localoff++;
    data++;

    READ_UNALIGNED_CK(dbg,segment_selector_size,unsigned,data,
        SIZEOFT8,error,end_data);
    buildhere->rc_segment_selector_size = segment_selector_size;
    data++;
    localoff++;
    if (segment_selector_size) {
        dwarfstring m;
        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            " DW_DLE_RNGLISTS_ERROR: .debug_rnglists[.dwo]"
            " The segment selector size "
            "of %u is not supported.",address_size);
        _dwarf_error_string(dbg,error,DW_DLE_RNGLISTS_ERROR,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    if ((starting_offset+localoff+SIZEOFT32) > secsize_dbg) {
        dwarfstring m;
        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            " DW_DLE_RNGLISTS_ERROR: .debug_rnglists[.dwo]"
            " Header runs off the end of the section "
            " with offset %u",starting_offset+localoff+SIZEOFT32);
        _dwarf_error_string(dbg,error,DW_DLE_RNGLISTS_ERROR,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }

    READ_UNALIGNED_CK(dbg,offset_entry_count,Dwarf_Unsigned,data,
        SIZEOFT32,error,end_data);
    buildhere->rc_offset_entry_count = offset_entry_count;
    data += SIZEOFT32;
    localoff+= SIZEOFT32;
    if (offset_entry_count > (arealen/offset_size)) {
        dwarfstring m;
        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            " DW_DLE_RNGLISTS_ERROR: .debug_rnglists[.dwo]"
            " offset entry size impossibly large "
            " with size 0x%x",
            offset_entry_count*offset_size);
        _dwarf_error_string(dbg,error,DW_DLE_RNGLISTS_ERROR,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    if (offset_entry_count){
        buildhere->rc_offsets_array = data;
        lists_byte_len += offset_size*offset_entry_count;
        if (build_offset_array) {
            Dwarf_Unsigned tabentrynum = 0;

            buildhere->rc_offset_value_array = (Dwarf_Unsigned *)
                calloc(offset_entry_count, sizeof(Dwarf_Unsigned));
            if (!buildhere->rc_offset_value_array) {
                _dwarf_error_string(dbg,error, DW_DLE_ALLOC_FAIL,
                    "dbg,DW_DLE_ALLOC_CAIL: "
                    " The debug_rnglists offset table "
                    "cannot be allocated.");
                return DW_DLV_ERROR;
            }
            for (tabentrynum = 0 ; tabentrynum < offset_entry_count;
                data += offset_size,++tabentrynum ) {
                Dwarf_Unsigned entry = 0;
                int res = 0;

                res = _dwarf_read_unaligned_ck_wrapper(dbg,
                    &entry,data,offset_size,end_data,error);
                if (res != DW_DLV_OK) {
                    free(buildhere->rc_offset_value_array);
                    buildhere->rc_offset_value_array = 0;
                    return res;
                }
                buildhere->rc_offset_value_array[tabentrynum] =
                    entry;
            }
        }
    } /* else no offset table */
    if (offset_entry_count >= secsize_dbg ||
        lists_byte_len >= secsize_dbg) {
        dwarfstring m;

        free(buildhere->rc_offset_value_array);
        buildhere->rc_offset_value_array = 0;
        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            " DW_DLE_RNGLISTS_ERROR: .debug_rnglists[.dwo]"
            " offset entry count"
            " of %u is clearly impossible. Corrupt data",
            offset_entry_count);
        _dwarf_error_string(dbg,error,DW_DLE_RNGLISTS_ERROR,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    data += lists_byte_len;
    buildhere->rc_offsets_off_in_sect = starting_offset+localoff;
    localoff += lists_byte_len;
    if (localoff > buildhere->rc_length) {
        dwarfstring m;

        free(buildhere->rc_offset_value_array);
        buildhere->rc_offset_value_array = 0;

        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            " DW_DLE_RNGLISTS_ERROR: .debug_rnglists[.dwo]"
            " length of rnglists header too large at"
            " of %u is clearly impossible. Corrupt data",
            localoff);
        _dwarf_error_string(dbg,error,DW_DLE_RNGLISTS_ERROR,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    buildhere->rc_first_rnglist_offset = starting_offset+localoff;
    buildhere->rc_rnglists_header = startdata;
    buildhere->rc_endaddr = startdata +buildhere->rc_length;
    if (buildhere->rc_endaddr > end_data) {
        dwarfstring m;

        free(buildhere->rc_offset_value_array);
        buildhere->rc_offset_value_array = 0;
        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            " DW_DLE_RNGLISTS_ERROR: .debug_rnglists[.dwo]"
            " length of rnglists header (%u) "
            "runs off end of section. Corrupt data",
            buildhere->rc_length);
        _dwarf_error_string(dbg,error,DW_DLE_RNGLISTS_ERROR,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    buildhere->rc_past_last_rnglist_offset =
        buildhere->rc_header_offset +buildhere->rc_length;
    if (buildhere->rc_past_last_rnglist_offset > secsize_dbg) {
        free(buildhere->rc_offset_value_array);
        buildhere->rc_offset_value_array = 0;
        _dwarf_error_string(dbg,error,DW_DLE_RNGLISTS_ERROR,
            "Impossible end of rnglist");
        return DW_DLV_ERROR;
    }
    *next_offset =  buildhere->rc_past_last_rnglist_offset;
    return DW_DLV_OK;
}

/*  We return a pointer to an array of contexts
    (not context pointers) through *cxt if
    we succeed and are returning DW_DLV_OK.
    We never return DW_DLV_NO_ENTRY here. */
static int
internal_load_rnglists_contexts(Dwarf_Debug dbg,
    Dwarf_Rnglists_Context **cxt,
    Dwarf_Unsigned *count,
    Dwarf_Error *error)
{
    Dwarf_Unsigned offset = 0;
    Dwarf_Unsigned nextoffset = 0;
    Dwarf_Small  * data = dbg->de_debug_rnglists.dss_data;
    Dwarf_Unsigned section_size = dbg->de_debug_rnglists.dss_size;
    Dwarf_Small  * startdata = data;
    Dwarf_Small  * end_data = data +section_size;
    Dwarf_Chain    curr_chain = 0;
    Dwarf_Chain    head_chain = 0;
    Dwarf_Chain  * plast = &head_chain;
    int            res = 0;
    Dwarf_Unsigned i = 0;
    Dwarf_Unsigned chainlength = 0;
    Dwarf_Rnglists_Context *fullarray = 0;

    for (i = 0 ; data < end_data ; ++i,data = startdata+nextoffset) {
        Dwarf_Rnglists_Context newcontext = 0;

        /* sizeof the context struct, not sizeof a pointer */
        newcontext = malloc(sizeof(*newcontext));
        if (!newcontext) {
            free_rnglists_chain(dbg,head_chain);
            _dwarf_error_string(dbg,error,
                DW_DLE_ALLOC_FAIL,
                "DW_DLE_ALLOC_FAIL: Allocation of "
                "Rnglists_Context failed");
            return DW_DLV_ERROR;
        }
        memset(newcontext,0,sizeof(*newcontext));
        newcontext->rc_magic = RNGLISTS_MAGIC;
        res = _dwarf_internal_read_rnglists_header(dbg, TRUE,
            chainlength,
            section_size,
            data,end_data,offset,
            newcontext,&nextoffset,error);
        if (res == DW_DLV_ERROR) {
            free_rnglists_context(newcontext);
            newcontext =  0;
            free_rnglists_chain(dbg,head_chain);
            return res;
        }
        newcontext->rc_magic = RNGLISTS_MAGIC;
        curr_chain = (Dwarf_Chain)
            _dwarf_get_alloc(dbg, DW_DLA_CHAIN, 1);
        if (curr_chain == NULL) {
            _dwarf_error_string(dbg, error, DW_DLE_ALLOC_FAIL,
                "DW_DLE_ALLOC_FAIL: allocating Rnglists_Context"
                " chain entry");
            free_rnglists_context(newcontext);
            newcontext =  0;
            free_rnglists_chain(dbg,head_chain);
            return DW_DLV_ERROR;
        }
        curr_chain->ch_item = newcontext;
        ++chainlength;
        (*plast) = curr_chain;
        plast = &(curr_chain->ch_next);
        offset = nextoffset;
        newcontext = 0;
    }
    fullarray= (Dwarf_Rnglists_Context *)malloc(
        chainlength *sizeof(Dwarf_Rnglists_Context /*pointer*/));
    if (!fullarray) {
        free_rnglists_chain(dbg,head_chain);
        _dwarf_error_string(dbg,error,
            DW_DLE_ALLOC_FAIL,"Allocation of "
            "Rnglists_Context pointer array failed");
        return DW_DLV_ERROR;
    }
    curr_chain = head_chain;
    for (i = 0; i < chainlength; ++i) {
        Dwarf_Chain prev = 0;
        Dwarf_Rnglists_Context c =
            (Dwarf_Rnglists_Context)curr_chain->ch_item;
        fullarray[i] = c;
        curr_chain->ch_item = 0;
        prev = curr_chain;
        curr_chain = curr_chain->ch_next;
        dwarf_dealloc(dbg, prev, DW_DLA_CHAIN);
    }
    /*  ASSERT: the chain is entirely alloc'd
        and the array of pointers points to
        individually malloc'd Dwarf_Rnglists_Context_s */
    *cxt = fullarray;
    *count = chainlength;
    return DW_DLV_OK;
}

/*  Used by dwarfdump to print raw rnglists data.
    Loads all the .debug_rnglists[.dwo]  headers and
    returns DW_DLV_NO_ENTRY if the section
    is missing or empty.
    Intended to be done quite early and
    done exactly once.
    Harmless to do more than once.
    With DW_DLV_OK it returns the number of
    rnglists headers in the section through
    rnglists_count. */
int dwarf_load_rnglists(
    Dwarf_Debug dbg,
    Dwarf_Unsigned *rnglists_count,
    Dwarf_Error *error)
{
    int res = DW_DLV_ERROR;
    Dwarf_Rnglists_Context *cxt = 0;
    Dwarf_Unsigned count = 0;

    CHECK_DBG(dbg,error,"dwarf_load_rnglists");
    if (dbg->de_rnglists_context) {
        if (rnglists_count) {
            *rnglists_count = dbg->de_rnglists_context_count;
    }
    if (rnglists_count) {
        }
        return DW_DLV_OK;
    }
    if (!dbg->de_debug_rnglists.dss_size) {
        /* nothing there. */
        return DW_DLV_NO_ENTRY;
    }
    if (!dbg->de_debug_rnglists.dss_data) {
        res = _dwarf_load_section(dbg, &dbg->de_debug_rnglists,
            error);
        if (res != DW_DLV_OK) {
            return res;
        }
    }
    res = internal_load_rnglists_contexts(dbg,&cxt,&count,error);
    if (res == DW_DLV_ERROR) {
        return res;
    }
    dbg->de_rnglists_context = cxt;
    dbg->de_rnglists_context_count = count;
    if (rnglists_count) {
        *rnglists_count = count;
    }
    return DW_DLV_OK;
}

/*  Frees the memory in use in all rnglists contexts.
    Done by dwarf_finish()  */
void
_dwarf_dealloc_rnglists_context(Dwarf_Debug dbg)
{
    Dwarf_Unsigned i = 0;
    Dwarf_Rnglists_Context * rngcon = 0;

    if (IS_INVALID_DBG(dbg)) {
        return;
    }
    if (!dbg->de_rnglists_context) {
        return;
    }
    rngcon = dbg->de_rnglists_context;
    for ( ; i < dbg->de_rnglists_context_count; ++i) {
        Dwarf_Rnglists_Context con = rngcon[i];
        free_rnglists_context(con);
        rngcon[i] = 0;
    }
    free(dbg->de_rnglists_context);
    dbg->de_rnglists_context = 0;
    dbg->de_rnglists_context_count = 0;
}

/*  Used by dwarfdump to print raw rnglists data. */
int
dwarf_get_rnglist_offset_index_value(
    Dwarf_Debug dbg,
    Dwarf_Unsigned context_index,
    Dwarf_Unsigned offsetentry_index,
    Dwarf_Unsigned * offset_value_out,
    Dwarf_Unsigned * global_offset_value_out,
    Dwarf_Error *error)
{
    Dwarf_Rnglists_Context con = 0;
    unsigned offset_len = 0;
    Dwarf_Small *offsetptr = 0;
    Dwarf_Unsigned targetoffset = 0;
    Dwarf_Unsigned localoffset = 0;
    Dwarf_Unsigned globaloffset = 0;
    Dwarf_Unsigned section_size = 0;

    CHECK_DBG(dbg,error,"dwarf_get_rnglist_offset_index_value()");
    if (!dbg->de_rnglists_context) {
        return DW_DLV_NO_ENTRY;
    }

    if (!dbg->de_rnglists_context_count) {
        return DW_DLV_NO_ENTRY;
    }
    if (context_index >= dbg->de_rnglists_context_count) {
        return DW_DLV_NO_ENTRY;
    }
    con = dbg->de_rnglists_context[context_index];
    if (con->rc_magic != RNGLISTS_MAGIC) {
        _dwarf_error_string(NULL, error,DW_DLE_DBG_NULL,
            "DW_DLE_DBG_NULL "
            "rnglists context magic wrong "
            "not RNGLISTS_MAGIC");
        return DW_DLV_ERROR;
    }
    if (offsetentry_index >= con->rc_offset_entry_count) {
        return DW_DLV_NO_ENTRY;
    }
    offset_len  = con->rc_offset_size;
    localoffset = offsetentry_index*offset_len;
    if (localoffset >= con->rc_length) {
        _dwarf_error_string(dbg,error, DW_DLE_RLE_ERROR,
            "DW_DLE_RLE_ERROR: a .debug_rnglists[.dwo] "
            "section offset "
            "is greater than this rnglists table length");
        return DW_DLV_ERROR;
    }
    if ((con->rc_offsets_off_in_sect +localoffset +
        offset_len) >
        con->rc_past_last_rnglist_offset) {
        dwarfstring m;

        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            "DW_DLE_RNGLISTS_ERROR "
            "dwarf_get_rnglist_offset_index_value() "
            " Offset for index %u is too large. ",
            offsetentry_index);
        _dwarf_error_string(dbg, error,DW_DLE_RNGLISTS_ERROR,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;

    }
    offsetptr   = con->rc_offsets_array + localoffset;
    READ_UNALIGNED_CK(dbg,targetoffset,Dwarf_Unsigned,
        offsetptr,
        offset_len,error,con->rc_endaddr);
    globaloffset =  con->rc_offsets_off_in_sect;
    section_size = dbg->de_debug_rnglists.dss_size;
    if (globaloffset >= section_size) {
        _dwarf_error_string(dbg,error,DW_DLE_RNGLISTS_ERROR,
            "DW_DLE_RNGLISTS_ERROR: "
            "The offset of a rnglists entry is past "
            "its allowed space");
        return DW_DLV_ERROR;
    }
    if (offset_value_out) {
        *offset_value_out = targetoffset;
    }
    if (global_offset_value_out) {
        *global_offset_value_out = globaloffset;
    }
    return DW_DLV_OK;
}

/*  Used by dwarfdump to print basic data from the
    data generated to look at a specific rangelist
    as returned by  dwarf_rnglists_index_get_rle_head()
    or dwarf_rnglists_offset_get_rle_head. */
int dwarf_get_rnglist_head_basics(
    Dwarf_Rnglists_Head head,
    Dwarf_Unsigned * rle_count,
    Dwarf_Unsigned * rle_version,
    Dwarf_Unsigned * rnglists_index_returned,
    Dwarf_Unsigned * bytes_total_in_rle,
    Dwarf_Half     * offset_size,
    Dwarf_Half     * address_size,
    Dwarf_Half     * segment_selector_size,
    Dwarf_Unsigned * overall_offset_of_this_context,
    Dwarf_Unsigned * total_length_of_this_context,
    Dwarf_Unsigned * offset_table_offset,
    Dwarf_Unsigned * offset_table_entrycount,
    Dwarf_Bool     * rnglists_base_present,
    Dwarf_Unsigned * rnglists_base,
    Dwarf_Bool     * rnglists_base_address_present,
    Dwarf_Unsigned * rnglists_base_address,
    Dwarf_Bool     * rnglists_debug_addr_base_present,
    Dwarf_Unsigned * rnglists_debug_addr_base,
    Dwarf_Error *error)
{
    Dwarf_Rnglists_Context rngcontext = 0;

    if (!head || (!head->rh_dbg) ||
        head->rh_magic != RNGLISTS_MAGIC) {
        _dwarf_error_string(NULL, error,DW_DLE_DBG_NULL,
            "DW_DLE_DBG_NULL "
            "NULL or invalid Dwarf_Rnglists_Head "
            "argument passed to "
            "dwarf_get_rnglist_head_basics()");
        return DW_DLV_ERROR;
    }
    *rle_count = head->rh_count;
    *rle_version = head->rh_version;
    *rnglists_index_returned = head->rh_index;
    *bytes_total_in_rle = head->rh_bytes_total;
    *offset_size = (Dwarf_Half)head->rh_offset_size;
    *address_size = (Dwarf_Half)head->rh_address_size;
    *segment_selector_size =
        (Dwarf_Half)head->rh_segment_selector_size;
    rngcontext = head->rh_localcontext;
    if (rngcontext) {
        *overall_offset_of_this_context =
            rngcontext->rc_header_offset;
        *total_length_of_this_context = rngcontext->rc_length;
        *offset_table_offset = rngcontext->rc_offsets_off_in_sect;
        *offset_table_entrycount = rngcontext->rc_offset_entry_count;
    }
    *rnglists_base_present = head->rh_at_rnglists_base_present;
    *rnglists_base= head->rh_at_rnglists_base;

    *rnglists_base_address_present = head->rh_cu_base_address_present;
    *rnglists_base_address= head->rh_cu_base_address;

    *rnglists_debug_addr_base_present =
        head->rh_cu_addr_base_offset_present;
    *rnglists_debug_addr_base  = head->rh_cu_addr_base_offset;
    return DW_DLV_OK;
}

/*  Used by dwarfdump to print raw rnglists data.
    Enables printing of details about the Range List Table
    Headers, one header per call. Index starting at 0.
    Returns DW_DLV_NO_ENTRY if index is too high for the table.
    A .debug_rnglists section may contain any number
    of Range List Table Headers with their details.  */
int dwarf_get_rnglist_context_basics(
    Dwarf_Debug dbg,
    Dwarf_Unsigned context_index,
    Dwarf_Unsigned * header_offset,
    Dwarf_Small    * offset_size,
    Dwarf_Small    * extension_size,
    unsigned       * version, /* 5 */
    Dwarf_Small    * address_size,
    Dwarf_Small    * segment_selector_size,
    Dwarf_Unsigned * offset_entry_count,
    Dwarf_Unsigned * offset_of_offset_array,
    Dwarf_Unsigned * offset_of_first_rangeentry,
    Dwarf_Unsigned * offset_past_last_rangeentry,
    Dwarf_Error *error)
{
    Dwarf_Rnglists_Context con = 0;

    CHECK_DBG(dbg,error,"dwarf_get_rnglist_context_basics()");
    if (!dbg->de_rnglists_context_count) {
        return DW_DLV_NO_ENTRY;
    }
    if (context_index >= dbg->de_rnglists_context_count) {
        return DW_DLV_NO_ENTRY;
    }
    con = dbg->de_rnglists_context[context_index];
    if (con->rc_magic != RNGLISTS_MAGIC) {
        _dwarf_error_string(NULL, error,DW_DLE_DBG_NULL,
            "DW_DLE_DBG_NULL "
            "rnglists context "
            "not RNGLISTS_MAGIC "
            "in dwarf_get_rnglist_context_basics()");
        return DW_DLV_ERROR;
    }
    if (header_offset) {
        *header_offset = con->rc_header_offset;
    }
    if (offset_size) {
        *offset_size = con->rc_offset_size;
    }
    if (extension_size) {
        *extension_size = con->rc_extension_size;
    }
    if (version) {
        *version = con->rc_version;
    }
    if (address_size) {
        *address_size = con->rc_address_size;
    }
    if (segment_selector_size) {
        *segment_selector_size = con->rc_segment_selector_size;
    }
    if (offset_entry_count) {
        *offset_entry_count = con->rc_offset_entry_count;
    }
    if (offset_of_offset_array) {
        *offset_of_offset_array = con->rc_offsets_off_in_sect;
    }
    if (offset_of_first_rangeentry) {
        *offset_of_first_rangeentry = con->rc_first_rnglist_offset;
    }
    if (offset_past_last_rangeentry) {
        *offset_past_last_rangeentry =
            con->rc_past_last_rnglist_offset;
    }
    return DW_DLV_OK;
}

/*  Used by dwarfdump to print raw rnglists data.
    entry offset is offset_of_first_rangeentry.
    Stop when the returned *next_entry_offset
    is == offset_past_last_rangentry (from
    dwarf_get_rnglist_context_plus).
    This only makes sense within those ranges.
    This retrieves raw detail from the section,
    no base values or anything are added.
    So this returns raw individual entries
    for a single rnglist header, meaning a
    a single Dwarf_Rnglists_Context.  */
int dwarf_get_rnglist_rle(
    Dwarf_Debug dbg,
    Dwarf_Unsigned contextnumber,
    Dwarf_Unsigned entry_offset,
    Dwarf_Unsigned endoffset,
    unsigned *entrylen,
    unsigned *entry_kind,
    Dwarf_Unsigned *entry_operand1,
    Dwarf_Unsigned *entry_operand2,
    Dwarf_Error *error)
{
    Dwarf_Rnglists_Context con = 0;
    Dwarf_Small *data = 0;
    Dwarf_Small *enddata = 0;
    int res = 0;
    unsigned address_size = 0;
    Dwarf_Unsigned secsize = 0;

    CHECK_DBG(dbg,error,"dwarf_get_rnglist_rle()");
    secsize = dbg->de_debug_rnglists.dss_size;
    if (!dbg->de_rnglists_context_count) {
        return DW_DLV_NO_ENTRY;
    }
    if (!dbg->de_rnglists_context_count) {
        return DW_DLV_NO_ENTRY;
    }
    if (contextnumber >= dbg->de_rnglists_context_count) {
        return DW_DLV_NO_ENTRY;
    }
    if (entry_offset >= secsize) {
        return DW_DLV_NO_ENTRY;
    }
    if (endoffset > secsize) {
        _dwarf_error_string(dbg, error, DW_DLE_RNGLISTS_ERROR,
            " DW_DLE_RNGLISTS_ERROR "
            "The end offset to "
            "dwarf_get_rnglist_rle() is "
            "too large for the section");
        return DW_DLV_ERROR;
    }
    if (endoffset <= entry_offset) {
        _dwarf_error_string(dbg, error, DW_DLE_RNGLISTS_ERROR,
            " DW_DLE_RNGLISTS_ERROR "
            "The end offset to "
            "dwarf_get_rnglist_rle() is smaller than "
            "the entry offset! Corrupt data");
        return DW_DLV_ERROR;
    }
    if ((entry_offset +1) > endoffset) {
        /*  The read_single_rle_entry call will need
            at least 1 byte as it reads at least one
            ULEB */
        dwarfstring m;
        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            " DW_DLE_RNGLISTS_ERROR "
            "The entry offset+1 (%lu) "
            "dwarf_get_rnglist_rle() is too close to the"
            " end",entry_offset+1);
        dwarfstring_append_printf_u(&m,
            " of the offset of the end of the entry (%lu)"
            " Apparently corrupt Dwarf",endoffset);
        _dwarf_error_string(dbg, error, DW_DLE_RNGLISTS_ERROR,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    data = dbg->de_debug_rnglists.dss_data +
        entry_offset;
    enddata = dbg->de_debug_rnglists.dss_data +
        endoffset;

    con = dbg->de_rnglists_context[contextnumber];
    address_size = con->rc_address_size;
    res = read_single_rle_entry(dbg,
        data,entry_offset,enddata,
        address_size,entrylen,
        entry_kind, entry_operand1, entry_operand2,
        error);
    return res;
}

static int
_dwarf_which_rnglists_context(Dwarf_Debug dbg,
    Dwarf_CU_Context ctx,
    Dwarf_Unsigned rnglist_offset/* not always set */,
    Dwarf_Unsigned *index,
    Dwarf_Error *error)
{
    Dwarf_Unsigned          count = 0;
    Dwarf_Rnglists_Context *array;
    Dwarf_Rnglists_Context  rcx = 0;
    Dwarf_Unsigned          rcxoff = 0;
    Dwarf_Unsigned          rcxend = 0;

    Dwarf_Unsigned          i = 0;
    Dwarf_Unsigned          rnglists_base = 0;
    Dwarf_Bool              found_base = FALSE;
    Dwarf_Unsigned          chosen_offset = 0;

    array = dbg->de_rnglists_context;
    count = dbg->de_rnglists_context_count;

    if (!array) {
        return DW_DLV_NO_ENTRY;
    }
    if (count == 1) {
        *index = 0;
        return DW_DLV_OK;
    }
    if (ctx->cc_rnglists_base_present) {
        rnglists_base = ctx->cc_rnglists_base;
        found_base = TRUE;
        chosen_offset = rnglists_base;
    }
#if 0 /* Do not do this, ignore fission data */
    Dwarf_Bool              rnglists_base_present = FALSE;
    if (!found_base) {
        int                     res = 0;
        /*  This works for CU access, but fails for TU access
            as for .debug_tu_index there is no whole-type-unit
            entry in any .debug_tu_index section.
            DWARF5 Sec 7.3.5 Page 190. */
        res = _dwarf_has_SECT_fission(ctx,
            DW_SECT_RNGLISTS,
            &rnglists_base_present,&rnglists_base);
        if (res == DW_DLV_OK) {
            found_base = TRUE;
            chosen_offset = rnglists_base;
        }
    }
#endif
    if (!found_base) {
        rnglists_base = rnglist_offset;
        chosen_offset = rnglist_offset;
    }

    rcx = array[i];
    rcxoff = rcx->rc_header_offset;
    rcxend = rcxoff + rcx->rc_length;

    /* We look at the location of each rnglist context
        to find one with the offset we want */
    for ( i = 0 ; i < count; ++i) {
        rcx = array[i];
        rcxoff = rcx->rc_header_offset;
        rcxend = rcxoff +
            rcx->rc_length;
        if (chosen_offset < rcxoff){
            continue;
        }
        if (chosen_offset < rcxend ){
            *index = i;
            return DW_DLV_OK;
        }
    }
    {
        dwarfstring m;

        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            "DW_DLE_RNGLISTS_ERROR: rnglist ran off end "
            " finding target offset of"
            " 0x%" DW_PR_XZEROS DW_PR_DUx ,chosen_offset);
        dwarfstring_append(&m,
            " Not found anywhere in .debug_rnglists[.dwo] "
            "data. Corrupted data?");
        _dwarf_error_string(dbg,error,
            DW_DLE_RNGLISTS_ERROR,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
    }
    return DW_DLV_ERROR;
}

void
dwarf_dealloc_rnglists_head(Dwarf_Rnglists_Head h)
{
    Dwarf_Debug dbg = 0;

    if (!h || !h->rh_dbg || h->rh_magic != RNGLISTS_MAGIC) {
        return;
    }
    dbg = h->rh_dbg;
    h->rh_magic = 0;
    dwarf_dealloc(dbg,h,DW_DLA_RNGLISTS_HEAD);
    return;
}

/*  Appends to singly-linked list.
    Caller will eventually free as appropriate. */
static int
alloc_rle_and_append_to_list(Dwarf_Debug dbg,
    Dwarf_Rnglists_Head rctx,
    Dwarf_Rnglists_Entry *e_out,
    Dwarf_Error *error)
{
    Dwarf_Rnglists_Entry e = 0;

    e = malloc(sizeof(struct Dwarf_Rnglists_Entry_s));
    if (!e) {
        _dwarf_error_string(dbg, error, DW_DLE_ALLOC_FAIL,
            "DW_DLE_ALLOC_FAIL: Out of memory in "
            "building list of rnglists entries on a DIE.");
        return DW_DLV_ERROR;
    }
    memset(e,0,sizeof(struct Dwarf_Rnglists_Entry_s));
    if (rctx->rh_first) {
        rctx->rh_last->rle_next = e;
        rctx->rh_last = e;
    } else {
        rctx->rh_first = e;
        rctx->rh_last = e;
    }
    rctx->rh_count++;
    *e_out = e;
    return DW_DLV_OK;
}

/*  Read the group of rangelists entries, and
    finally build an array of Dwarf_Rnglists_Entry
    records. Attach to rctx here.
    Since on error the caller will destruct the rctx
    and we ensure to attach allocations there
    the caller will destruct the allocations here
    in case we return DW_DLV_ERROR*/
static int
build_array_of_rle(Dwarf_Debug dbg,
    Dwarf_Rnglists_Head rhd,
    Dwarf_Error *error)
{
    int res = 0;
    Dwarf_Small * data        = rhd->rh_rlepointer;
    Dwarf_Unsigned dataoffset = rhd->rh_rlearea_offset;
    Dwarf_Small *enddata      = rhd->rh_end_data_area;
    unsigned address_size     = (unsigned int)rhd->rh_address_size;
    Dwarf_Unsigned bytescounttotal= 0;
    Dwarf_Unsigned latestbaseaddr = 0;
    Dwarf_Bool foundbaseaddr        = FALSE;
    int done = FALSE;
    Dwarf_Bool no_debug_addr_available = FALSE;

    if (rhd->rh_cu_base_address_present) {
        /*  The CU DIE had DW_AT_low_pc
            and it is a base address. */
        latestbaseaddr = rhd->rh_cu_base_address;
        foundbaseaddr  = TRUE;
    }
    for ( ; !done  ; ) {
        unsigned entrylen = 0;
        unsigned code = 0;
        Dwarf_Unsigned val1 = 0;
        Dwarf_Unsigned val2 = 0;
        Dwarf_Addr addr1= 0;
        Dwarf_Addr addr2 = 0;
        Dwarf_Rnglists_Entry e = 0;

        res = read_single_rle_entry(dbg,
            data,dataoffset, enddata,
            address_size,&entrylen,
            &code,&val1, &val2,error);
        if (res != DW_DLV_OK) {
            return res;
        }
        res = alloc_rle_and_append_to_list(dbg,rhd,&e,error);
        if (res != DW_DLV_OK) {
            return res;
        }
        e->rle_code = code,
        e->rle_entrylen = entrylen;
        e->rle_raw1 = val1;
        e->rle_raw2 = val2;
        bytescounttotal += entrylen;
        data += entrylen;
        if (code == DW_RLE_end_of_list) {
            done = TRUE;
            break;
        }
        switch(code) {
        case DW_RLE_base_addressx:
            if (no_debug_addr_available) {
                res = DW_DLV_NO_ENTRY;
            } else {
                res = _dwarf_look_in_local_and_tied_by_index(
                    dbg,rhd->rh_context,val1,&addr1,
                    error);
            }
            if (res != DW_DLV_OK) {
                no_debug_addr_available = TRUE;
                e->rle_index_failed = TRUE;
                e->rle_cooked1 = 0;
                foundbaseaddr = FALSE;
                if (res == DW_DLV_ERROR) {
                    dwarf_dealloc_error(dbg,*error);
                    *error = 0;
                }
            } else {
                foundbaseaddr = TRUE;
                e->rle_cooked1 = addr1;
                latestbaseaddr = addr1;
            }
            break;
        case DW_RLE_startx_endx:
            if (no_debug_addr_available) {
                res = DW_DLV_NO_ENTRY;
            } else {
                res = _dwarf_look_in_local_and_tied_by_index(
                    dbg,rhd->rh_context,val1,&addr1,
                    error);
            }
            if (res != DW_DLV_OK) {
                no_debug_addr_available = TRUE;
                e->rle_index_failed = TRUE;
                e->rle_cooked1 = 0;
                if (res == DW_DLV_ERROR) {
                    dwarf_dealloc_error(dbg,*error);
                    *error = 0;
                }
            } else {
                e->rle_cooked1 = addr1;
            }
            if (no_debug_addr_available) {
                res = DW_DLV_NO_ENTRY;
            } else {
                res = _dwarf_look_in_local_and_tied_by_index(
                    dbg,rhd->rh_context,val2,&addr2,
                    error);
            }
            if (res != DW_DLV_OK) {
                no_debug_addr_available = TRUE;
                e->rle_index_failed = TRUE;
                e->rle_cooked2 = 0;
                if (res == DW_DLV_ERROR) {
                    dwarf_dealloc_error(dbg,*error);
                    *error = 0;
                }
            } else {
                e->rle_cooked2 = addr2;
            }
            break;
        case DW_RLE_startx_length:
            if (no_debug_addr_available) {
                res = DW_DLV_NO_ENTRY;
            } else {
                res = _dwarf_look_in_local_and_tied_by_index(
                    dbg,rhd->rh_context,val1,&addr1,
                    error);
            }
            if (res != DW_DLV_OK) {
                no_debug_addr_available = TRUE;
                e->rle_index_failed = TRUE;
                e->rle_cooked2 = 0;
                e->rle_cooked1 = 0;
                if (res == DW_DLV_ERROR) {
                    dwarf_dealloc_error(dbg,*error);
                    *error = 0;
                }
            } else {
                e->rle_cooked1 = addr1;
                e->rle_cooked2 = val2+addr1;
            }
            break;
        case DW_RLE_offset_pair:
            if (foundbaseaddr) {
                e->rle_cooked1 = val1+latestbaseaddr;
                e->rle_cooked2 = val2+latestbaseaddr;
            } else {
                /*  Well, something failed, could be
                    missing debug_addr. */
                e->rle_index_failed = TRUE;
                e->rle_cooked2 = 0;
                e->rle_cooked1 = 0;
            }
            break;
        case DW_RLE_base_address:
            foundbaseaddr = TRUE;
            latestbaseaddr = val1;
            e->rle_cooked1 = val1;
            break;
        case DW_RLE_start_end:
            e->rle_cooked1 = val1;
            e->rle_cooked2 = val2;
            break;
        case DW_RLE_start_length:
            e->rle_cooked1 = val1;
            e->rle_cooked2 = val2+val1;
            break;
        default: {
            dwarfstring m;

            dwarfstring_constructor(&m);
            dwarfstring_append_printf_u(&m,
                " DW_DLE_RNGLISTS_ERROR: "
                " The .debug_rnglists[.dwo] "
                " rangelist code 0x%x is unknown, "
                " DWARF5 is corrupted.",code);
            _dwarf_error_string(dbg, error,
                DW_DLE_RNGLISTS_ERROR,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
        }
        }
    }
    if (rhd->rh_count > 0) {
        Dwarf_Rnglists_Entry* array = 0;
        Dwarf_Rnglists_Entry cur = 0;
        Dwarf_Unsigned i = 0;

        /*  Creating an array of pointers. */
        array = (Dwarf_Rnglists_Entry*)malloc(
            rhd->rh_count *sizeof(Dwarf_Rnglists_Entry));
        if (!array) {
            _dwarf_error_string(dbg, error, DW_DLE_ALLOC_FAIL,
                "DW_DLE_ALLOC_FAIL: Out of memory in "
                "turning list of rnglists entries on a DIE"
                "into a pointer array");
            return DW_DLV_ERROR;
        }
        cur = rhd->rh_first;
        for ( ; i < rhd->rh_count; ++i) {
            array[i] = cur;
            cur = cur->rle_next;
        }
        rhd->rh_rnglists = array;
        rhd->rh_first = 0;
        rhd->rh_last = 0;
    }
    rhd->rh_bytes_total = bytescounttotal;
    return DW_DLV_OK;
}

static int
_dwarf_fill_in_rle_head(Dwarf_Debug dbg,
    Dwarf_Half     theform,
    /*  attr_val is either an offset
        (theform == DW_FORM_sec_offset)
        to a specific rangelist entry set  or
        or a rnglistx index DW_FORM_rnglistx
        at that index is the local offset of
        a specific rangelist entry set.  */
    Dwarf_Unsigned attr_val,
    /*  ctx is the context from the PRIMARY
        dbg, not necessarily for *this* dbg */
    Dwarf_CU_Context ctx,
    Dwarf_Rnglists_Head *head_out,
    Dwarf_Unsigned      *entries_count_out,
    Dwarf_Unsigned      *global_offset_of_rle_set,
    Dwarf_Error         *error)
{
    Dwarf_Bool     is_rnglistx = FALSE;
    Dwarf_Unsigned entrycount = 0;
    Dwarf_Unsigned offset_in_rnglists = 0;
    Dwarf_Rnglists_Context      *array = 0;
    Dwarf_Rnglists_Context       rctx = 0;
    struct Dwarf_Rnglists_Head_s shead;
    Dwarf_Rnglists_Head          lhead = 0;
    Dwarf_Unsigned rnglists_contextnum = 0;
    int            res = 0;
    Dwarf_Small   *table_base = 0;
    Dwarf_Small   *table_entry = 0;
    Dwarf_Small   *enddata = 0;
    Dwarf_Unsigned rle_global_offset = 0;
    unsigned       offsetsize = 0;
    Dwarf_Unsigned secsize = 0;
    Dwarf_Debug    localdbg = 0;

    if (theform == DW_FORM_rnglistx) {
        is_rnglistx = TRUE;
    } else {
        offset_in_rnglists = attr_val;
    }
    /*  ASSERT:  the 3 pointers just set are non-null */
    /*  the context cc_rnglists_base gives the offset
        of the array. of offsets (if cc_rnglists_base_present) */
    secsize = dbg->de_debug_rnglists.dss_size;
    if (dbg != ctx->cc_dbg) {
        /* is_rnglistx TRUE */
        /*  Now find the correct secondary context via
            signature. */
        Dwarf_CU_Context lctx = 0;
        if (ctx->cc_signature_present) {
            /* Looking in current dbg for the correct cu context */
            res = _dwarf_search_for_signature(dbg,
                ctx->cc_signature,
                &lctx,error);
            if (res != DW_DLV_OK) {
                if (res == DW_DLV_NO_ENTRY) {
                } else {
                    _dwarf_error_string(dbg,error, DW_DLE_RLE_ERROR,
                    "DW_DLE_RLE_ERROR: a .debug_rnglists[.dwo] "
                    "cu context cannot be found with the "
                    "correct signature");
                }
                return res;
            } else {
                ctx = lctx;
            }
        } else {
            /*  No signature. Hopeless, I think. */
            _dwarf_error_string(dbg,error, DW_DLE_RLE_ERROR,
                "DW_DLE_RLE_ERROR: a .debug_rnglists[.dwo] "
                "cu context cannot be found as there is "
                "no signature to use");
            return DW_DLV_ERROR;
        }
    }

    /* A */
    if (ctx->cc_rnglists_base_present) {
        offset_in_rnglists = ctx->cc_rnglists_base;
    } else {
        offset_in_rnglists = attr_val;
    }
    if (offset_in_rnglists >= secsize) {
        _dwarf_error_string(dbg,error, DW_DLE_RLE_ERROR,
            "DW_DLE_RLE_ERROR: a .debug_rnglists[.dwo] offset "
            "is greater than the rnglists section size");
        return DW_DLV_ERROR;
    }
    /* B */
    localdbg = dbg;
    {
        /*  This call works for updated DWARF5 not inheriting
            DW_AT_rnglists_base or generating such.
            It returns rangelists_context 0 if no
            base, which works for gcc/clang. */
        res = _dwarf_which_rnglists_context(localdbg,ctx,
            offset_in_rnglists,
            &rnglists_contextnum,error);
        if (res == DW_DLV_OK) {
            /* FALL THROUGH */
        } else if (res == DW_DLV_NO_ENTRY) {
            /*  This default is a gcc extension
                See dwarfstd.org Issue 240618.2
            */
            rnglists_contextnum = 0;
            /* FALL THROUGH, do not return here. */
        } else {
            return res;
        }
    }
    /* C */
    array = localdbg->de_rnglists_context;
    rctx = array[rnglists_contextnum];
    table_base = rctx->rc_offsets_array;
    entrycount = rctx->rc_offset_entry_count;
    offsetsize = rctx->rc_offset_size;
    enddata = rctx->rc_endaddr;
    if (is_rnglistx && attr_val >= entrycount) {
        dwarfstring m;

        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            "DW_DLE_RNGLISTS_ERROR: rnglists table index of"
            " %u"  ,attr_val);
        dwarfstring_append_printf_u(&m,
            " too large for table of %u "
            "entries.",entrycount);
        _dwarf_error_string(dbg,error,
            DW_DLE_RNGLISTS_ERROR,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    memset(&shead,0,sizeof(shead));
    shead.rh_context = ctx;
    shead.rh_magic = RNGLISTS_MAGIC;
    shead.rh_localcontext = rctx;
    shead.rh_index = rnglists_contextnum;
    shead.rh_version = rctx->rc_version;
    shead.rh_offset_size = offsetsize;
    shead.rh_address_size  = rctx->rc_address_size;
    shead.rh_segment_selector_size =
        rctx->rc_segment_selector_size;

    /*  DW_AT_rnglists_base from CU */
    shead.rh_at_rnglists_base_present =
        ctx->cc_rnglists_base_present;
    shead.rh_at_rnglists_base =  ctx->cc_rnglists_base;

    /*  DW_AT_low_pc originally if present.  From CU,
        possibly inherited into dwo from skeleton. */
    shead.rh_cu_base_address_present = ctx->cc_base_address_present;
    shead.rh_cu_base_address = ctx->cc_base_address;;

    /*  base address DW_AT_addr_base of our part of
        .debug_addr, from CU */
    shead.rh_cu_addr_base_offset = ctx->cc_addr_base_offset;
    shead.rh_cu_addr_base_offset_present =
        ctx->cc_addr_base_offset_present;

    /* D */
    if (is_rnglistx) {
        Dwarf_Unsigned table_entryval = 0;

        table_entry = attr_val*offsetsize + table_base;
        /*  No malloc here yet so no leak if the macro returns
            DW_DLV_ERROR */
        READ_UNALIGNED_CK(localdbg,table_entryval, Dwarf_Unsigned,
            table_entry,offsetsize,error,enddata);
        if (table_entryval >= secsize) {
            _dwarf_error_string(dbg,error, DW_DLE_RLE_ERROR,
                "DW_DLE_RLE_ERROR: a .debug_rnglists[.dwo] "
                "table entry "
                "value is impossibly large");
            return DW_DLV_ERROR;
        }
        if ((rctx->rc_offsets_off_in_sect +
            table_entryval) >= secsize) {
            _dwarf_error_string(dbg,error, DW_DLE_RLE_ERROR,
                "DW_DLE_RLE_ERROR: a .debug_rnglist[.dwo]s "
                "table entry "
                "value + section offset  is impossibly large");
            return DW_DLV_ERROR;
        }
        rle_global_offset = rctx->rc_offsets_off_in_sect +
            table_entryval;
    } else {
        rle_global_offset = attr_val;
    }
    /* E */
    shead.rh_end_data_area = enddata;
    shead.rh_rlearea_offset = rle_global_offset;
    shead.rh_rlepointer = rle_global_offset +
        localdbg->de_debug_rnglists.dss_data;
    lhead = (Dwarf_Rnglists_Head)
        _dwarf_get_alloc(dbg,DW_DLA_RNGLISTS_HEAD,1);
    if (!lhead) {
        _dwarf_error_string(dbg, error, DW_DLE_ALLOC_FAIL,
            "Allocating a Dwarf_Rnglists_Head struct fails"
            " in libdwarf function "
            "dwarf_rnglists_index_get_rle_head()");
        return DW_DLV_ERROR;
    }
    /*  To read correctly from DWARF data we must
        use the data in the tied (SECONDARY) when
        applicable. This is a bit delicate. */
    shead.rh_dbg = localdbg;
    *lhead = shead;
    res = build_array_of_rle(localdbg,lhead,error);
    if (res != DW_DLV_OK) {
        dwarf_dealloc(localdbg,lhead,DW_DLA_RNGLISTS_HEAD);
        return res;
    }
    if (global_offset_of_rle_set) {
        *global_offset_of_rle_set = rle_global_offset;
    }
    /*  Caller needs the head pointer else there will be leaks. */
    *head_out = lhead;
    if (entries_count_out) {
        *entries_count_out = lhead->rh_count;
    }
    return DW_DLV_OK;
}

/*  Build a head with all the relevent Entries
    attached.
    If the context is not found in the main object,
    and there is a tied-file
    use the context signature (should be present)
    and look in the tied-file for a match and
    get the content there.
*/
int
dwarf_rnglists_get_rle_head(
    Dwarf_Attribute attr,
    Dwarf_Half     theform,
    /*  attr_val is either an offset
        (theform == DW_FORM_sec_offset)
        to a specific rangelist entry set  or
        or a rnglistx index DW_FORM_rnglistx
        at that index is the local offset of
        a specific rangelist entry set.  */
    Dwarf_Unsigned attr_val,
    Dwarf_Rnglists_Head *head_out,
    Dwarf_Unsigned      *entries_count_out,
    Dwarf_Unsigned      *global_offset_of_rle_set,
    Dwarf_Error         *error)
{
    int res = 0;
    Dwarf_CU_Context ctx = 0;
    Dwarf_Debug dbg = 0;
    Dwarf_Bool     is_rnglistx = FALSE;

    if (!attr) {
        _dwarf_error_string(NULL, error,DW_DLE_DBG_NULL,
            "DW_DLE_DBG_NULL "
            "NULL attribute "
            "argument passed to "
            "dwarf_rnglists_get_rle_head()");
        return DW_DLV_ERROR;
    }
    ctx = attr->ar_cu_context;
    dbg =  ctx->cc_dbg;

    if (theform == DW_FORM_rnglistx) {
        is_rnglistx = TRUE;
    }
    CHECK_DBG(dbg,error,
        "dwarf_rnglists_get_rle_head() via attribute");

    if (is_rnglistx) {
        if (ctx->cc_rnglists_base_present ||
            dbg->de_rnglists_context_count == 1) {
            /*  leave on primary.
                WARNING: It is not clear whether
                looking for a context count of 1
                is actually correct, but it
                seems to work. */
        } else if (DBG_HAS_SECONDARY(dbg)){
            dbg = dbg->de_secondary_dbg;
            CHECK_DBG(dbg,error,
                "dwarf_rnglists_get_rle_head() via attribute(sec)");
        }
    } else {
        /*  attr_val is .debug_rnglists section global offset
            of a range list*/
        if (!dbg->de_debug_rnglists.dss_size ||
            attr_val >= dbg->de_debug_rnglists.dss_size) {
            if (DBG_HAS_SECONDARY(dbg)) {
                dbg = dbg->de_secondary_dbg;
                CHECK_DBG(dbg,error,
                    "dwarf_rnglists_get_rle_head() "
                    "via attribute(secb)");
            } else {
                /*  There is an error to be
                    generated later */
            }
        }
    }

    res = _dwarf_load_section(dbg,
        &dbg->de_debug_rnglists,
        error);
    if (res == DW_DLV_ERROR) {
        return res;
    }
    if (res == DW_DLV_OK && dbg->de_debug_rnglists.dss_size) {
        res = _dwarf_fill_in_rle_head(dbg, theform,
            /*  attr_val is either an offset
                (theform == DW_FORM_sec_offset)
                to a specific rangelist entry set  or
                or a rnglistx index DW_FORM_rnglistx
                at that index is the local offset of
                a specific rangelist entry set offset table.  */
            attr_val,
            ctx,
            head_out,
            entries_count_out,
            global_offset_of_rle_set,
            error);
        if (res == DW_DLV_ERROR) {
            return res;
        }
    }
    return res;
}

/*  As of 18 Aug 2020
    this ignores null pointer inputs for the various
    pointers-returning-values (rle_value_out etc)
    for the convenience of callers.  */
int
dwarf_get_rnglists_entry_fields_a(
    Dwarf_Rnglists_Head head,
    Dwarf_Unsigned  entrynum,
    unsigned       *entrylen,
    unsigned       *rle_value_out,
    Dwarf_Unsigned *raw1,
    Dwarf_Unsigned *raw2,
    Dwarf_Bool     *debug_addr_unavailable,
    Dwarf_Unsigned *cooked1,
    Dwarf_Unsigned *cooked2,
    Dwarf_Error *error)
{
    Dwarf_Rnglists_Entry e = 0;

    if (!head || !head->rh_dbg || head->rh_magic != RNGLISTS_MAGIC) {
        _dwarf_error_string(NULL, error,DW_DLE_DBG_NULL,
            "DW_DLE_DBG_NULL "
            "NULL or invalid Dwarf_Rnglists_Head "
            "argument passed to "
            "dwarf_get_rnglists_entry_fields_a()");
        return DW_DLV_ERROR;
    }
    if (entrynum >= head->rh_count) {
        return DW_DLV_NO_ENTRY;
    }
    e = head->rh_rnglists[entrynum];
    if (entrylen) {
        *entrylen  = e->rle_entrylen;
    }
    if (rle_value_out) {
        *rle_value_out = e->rle_code;
    }
    if (raw1) {
        *raw1      = e->rle_raw1;
    }
    if (raw2) {
        *raw2      = e->rle_raw2;
    }
    if (debug_addr_unavailable) {
        *debug_addr_unavailable = e->rle_index_failed;
    }
    if (cooked1) {
        *cooked1   = e->rle_cooked1;
    }
    if (cooked2) {
        *cooked2   = e->rle_cooked2;
    }
    return DW_DLV_OK;
}

/*  Deals with both fully and partially build head */
static void
_dwarf_free_rnglists_head(Dwarf_Rnglists_Head head)
{
    if (head->rh_first) {
        /* partially built head. */
        /*  ASSERT: rh_rnglists is NULL */
        Dwarf_Rnglists_Entry cur = head->rh_first;
        Dwarf_Rnglists_Entry next = 0;

        for ( ; cur ; cur = next) {
            next = cur->rle_next;
            free(cur);
        }
        head->rh_first = 0;
        head->rh_last = 0;
        head->rh_count = 0;
    } else {
        /*  ASSERT: rh_first and rh_last are NULL */
        /* fully built head. */
        Dwarf_Unsigned i = 0;

        /* Deal with the array form. */
        for ( ; i < head->rh_count; ++i) {
            free(head->rh_rnglists[i]);
        }
        free(head->rh_rnglists);
        head->rh_rnglists = 0;
    }
}

void
_dwarf_rnglists_head_destructor(void *head)
{
    Dwarf_Rnglists_Head h = head;

    _dwarf_free_rnglists_head(h);
}
