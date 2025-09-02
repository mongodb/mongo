/*
  Copyright (C) 2020 David Anderson. All Rights Reserved.
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

/*  This is for accessing .debug_gnu_pubnames
    and .debug_gnu_pubtypes.

    These sections are designed to be compiler-generated
    to be used by the linker to create the .gdb_index
    section for DWARF3 and DWARF4 as part of
    Split Dwarf (aka Debug Fission).
    They are not expected to be in an executable. */

#include <config.h>

#include <stdlib.h> /* calloc() free() */
#include <stdio.h> /* debugging */

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
#include "dwarf_gnu_index.h"
#include "dwarf_string.h"

#if 0 /* dump_head */
static void
dump_head(const char *msg,int lno,
    struct Dwarf_Gnu_Index_Head_s *h)
{
    printf("HEAD block line %d\n",__LINE__);
    printf("data        : %p\n",
        (void *)h->gi_section_data);
    printf("sec len     : %llu 0x%llx\n",
        h->gi_section_length,
        h->gi_section_length);
    printf("data end    : %p\n",
        (void *)(h->gi_section_end));
    printf("blockarray  : %p\n",
        (void *)h->gi_blockarray);
    printf("block count : %llu 0x%llx\n",
        h->gi_blockcount,
        h->gi_blockcount);
    printf("gnu_pubnames: %u\n",h->gi_is_pubnames);
}

static void
dump_block(const char *msg,int bn, int lno,
    struct Dwarf_Gnu_IBlock_s *b)
{
    printf("BLOCK dump block %d %s line %d\n",
        bn,msg,lno);
    printf("head             : 0x%lx\n",
        (unsigned long)b->ib_head);
    printf("index            : %lu\n",
        (unsigned long)b->ib_index);
    printf("blk len offset   : %lu 0x%lx\n",
        (unsigned long)b->ib_block_length_offset,
        (unsigned long)b->ib_block_length_offset);
    printf("block length     : %lu 0x%lx\n",
        (unsigned long)b->ib_block_length,
        (unsigned long)b->ib_block_length);
    printf("offset size      : %u\n",
        b->ib_offset_size);
    printf("extension size   : %u\n",
        b->ib_extension_size);
    printf("version          : %u\n",
        b->ib_version);
    printf("built entries?   : %s\n",
        b->ib_counted_entries?"yes":"no");
    printf("debug_info offset: %lu 0x%lx\n",
        (unsigned long)b->ib_offset_in_debug_info,
        (unsigned long)b->ib_offset_in_debug_info);
    printf("debug_info size  : %lu 0x%lx\n",
        (unsigned long)b->ib_size_in_debug_info,
        (unsigned long)b->ib_size_in_debug_info);
    printf("data offset      : %lu 0x%lx\n",
        (unsigned long)b->ib_b_data_offset,
        (unsigned long)b->ib_b_data_offset);
    printf("entries offset   : %lu 0x%lx\n",
        (unsigned long)b->ib_b_offset,
        (unsigned long)b->ib_b_offset);
    printf("entries  ptr     : 0x%lx\n",
        (unsigned long)b->ib_b_data);
    printf("entries length   : %lu 0x%lx\n",
        (unsigned long)b->ib_b_entrylength,
        (unsigned long)b->ib_b_entrylength);
    printf("entry count      : %lu 0x%lx\n",
        (unsigned long)b->ib_entry_count,
        (unsigned long)b->ib_entry_count);
    printf("entries array    : 0x%lx\n",
        (unsigned long)b->ib_entryarray);
    fflush(stdout);
}
#endif /*0*/
/*  We could use dwarf_get_real_section_name()
    to determine the real name (perhaps ending in .dwo)
    but for now we just use the standard name here. */
