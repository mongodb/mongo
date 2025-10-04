/*
Copyright (C) 2022 David Anderson. All Rights Reserved.

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

#include <stddef.h> /* NULL size_t */
#include <stdio.h>  /* debug printf */
#include <string.h> /* memset() strlen() */

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
#include "dwarf_debugaddr.h"
#include "dwarf_string.h"

#if 0 /* dump_bytes */
static void
dump_bytes(const char *msg,Dwarf_Small * start, long len)
{
    Dwarf_Small *end = start + len;
    Dwarf_Small *cur = start;
    int linelim=16;
    int lineb = 0;

    printf("dump_bytes: %s %ld starting %p \n",msg,len,
        (void *)start);
    fflush(stdout);
    for (; cur < end; cur++, ++lineb) {
        if (lineb == linelim) {
            printf("\n");
            lineb = 0;
        }
        printf("%02x",*cur);
    }
    printf("\n");
    fflush(stdout);
}
#endif /* 0 */

int
dwarf_debug_addr_table(Dwarf_Debug dbg,
    Dwarf_Unsigned    dw_section_offset,
    Dwarf_Debug_Addr_Table *dw_table_header,
    Dwarf_Unsigned   *dw_length,
    Dwarf_Half       *dw_version,
    Dwarf_Small      *dw_address_size,
    Dwarf_Unsigned   *dw_at_addr_base,
    Dwarf_Unsigned   *dw_entry_count,
    Dwarf_Unsigned   *dw_next_table_offset,
    Dwarf_Error      *error)
{
    int res = 0;
    struct Dwarf_Debug_Addr_Table_s tab;
    Dwarf_Unsigned section_size = 0;
    Dwarf_Small   *end_data = 0;
    Dwarf_Small   *data = 0;
    Dwarf_Small   *section_start = 0;
    Dwarf_Unsigned arealen = 0;
    Dwarf_Unsigned tablelen = 0;
    int            offset_size = 0;
    int            exten_size = 0;
    Dwarf_Small    address_size = 0;
    Dwarf_Small    segment_selector_size = 0;
    Dwarf_Half     version = 0;
    Dwarf_Unsigned curlocaloffset = 0;
    Dwarf_Unsigned offset_one_past_end = 0;
    /* we will instantiate this below */
    Dwarf_Debug_Addr_Table newad = 0;

    CHECK_DBG(dbg,error,"dwarf_debug_addr_table()");
    res = _dwarf_load_section(dbg, &dbg->de_debug_addr,error);
    if (res == DW_DLV_NO_ENTRY) {
        return res;
    }
    if (res == DW_DLV_ERROR) {
        if (error) {
            Dwarf_Error e = *error;

            /* Lets append info to the error string! */
            if (e->er_static_alloc != DE_STATIC) {
                dwarfstring *em = (dwarfstring*)(e->er_msg);
                dwarfstring_append(em, "Unable to open "
                    ".debug_addr section, serious error");
            }
        }
        return DW_DLV_ERROR;
    }
    memset(&tab,0,sizeof(tab));
    tab.da_magic = DW_ADDR_TABLE_MAGIC;
    section_size = dbg->de_debug_addr.dss_size;
    section_start = dbg->de_debug_addr.dss_data;
    end_data   = section_start + section_size;
    tab.da_section_size = section_size;
    if (dw_section_offset >= section_size) {
        return DW_DLV_NO_ENTRY;
    }
    if (dbg->de_debug_addr_version == DW_CU_VERSION4) {
        /*  Create a table from what we know. */
        address_size = (Dwarf_Small)dbg->de_debug_addr_address_size;
        offset_size = dbg->de_debug_addr_offset_size;
        tab.da_address_size = address_size;
        tab.da_length_size = (Dwarf_Small)offset_size;
        tab.da_length = section_size;
        tab.da_version = dbg->de_debug_addr_version;
        end_data = section_start + section_size;
        tab.da_end_table = end_data;
        /*  Must be zero. */
        if (dw_section_offset) {
            _dwarf_error_string(dbg,error,DW_DLE_DEBUG_ADDR_ERROR,
                "DW_DLE_DEBUG_ADDR_ERROR: DWARF4 extension "
                ".debug_addr has non-zero offset. Impossible");
            return DW_DLV_ERROR;
        }
        tab.da_table_section_offset = dw_section_offset;
        tab.da_data_entries = section_start;
        tab.da_entry_count= section_size/tab.da_address_size;
        /*  One past end of this Debug Addr Table */
        tab.da_end_table = end_data;
        if (tab.da_address_size != 4 && tab.da_address_size != 8 &&
            tab.da_address_size != 2) {
            dwarfstring m;
            dwarfstring_constructor(&m);
            dwarfstring_append_printf_u(&m,
                " DW_DLE_ADDRESS_SIZE_ERROR: The "
                " .debug_addr DWARF4 address size "
                "of %u is not supported.",address_size);
            _dwarf_error_string(dbg,error,DW_DLE_ADDRESS_SIZE_ERROR,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
        }
        offset_one_past_end = section_size;
        tab.da_dbg = dbg;
    } else {
        /* all other cases, assume DWARF5 table. */
        curlocaloffset = 0;
        data = section_start + dw_section_offset;
        READ_AREA_LENGTH_CK(dbg,arealen,Dwarf_Unsigned,
            data,offset_size,exten_size,
            error,
            section_size,end_data);
        if (arealen > section_size ||
            (arealen + offset_size +exten_size) > section_size) {
            dwarfstring m;
            dwarfstring_constructor(&m);
            dwarfstring_append_printf_u(&m,
                "DW_DLE_SECTION_SIZE_ERROR: A .debug_addr "
                "area size of 0x%x ",arealen);
            dwarfstring_append_printf_u(&m,
                "at offset 0x%x ",dw_section_offset);
            dwarfstring_append_printf_u(&m,
                "is larger than the entire section size of "
                "0x%x. Corrupt DWARF.",section_size);
            _dwarf_error_string(dbg,error,
                DW_DLE_SECTION_SIZE_ERROR,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
        }
        tab.da_dbg = dbg;
        tablelen = 0;
        if (arealen <= 4) {
            _dwarf_error_string(dbg,error,
                DW_DLE_SECTION_SIZE_ERROR,
                "DW_DLE_SECTION_SIZE_ERROR: "
                "The end of a .debug_addr header record is missing, "
                "corrupt DWARF");
            return DW_DLV_ERROR;
        }
        tablelen = arealen - 4; /* 4: the rest of the header */
        tab.da_length = tablelen;
        curlocaloffset = offset_size + exten_size;
        offset_one_past_end = dw_section_offset
            + curlocaloffset + 4 /*rest of header */
            + tablelen;
        end_data = section_start + offset_one_past_end;
        tab.da_end_table = end_data;
        READ_UNALIGNED_CK(dbg,version,Dwarf_Half,data,
            SIZEOFT16,error,end_data);
        if (version != DW_CU_VERSION5) {
            dwarfstring m;
            dwarfstring_constructor(&m);
            dwarfstring_append_printf_u(&m,
                "DW_DLE_VERSION_STAMP_ERROR: "
                "The .debug_addr version should be 5 "
                "but we find %u instead.",version);
            _dwarf_error_string(dbg,error,
                DW_DLE_VERSION_STAMP_ERROR,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
        }
        tab.da_version = version;
        data += SIZEOFT16;
        curlocaloffset += SIZEOFT16;
        READ_UNALIGNED_CK(dbg,address_size,Dwarf_Small,data,
            1,error,end_data);
        if (address_size != 4 && address_size != 8 &&
            address_size != 2) {
            dwarfstring m;
            dwarfstring_constructor(&m);
            dwarfstring_append_printf_u(&m,
                " DW_DLE_ADDRESS_SIZE_ERROR: The "
                " .debug_addr address size "
                "of %u is not supported.",address_size);
            _dwarf_error_string(dbg,error,DW_DLE_ADDRESS_SIZE_ERROR,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
        }
        tab.da_address_size = address_size;
        data++;
        curlocaloffset++;

        READ_UNALIGNED_CK(dbg,segment_selector_size,Dwarf_Small,data,
            1,error,end_data);
        if (segment_selector_size != 0) {
            dwarfstring m;
            dwarfstring_constructor(&m);
            dwarfstring_append(&m,
                " DW_DLE_DEBUG_ADDR_ERROR: The "
                " .debug_addr segment selector size "
                "of non-zero is not supported.");
            _dwarf_error_string(dbg,error,DW_DLE_DEBUG_ADDR_ERROR,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
        }
        /*  We do not record segment selector size,
            as it is not supported. */
        curlocaloffset++;
        data++;
        tab.da_data_entries = data;
    }
    /*  Now we are at the beginning of the actual table */
    {
        Dwarf_Unsigned entry_count = 0;
        /*  Two byte version and two byte flags preceed the
            actual table */
        Dwarf_Unsigned table_len_bytes = tab.da_length;

        if (table_len_bytes%(Dwarf_Unsigned)address_size) {
            dwarfstring m;
            dwarfstring_constructor(&m);
            dwarfstring_append_printf_u(&m,
                " DW_DLE_DEBUG_ADDR_ERROR: The "
                " .debug_addr address array "
                "length of %u not a multiple of "
                "address_size.",table_len_bytes);
            _dwarf_error_string(dbg,error,
                DW_DLE_DEBUG_ADDR_ERROR,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
        }
        entry_count = table_len_bytes/address_size;
        tab.da_entry_count = entry_count;
    }
    tab.da_table_section_offset = dw_section_offset;
    tab.da_addr_base =  dw_section_offset + curlocaloffset;
    /*  Do alloc as late as possible to avoid
        any concern about missing a dealloc in
        case of error. */
    newad = (Dwarf_Debug_Addr_Table)
        _dwarf_get_alloc(dbg,DW_DLA_DEBUG_ADDR,1);
    if (!newad) {
        _dwarf_error_string(dbg, error,
            DW_DLE_ALLOC_FAIL,
            "DW_DLE_ALLOC_FAIL: "
            "allocating a Dwarf_Debug_Addr_Table "
            "record.");
        return DW_DLV_ERROR;
    }
    /*  Copy structure itself */
    *newad = tab;
    *dw_table_header = newad;
    if (dw_length) {
        *dw_length =  newad->da_length;
    }
    if (dw_version) {
        *dw_version = newad->da_version;
    }
    if (dw_address_size) {
        *dw_address_size = newad->da_address_size;
    }
    if (dw_at_addr_base) {
        *dw_at_addr_base = newad->da_addr_base;
    }
    if (dw_entry_count) {
        *dw_entry_count = newad->da_entry_count;
    }
    if (dw_next_table_offset) {
        *dw_next_table_offset = offset_one_past_end;
    }
    return DW_DLV_OK;
}

int
dwarf_debug_addr_by_index(Dwarf_Debug_Addr_Table dw_dat,
    Dwarf_Unsigned    dw_entry_index,
    Dwarf_Unsigned   *dw_address,
    Dwarf_Error      *dw_error)
{
    Dwarf_Small *data = 0;
    Dwarf_Unsigned addr = 0;

    if (!dw_dat) {
        _dwarf_error_string(NULL,dw_error,
            DW_DLE_DEBUG_ADDR_ERROR,
            "DW_DLE_DEBUG_ADDR_ERROR: "
            "NULL dw_dat passed in.");
        return DW_DLV_ERROR;
    }
    if (dw_dat->da_magic != DW_ADDR_TABLE_MAGIC) {
        _dwarf_error_string(NULL,dw_error,
            DW_DLE_DEBUG_ADDR_ERROR,
            "DW_DLE_DEBUG_ADDR_ERROR: "
            "Bad debug addr table magic number. ");
        return DW_DLV_ERROR;
    }
    if (dw_entry_index >= dw_dat->da_entry_count) {
        return DW_DLV_NO_ENTRY;
    }
    data = dw_dat->da_data_entries+
        dw_dat->da_address_size * dw_entry_index;
    if ((data+ dw_dat->da_address_size) >  dw_dat->da_end_table) {
        _dwarf_error_string(NULL,dw_error,
            DW_DLE_DEBUG_ADDR_ERROR,
            "DW_DLE_DEBUG_ADDR_ERROR: "
            "Bad debug addr table: miscount, too short. ");
        return DW_DLV_ERROR;
    }
    READ_UNALIGNED_CK(dw_dat->da_dbg,
        addr,Dwarf_Unsigned, data,
        dw_dat->da_address_size,dw_error,
        dw_dat->da_end_table);
    *dw_address =  addr;
    return DW_DLV_OK;
}

void
dwarf_dealloc_debug_addr_table(Dwarf_Debug_Addr_Table dw_dat)
{
    Dwarf_Debug dbg = 0;
    if (!dw_dat) {
        return;
    }
    if (dw_dat->da_magic != DW_ADDR_TABLE_MAGIC) {
        return;
    }
    dbg = dw_dat->da_dbg;
    dw_dat->da_magic = 0;
    dwarf_dealloc(dbg,dw_dat,DW_DLA_DEBUG_ADDR);
}