static void
get_pubxx_fields(Dwarf_Debug dbg,
    Dwarf_Bool for_gnu_pubnames,
    struct Dwarf_Section_s **sec_out,
    const char             **sec_name_out,
    int                     *errnum_out,
    const char             **errstr_out)
{
    if (IS_INVALID_DBG(dbg)) {
        return;
    }
    if (for_gnu_pubnames) {
        if (sec_name_out) {
            *sec_name_out = ".debug_gnu_pubnames";
        }
        if (sec_out) {
            *sec_out = &dbg->de_debug_gnu_pubnames;
        }
        if (errnum_out) {
            *errnum_out = DW_DLE_GNU_PUBNAMES_ERROR;
        }

        if (errstr_out) {
            *errstr_out = "DW_DLE_GNU_PUBNAMES_ERROR";
        }
    } else {
        if (sec_name_out) {
            *sec_name_out = ".debug_gnu_pubtypes";
        }
        if (sec_out) {
            *sec_out = &dbg->de_debug_gnu_pubtypes;
        }
        if (errnum_out) {
            *errnum_out = DW_DLE_GNU_PUBTYPES_ERROR;
        }
        if (errstr_out) {
            *errstr_out = "DW_DLE_GNU_PUBTYPES_ERROR";
        }
    }
}
static void
build_errm_no_v(Dwarf_Debug dbg,
    Dwarf_Bool is_for_pubnames,
    const char * str1,
    Dwarf_Error *error)
{
    const char *errstr  = 0;
    int   errnum        = 0;
    const char *secname = 0;
    dwarfstring m;

    get_pubxx_fields(dbg,is_for_pubnames,
        0,&secname,&errnum, &errstr);
    dwarfstring_constructor(&m);
    dwarfstring_append(&m,(char *)errstr);
    dwarfstring_append(&m," ");
    dwarfstring_append(&m,(char *)str1);
    dwarfstring_append_printf_s(&m," for section %s",
        (char*)secname);
    _dwarf_error_string(dbg,error,errnum,
        dwarfstring_string(&m));
    dwarfstring_destructor(&m);
    return;
}

static void
build_errm_one_num(Dwarf_Debug dbg,
    Dwarf_Bool is_for_pubnames,
    const char * str1,
    Dwarf_Unsigned val1,
    Dwarf_Error *error)
{
    const char *errstr  = 0;
    int   errnum        = 0;
    const char *secname = 0;
    dwarfstring m;

    /* Something is very wrong */
    get_pubxx_fields(dbg,is_for_pubnames,
        0,&secname,&errnum, &errstr);
    dwarfstring_constructor(&m);
    dwarfstring_append(&m,(char *)errstr);
    dwarfstring_append(&m," ");
    dwarfstring_append_printf_u(&m,(char *)str1,val1);
    dwarfstring_append_printf_s(&m," for section %s",
        (char*)secname);
    _dwarf_error_string(dbg,error,errnum,
        dwarfstring_string(&m));
    dwarfstring_destructor(&m);
    return;
}

static int
load_pub_section(Dwarf_Debug dbg,
    Dwarf_Bool for_gnu_pubnames,
    Dwarf_Error *error)
{
    struct Dwarf_Section_s * sec = 0;
    int res;

    get_pubxx_fields(dbg,for_gnu_pubnames,&sec,0,0,0);
    if (sec) {
        res = _dwarf_load_section(dbg,sec,error);
        return res;
    }
    return DW_DLV_NO_ENTRY;
}

static int
scan_block_entries(Dwarf_Debug  dbg,
    Dwarf_Bool for_gnu_pubnames,
    Dwarf_Unsigned *count_out,
    Dwarf_Error *error)
{
    struct Dwarf_Section_s *sec = 0;
    Dwarf_Small *startptr =  0;
    Dwarf_Small *curptr =  0;
    Dwarf_Small *endptr =  0;
    Dwarf_Unsigned seclen = 0;
    Dwarf_Unsigned count = 0;
    Dwarf_Unsigned filesize = 0;
    int errnum          = 0;
    Dwarf_Unsigned  totalframessize = 0;
    const char * errstr = 0;
    const char * secname = 0;

    get_pubxx_fields(dbg,for_gnu_pubnames,&sec,&secname,
        &errnum,&errstr);
    filesize = dbg->de_filesize;
    startptr = sec->dss_data;
    curptr   = startptr;
    seclen =   sec->dss_size;
    endptr =   startptr + seclen;
    if (filesize) {
        if (seclen >= filesize) {
            build_errm_one_num(dbg,for_gnu_pubnames,
                ": section length %u"
                " is larger than the file size in",
                seclen,error);
            return DW_DLV_ERROR;
        }
    }
    for (;;) {
        Dwarf_Unsigned length = 0;
        unsigned int offsetsize = 0;
        unsigned int extensize = 0;
        unsigned int sumsize = 0;

        if (curptr == endptr) {
            break;
        }
        /*  Not sure how the coders think about
            the initial value. But the last
            4 bytes are zero, ignore those.
            Unclear 64bit is not allowed. */
        READ_AREA_LENGTH_CK(dbg,length,Dwarf_Unsigned,
            curptr,offsetsize,extensize,error,seclen,endptr);
        totalframessize += length +offsetsize+extensize;
        if (length > seclen || totalframessize > seclen) {
            build_errm_one_num(dbg,for_gnu_pubnames,
                "Sum of frame fde/cies sizes 0x%" DW_PR_DUx
                " exceeds section size",totalframessize,error);
            return DW_DLV_ERROR;
        }

        ++count;
        sumsize = offsetsize +extensize;
        if (length < sumsize) {
            build_errm_one_num(dbg,for_gnu_pubnames,
                "Length of fde/cies header sizes 0x%" DW_PR_DUx
                " is impossibly small",length,error);
            return DW_DLV_ERROR;
        }
        curptr +=  length - sumsize;
        curptr += 4;
    }
    *count_out = count;
    return DW_DLV_OK;
}

static int
_dwarf_count_entries_in_block(struct Dwarf_Gnu_IBlock_s * gib,
    struct DGI_Entry_s* entries,
    Dwarf_Bool for_gnu_pubnames,
    Dwarf_Error* error)
{
    Dwarf_Small *curptr = gib->ib_b_data;
    Dwarf_Small *endptr = curptr + gib->ib_b_entrylength;
    Dwarf_Unsigned entrycount = 0;
    Dwarf_Half offsetsize = gib->ib_offset_size;
    struct DGI_Entry_s *curentry = 0;
    Dwarf_Debug dbg = 0;
    Dwarf_Gnu_Index_Head head = 0;
    Dwarf_Bool for_pubnames = 0;
    char *strptr = 0;

    head = gib->ib_head;
    for_pubnames  =  head->gi_is_pubnames;
    dbg = head->gi_dbg;
    for ( ; curptr < endptr; ++entrycount) {
        Dwarf_Unsigned infooffset = 0;
        Dwarf_Unsigned offset = 0;
        char  flagbyte = 0;

        if ((curptr+offsetsize) == endptr) {
            break;
        }
        if ((curptr+offsetsize) > endptr) {
            build_errm_one_num(dbg,for_gnu_pubnames,
                " Reading an address runs off the end of "
                "this entry at entry %" DW_PR_DUu,
                entrycount,error);
            return DW_DLV_ERROR;
        }
        READ_UNALIGNED_CK(dbg,offset,
            Dwarf_Unsigned,curptr,
            offsetsize,error,endptr);
        infooffset = offset;
        curptr += offsetsize;
        if (entries) {
            curentry = entries +entrycount;
            curentry->ge_debug_info_offset = infooffset;
        }
        /* Ensure flag and start-of-string possible. */
        if ((curptr+2) >= endptr) {
            build_errm_one_num(dbg,for_gnu_pubnames,
                "Past end of current block reading strings"
                " Reading an address runs off the end of "
                "this entry at entry %" DW_PR_DUu,
                entrycount,error);
            return DW_DLV_ERROR;
        }
        flagbyte = *curptr;
        curptr += 1;
        strptr = (char *)curptr;
        if (curentry) {
            curentry->ge_flag_byte = flagbyte;
            curentry->ge_string = (char *)strptr;
        }
        for ( ; curptr && curptr < endptr ;++curptr,++offset ) {
            if (! *curptr ) {
                /* end of string */
                break;
            }
            if (curptr > endptr) {
                build_errm_no_v(dbg,for_gnu_pubnames,
                    "Past end of current block reading strings",
                    error);
                return DW_DLV_ERROR;
            }
        }
        /* string-terminating null byte */
        curptr += 1;
    }
    if (!entries)  {
        gib->ib_entry_count = entrycount;
    } else if (gib->ib_entry_count != entrycount) {
        int err = 0;
        const char *errstr  = 0;
        const char *secname  = 0;
        char buf[120];
        dwarfstring m;

        buf[0] = 0;
        get_pubxx_fields(dbg,for_pubnames,0,&secname,
            &err,&errstr);
        dwarfstring_constructor_static(&m,buf,sizeof(buf));
        dwarfstring_append(&m,(char *)errstr);
        dwarfstring_append_printf_s(&m,":mismatch counts "
            "creating %s"
            "block_entries.",
            (char *)secname);
        dwarfstring_append_printf_u(&m," Origcount %u",
            gib->ib_entry_count);
        dwarfstring_append_printf_u(&m," new count %u",
            entrycount);
        _dwarf_error_string(dbg,error,err,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    return DW_DLV_OK;
}

static int
fill_in_blocks(Dwarf_Gnu_Index_Head head,
    Dwarf_Error *error)
{
    Dwarf_Unsigned i = 0;
    Dwarf_Unsigned dataoffset = 0;
    Dwarf_Small    * endptr = 0;
    Dwarf_Small    * curptr = 0;
    Dwarf_Small    * baseptr = 0;
    Dwarf_Bool     is_for_pubnames = head->gi_is_pubnames;
    Dwarf_Debug    dbg = head->gi_dbg;
    Dwarf_Unsigned seclen = head->gi_section_length;

    baseptr = head->gi_section_data;
    endptr = baseptr + head->gi_section_length;
    for ( ;i < head->gi_blockcount; ++i) {
        Dwarf_Unsigned length = 0;
        unsigned int offsetsize = 0;
        unsigned int extensize = 0;
        Dwarf_Half version = 0;
        Dwarf_Unsigned offset_into_debug_info = 0;
        Dwarf_Unsigned length_of_CU_in_debug_info = 0;
        struct Dwarf_Gnu_IBlock_s *gib = 0;
        int res = 0;

        gib = head->gi_blockarray+i;
        /* gib is a blank slate ready to be filled */
        curptr = baseptr+ dataoffset;
        READ_AREA_LENGTH_CK(dbg,length,Dwarf_Unsigned,
            curptr,offsetsize,extensize,error,seclen,endptr);
        if (!length) {
            /*  Must be end of the section */
            if (curptr != endptr) {
                build_errm_no_v(dbg,is_for_pubnames,
                    ": encountered zero area length "
                    "before the section end.",
                    error);
                return DW_DLV_ERROR;
            }
            return DW_DLV_OK;
        }
        if (length > head->gi_section_length ||
            (length+offsetsize+extensize) >
            head->gi_section_length) {
            build_errm_one_num(dbg,is_for_pubnames,
                ": block"
                " length %" DW_PR_DUu " is too big",
                length,error);
            return DW_DLV_ERROR;
        }
        /*  offsetsize will be set to a constant
            of either 4 or 8 by READ_AREA_LENGTH_CK.
            So any check for a usable offsetsize
            here is dead code.
            see dwarf_util.h  */
        gib->ib_index = i;
        gib->ib_head  = head;
        gib->ib_offset_size = (Dwarf_Half)offsetsize;
        gib->ib_block_length        = length;
        gib->ib_block_length_offset = dataoffset;
        dataoffset += offsetsize + extensize;
        /* Now past length field. */
        gib->ib_b_data_offset       = dataoffset;
        if ((dataoffset + length) > head->gi_section_length){
            build_errm_one_num(dbg,is_for_pubnames,
                " header length is %u which runs past the "
                "end of section suggesting corrupt data",
                length,error);
            return DW_DLV_ERROR;
        }
        READ_UNALIGNED_CK(dbg,version,Dwarf_Half,curptr,
            DWARF_HALF_SIZE,error,endptr);
        if (version != 2) {
            build_errm_one_num(dbg,is_for_pubnames,
                " version is %u, not 2,"
                " which suggests corrupt data",version,error);
            return DW_DLV_ERROR;
        }
        curptr     += DWARF_HALF_SIZE;
        dataoffset += DWARF_HALF_SIZE;
        gib->ib_version = version;
        READ_UNALIGNED_CK(dbg,offset_into_debug_info,
            Dwarf_Unsigned,curptr,
            offsetsize,error,endptr);
        curptr     += offsetsize;
        dataoffset += offsetsize;
        gib->ib_offset_in_debug_info = offset_into_debug_info;
        READ_UNALIGNED_CK(dbg,length_of_CU_in_debug_info,
            Dwarf_Unsigned,curptr,
            offsetsize,error,endptr);
        gib->ib_size_in_debug_info = length_of_CU_in_debug_info;
        dataoffset += offsetsize;
        curptr     += offsetsize;
        gib->ib_b_data = curptr;
        gib->ib_b_offset = dataoffset;
        gib->ib_b_entrylength = length - (2 +
            (2*offsetsize)) -4;
        if ((gib->ib_b_data +gib->ib_b_entrylength)
            > endptr) {
            build_errm_one_num(dbg,is_for_pubnames,
                " length is %u which runs past the "
                "end of section suggesting corrupt data",
                length,error);
            return DW_DLV_ERROR;
        }
        res = _dwarf_count_entries_in_block(gib,0,is_for_pubnames,
            error);
        if (res != DW_DLV_OK) {
            return res;
        }
        /* Set for next block., add in 4 for ending zeros */
        dataoffset = gib->ib_block_length_offset + length + 4;
    }
    return DW_DLV_OK;
}

static int
fill_in_entries(Dwarf_Gnu_Index_Head head,
    struct Dwarf_Gnu_IBlock_s *gib,
    Dwarf_Error *error)
{
    Dwarf_Unsigned count = gib->ib_entry_count;
    struct DGI_Entry_s * entryarray = 0;
    Dwarf_Bool for_gnu_pubnames = head->gi_is_pubnames;
    int res = 0;
    Dwarf_Debug dbg = 0;

    dbg = head->gi_dbg;
    entryarray = (struct DGI_Entry_s*)calloc(count,
        sizeof(struct DGI_Entry_s));
    if (!entryarray) {
        build_errm_no_v(dbg,for_gnu_pubnames,
            ": Unable to allocate "
            "block_entries. Out of memory creating record.",
            error);
        return DW_DLV_ERROR;
    }
    res = _dwarf_count_entries_in_block(gib,
        entryarray,for_gnu_pubnames,error);
    if (res != DW_DLV_OK) {
        free(entryarray);
        return res;
    }
    gib->ib_entryarray = entryarray;
    entryarray = 0;
    return DW_DLV_OK;
}

int
dwarf_get_gnu_index_head(Dwarf_Debug dbg,
    /*  The following arg false to select gnu_pubtypes */
    Dwarf_Bool             for_gnu_pubnames ,
    Dwarf_Gnu_Index_Head * index_head_out,
    Dwarf_Unsigned       * index_block_count_out,
    Dwarf_Error * error)
{
    Dwarf_Unsigned       count = 0;
    Dwarf_Gnu_Index_Head head = 0;
    struct Dwarf_Gnu_IBlock_s *iblock_array = 0;
    int                  res = 0;

    CHECK_DBG(dbg,error,"dwarf_get_gnu_index_head()");
    res = load_pub_section(dbg,for_gnu_pubnames,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    /*  We want this loaded for error checking by callers
        in case they had no reason to load already. */
    res = _dwarf_load_debug_info(dbg,error);
    if (res == DW_DLV_ERROR) {
        return res;
    }
    /* if count zero, returns DW_DLV_NO_ENTRY */
    res = scan_block_entries(dbg,for_gnu_pubnames,&count,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    head = (Dwarf_Gnu_Index_Head)
        _dwarf_get_alloc(dbg,DW_DLA_GNU_INDEX_HEAD,1);
    if (!head) {
        build_errm_no_v(dbg,for_gnu_pubnames,
            " Unable to allocate "
            "a header record. Out of memory creating record.",
            error);
        return DW_DLV_ERROR;
    }
    head->gi_dbg = dbg;
    head->gi_section_data = for_gnu_pubnames?
        dbg->de_debug_gnu_pubnames.dss_data:
        dbg->de_debug_gnu_pubtypes.dss_data;
    head->gi_section_length = for_gnu_pubnames?
        dbg->de_debug_gnu_pubnames.dss_size:
        dbg->de_debug_gnu_pubtypes.dss_size;
    head->gi_section_end = head->gi_section_data +
        head->gi_section_length;
    head->gi_is_pubnames = for_gnu_pubnames;
    iblock_array = calloc(count,
        sizeof(struct Dwarf_Gnu_IBlock_s));
    if (!iblock_array) {
        build_errm_one_num(dbg,for_gnu_pubnames,
            "Unable to allocate "
            " %u block records. Out of memory.",
            count,error);
        dwarf_gnu_index_dealloc(head);
        head = 0;
        return DW_DLV_ERROR;
    }
    head->gi_blockarray = iblock_array;
    head->gi_blockcount = count;
    res = fill_in_blocks(head,error);
    if (res != DW_DLV_OK) {
        dwarf_gnu_index_dealloc(head);
        head = 0;
        return res;
    }
    *index_head_out = head;
    *index_block_count_out = count;
    return DW_DLV_OK;
}

/*  Frees all resources used for the indexes. */
void
_dwarf_free_gnu_index_head_content(Dwarf_Gnu_Index_Head head)
{
    if (!head) {
        return;
    }
    if (head->gi_blockarray) {
        Dwarf_Unsigned i = 0;
        struct Dwarf_Gnu_IBlock_s *block =
            head->gi_blockarray;

        for ( ; i < head->gi_blockcount; ++i,block++) {
            if (block->ib_entryarray) {
                free(block->ib_entryarray);
                block->ib_entryarray = 0;
            }
            block->ib_entry_count = 0;
        }
        free(head->gi_blockarray);
        head->gi_blockarray = 0;
        head->gi_blockcount = 0;
    }
}

void
dwarf_gnu_index_dealloc(Dwarf_Gnu_Index_Head head)
{
    Dwarf_Debug dbg = 0;
    if (!head) {
        return;
    }
    dbg = head->gi_dbg;
    if (IS_INVALID_DBG(dbg)) {
        return;
    }
    _dwarf_free_gnu_index_head_content(head);
    dwarf_dealloc(dbg,head,DW_DLA_GNU_INDEX_HEAD);
}

void
_dwarf_gnu_index_head_destructor(void *incoming)
{
    Dwarf_Gnu_Index_Head head = 0;

    head = (Dwarf_Gnu_Index_Head)incoming;
    if (!head) {
        return;
    }
    _dwarf_free_gnu_index_head_content(head);
    return;
}

int
dwarf_get_gnu_index_block(Dwarf_Gnu_Index_Head head,
    Dwarf_Unsigned     number,
    Dwarf_Unsigned   * block_length,
    Dwarf_Half       * version,
    Dwarf_Unsigned   * offset_into_debug_info,
    Dwarf_Unsigned   * size_of_debug_info_area,
    Dwarf_Unsigned   * count_of_entries,
    Dwarf_Error      * error)
{
    struct Dwarf_Gnu_IBlock_s *gib = 0;

    if (!head) {
        _dwarf_error_string(0,error,DW_DLE_DBG_NULL,
            "DW_DLE_DBG_NULL: in "
            "dwarf_get_gnu_index_block");
        return DW_DLV_ERROR;
    }
    if (number >= head->gi_blockcount) {
        return DW_DLV_NO_ENTRY;
    }
    gib = head->gi_blockarray + number;
    if (block_length) {
        *block_length = gib->ib_block_length;
    }
    if (version) {
        *version      = gib->ib_version;
    }
    if (offset_into_debug_info) {
        *offset_into_debug_info = gib->ib_offset_in_debug_info;
    }
    if (size_of_debug_info_area) {
        *size_of_debug_info_area = gib->ib_size_in_debug_info;
    }
    if (count_of_entries) {
        *count_of_entries = gib->ib_entry_count;
    }
    return DW_DLV_OK;
}

int
dwarf_get_gnu_index_block_entry(Dwarf_Gnu_Index_Head head,
    Dwarf_Unsigned    blocknumber,
    Dwarf_Unsigned    entrynumber,
    Dwarf_Unsigned  * offset_in_debug_info,
    const char     ** name_string,
    unsigned char   * flagbyte,
    unsigned char   * staticorglobal,
    unsigned char   * typeofentry,
    Dwarf_Error     * error)
{
    struct Dwarf_Gnu_IBlock_s *gib = 0;
    struct DGI_Entry_s        *be  = 0;

    if (!head) {
        _dwarf_error_string(0,error,DW_DLE_DBG_NULL,
            "DW_DLE_DBG_NULL: in "
            "dwarf_get_gnu_index_block_entry");
        return DW_DLV_ERROR;
    }
    if (blocknumber >= head->gi_blockcount) {
        return DW_DLV_NO_ENTRY;
    }
    gib = head->gi_blockarray + blocknumber;
    if (!gib->ib_counted_entries) {
        int res = 0;

        gib->ib_counted_entries = TRUE;
        res = fill_in_entries(head,gib,error);
        if (res != DW_DLV_OK) {
            return res;
        }
    }
    if (entrynumber >= gib->ib_entry_count) {
        return DW_DLV_NO_ENTRY;
    }
    be = gib->ib_entryarray + entrynumber;

    if (offset_in_debug_info) {
        *offset_in_debug_info = be->ge_debug_info_offset;
    }
    if (name_string) {
        *name_string = be->ge_string;
    }
    if (flagbyte) {
        *flagbyte = be->ge_flag_byte;
    }
    if (staticorglobal) {
        if (be->ge_flag_byte &0x80) {
            /* value 1, of course... */
            *staticorglobal = DW_GNUIVIS_global;
        } else {
            *staticorglobal = DW_GNUIVIS_static;
        }
    }
    if (typeofentry) {
        /* DW_GNUIKIND_ */
        unsigned v = be->ge_flag_byte & 0x70;
        v = v>>4;
        *typeofentry = (unsigned char)v;
    }
    return DW_DLV_OK;
}
