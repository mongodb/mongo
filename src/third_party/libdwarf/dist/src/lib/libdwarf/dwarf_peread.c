/*
Copyright (c) 2020-2021, David Anderson All rights reserved.

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

/*  This file reads the parts of a Windows PE
    file appropriate to reading DWARF debugging data.
*/

#include <config.h>
#include <stddef.h> /* size_t */
#include <stdlib.h> /* atoi() calloc() free() malloc() */
#include <string.h> /* memset() strdup() strlen() */

#include "dwarf.h"
#include "libdwarf.h"
#include "dwarf_local_malloc.h"
#include "libdwarf_private.h"
#include "dwarf_base_types.h"
#include "dwarf_safe_strcpy.h"
#include "dwarf_opaque.h"
#include "dwarf_memcpy_swap.h"
#include "dwarf_error.h" /* for _dwarf_error() declaration */
#include "dwarf_reading.h"
#include "dwarf_object_read_common.h"
#include "dwarf_object_detector.h"
#include "dwarf_pe_descr.h"
#include "dwarf_peread.h"

#define DOS_HEADER_LEN 64

#if 0 /* for debugging. */
static void
dump_bytes(char * msg,Dwarf_Small * start, long len)
{
    Dwarf_Small *end = start + len;
    Dwarf_Small *cur = start;

    printf("%s len %ld ",msg,len);
    for (; cur < end; cur++) {
        printf("%02x ", *cur);
    }
    printf("\n");
}
#endif /*0*/

static int _dwarf_pe_object_access_init(
    int  fd,
    unsigned ftype,
    unsigned endian,
    unsigned offsetsize,
    size_t filesize,
    Dwarf_Obj_Access_Interface_a **binary_interface,
    int *localerrnum);

static unsigned long
magic_copy(char *d, unsigned len)
{
    unsigned i = 0;
    unsigned long v = 0;

    v = d[0];
    for (i = 1 ; i < len; ++i) {
        v <<= 8;
        v |=  0xff&d[i];
    }
    return v;
}
static int
check_valid_string(char *tab,
    Dwarf_Unsigned size,
    Dwarf_Unsigned startindex)
{
    Dwarf_Unsigned i = startindex;
    for ( ; i < size; ++i) {
        if (!tab[i]) {
            return DW_DLV_OK;
        }
    }
    return DW_DLV_ERROR;
}

/*  Name_array is 8 byte string, or it is supposed to be
    anyway.  */
static int
pe_section_name_get(dwarf_pe_object_access_internals_t *pep,
    const char *name_array,

    /* The name strlen */
    unsigned long size_name,
    const char ** name_out,
    int *errcode)
{
    if (name_array[0] == '/') {
        long v = 0;
        unsigned long u = 0;
        const char *s = 0;
        char temp_array[9];
        int res = 0;

        /*  The value is an integer after the /,
            and we want the value */
        _dwarf_safe_strcpy(temp_array,sizeof(temp_array),
            name_array+1,size_name-1);
        v = atoi(temp_array);
        if (v < 0) {
            *errcode = DW_DLE_STRING_OFFSET_BAD;
            return DW_DLV_ERROR;
        }
        u = v;
        if (!pep->pe_string_table) {
            *errcode = DW_DLE_STRING_OFFSET_BAD;
            return DW_DLV_ERROR;
        }
        if (u >= pep->pe_string_table_size) {
            *errcode = DW_DLE_STRING_OFFSET_BAD;
            return DW_DLV_ERROR;
        }
        res = check_valid_string(pep->pe_string_table,
            pep->pe_string_table_size,u);
        if (res != DW_DLV_OK) {
            *errcode = DW_DLE_STRING_OFFSET_BAD;
            return DW_DLV_ERROR;
        }
        s = pep->pe_string_table +u;
        *name_out = s;
        return DW_DLV_OK;
    }
    *name_out = name_array;
    return DW_DLV_OK;
}

static Dwarf_Small
pe_get_byte_order (void *obj)
{
    dwarf_pe_object_access_internals_t *pep =
        (dwarf_pe_object_access_internals_t*)(obj);
    return pep->pe_endian;
}

static Dwarf_Small
pe_get_length_size (void *obj)
{
    dwarf_pe_object_access_internals_t *pep =
        (dwarf_pe_object_access_internals_t*)(obj);
    return pep->pe_offsetsize/8;
}

static Dwarf_Unsigned
pe_get_file_size (void *obj)
{
    dwarf_pe_object_access_internals_t *pep =
        (dwarf_pe_object_access_internals_t*)(obj);
    return pep->pe_filesize;
}

static Dwarf_Small
pe_get_pointer_size (void *obj)
{
    dwarf_pe_object_access_internals_t *pep =
        (dwarf_pe_object_access_internals_t*)(obj);
    return pep->pe_pointersize/8;
}

static Dwarf_Unsigned
pe_get_section_count (void *obj)
{
    dwarf_pe_object_access_internals_t *pep =
        (dwarf_pe_object_access_internals_t*)(obj);
    return pep->pe_section_count;
}

static int
pe_get_section_info (void *obj,
    Dwarf_Unsigned section_index,
    Dwarf_Obj_Access_Section_a *return_section,
    int *error)
{
    dwarf_pe_object_access_internals_t *pep =
        (dwarf_pe_object_access_internals_t*)(obj);

    (void)error;
    if (section_index < pep->pe_section_count) {
        struct dwarf_pe_generic_image_section_header *sp = 0;
        sp = pep->pe_sectionptr + section_index;
        return_section->as_name = sp->dwarfsectname;
        return_section->as_type = 0;
        return_section->as_flags = sp->Characteristics;
        return_section->as_addr = pep->pe_OptionalHeader.ImageBase +
            sp->VirtualAddress;
        return_section->as_offset = sp->PointerToRawData;
        /*  SizeOfRawData can be rounded or truncated,
            use VirtualSize for the real analog of Elf
            section size. */
        return_section->as_size = sp->VirtualSize;
        return_section->as_link = 0;
        return_section->as_info = 0;
        return_section->as_addralign = 0;
        return_section->as_entrysize = 0;
        return DW_DLV_OK;
    }
    return DW_DLV_NO_ENTRY;
}

static int
load_optional_header32(dwarf_pe_object_access_internals_t *pep,
    Dwarf_Unsigned offset, int*errcode)
{
    int res = 0;
    IMAGE_OPTIONAL_HEADER32_dw hdr;

    pep->pe_optional_header_size = sizeof(IMAGE_OPTIONAL_HEADER32_dw);

    if ((pep->pe_optional_header_size + offset) >
        pep->pe_filesize) {
        *errcode = DW_DLE_FILE_TOO_SMALL;
        return DW_DLV_ERROR;
    }

    res =  _dwarf_object_read_random(pep->pe_fd,
        (char *)&hdr,
        offset, sizeof(IMAGE_OPTIONAL_HEADER32_dw),
        pep->pe_filesize,
        errcode);
    if (res != DW_DLV_OK) {
        return res;
    }

    /* This is a subset of fields. */
    ASNAR(pep->pe_copy_word,pep->pe_OptionalHeader.Magic,
        hdr.Magic);
    pep->pe_OptionalHeader.MajorLinkerVersion= hdr.MajorLinkerVersion;
    pep->pe_OptionalHeader.MinorLinkerVersion= hdr.MinorLinkerVersion;
    ASNAR(pep->pe_copy_word,pep->pe_OptionalHeader.ImageBase,
        hdr.ImageBase);
    ASNAR(pep->pe_copy_word,pep->pe_OptionalHeader.SizeOfCode,
        hdr.SizeOfCode);
    ASNAR(pep->pe_copy_word,pep->pe_OptionalHeader.SizeOfImage,
        hdr.SizeOfImage);
    ASNAR(pep->pe_copy_word,pep->pe_OptionalHeader.SizeOfHeaders,
        hdr.SizeOfHeaders);
    pep->pe_OptionalHeader.SizeOfDataDirEntry =
        sizeof(IMAGE_DATA_DIRECTORY_dw);
    return DW_DLV_OK;
}
static int
load_optional_header64(dwarf_pe_object_access_internals_t *pep,
    Dwarf_Unsigned offset, int*errcode )
{
    IMAGE_OPTIONAL_HEADER64_dw hdr;
    int res = 0;

    pep->pe_optional_header_size = sizeof(IMAGE_OPTIONAL_HEADER64_dw);
    if ((pep->pe_optional_header_size + offset) >
        pep->pe_filesize) {
        *errcode = DW_DLE_FILE_TOO_SMALL;
        return DW_DLV_ERROR;
    }
    res =  _dwarf_object_read_random(pep->pe_fd,
        (char *)&hdr,
        offset, sizeof(IMAGE_OPTIONAL_HEADER64_dw),
        pep->pe_filesize,
        errcode);
    if (res != DW_DLV_OK) {
        return res;
    }

    /* This is a subset of fields. */
    ASNAR(pep->pe_copy_word,pep->pe_OptionalHeader.Magic,
        hdr.Magic);
    pep->pe_OptionalHeader.MajorLinkerVersion= hdr.MajorLinkerVersion;
    pep->pe_OptionalHeader.MinorLinkerVersion= hdr.MinorLinkerVersion;
    ASNAR(pep->pe_copy_word,pep->pe_OptionalHeader.ImageBase,
        hdr.ImageBase);
    ASNAR(pep->pe_copy_word,pep->pe_OptionalHeader.SizeOfCode,
        hdr.SizeOfCode);
    ASNAR(pep->pe_copy_word,pep->pe_OptionalHeader.SizeOfImage,
        hdr.SizeOfImage);
    ASNAR(pep->pe_copy_word,pep->pe_OptionalHeader.SizeOfHeaders,
        hdr.SizeOfHeaders);
    pep->pe_OptionalHeader.SizeOfDataDirEntry =
        sizeof(IMAGE_DATA_DIRECTORY_dw);
    return DW_DLV_OK;
}

static char *boringname[] = {
".text",
".bss",
".data",
".rdata",
0
};

static int
in_name_list(char * name)
{
    int i = 0;

    if (!name) {
        return FALSE;
    }
    for ( ; ; ++i) {
        if (!boringname[i]) {
            break;
        }
        if (!strcmp(name,boringname[i])) {
            return TRUE;
        }
    }
    return FALSE;
}

static int
is_irrelevant_section(char * name,
    Dwarf_Unsigned virtsz)
{
    int res = FALSE;

    res = in_name_list(name);
    if (res) {
        return TRUE;
    }
    if (!virtsz) {
        return TRUE;
    }
    return FALSE;
}

static int
pe_load_section (void *obj, Dwarf_Unsigned section_index,
    Dwarf_Small **return_data, int *error)
{
    dwarf_pe_object_access_internals_t *pep =
        (dwarf_pe_object_access_internals_t*)(obj);

    if (0 < section_index &&
        section_index < pep->pe_section_count) {
        int res = 0;
        struct dwarf_pe_generic_image_section_header *sp =
            pep->pe_sectionptr + section_index;
        Dwarf_Unsigned read_length = 0;

        if (sp->loaded_data) {
            *return_data = sp->loaded_data;
            return DW_DLV_OK;
        }
        if (sp->section_irrelevant_to_dwarf) {
            return DW_DLV_NO_ENTRY;
        }
        if (!sp->VirtualSize) {
            return DW_DLV_NO_ENTRY;
        }
        if (sp->SizeOfRawData >= pep->pe_filesize) {
            *error = DW_DLE_PE_SECTION_SIZE_ERROR;
            return DW_DLV_ERROR;
        }
        read_length = sp->SizeOfRawData;
        if (sp->VirtualSize < read_length) {
            /* Don't read padding that wasn't allocated in memory */
            read_length = sp->VirtualSize;
        }
        if ((read_length + sp->PointerToRawData) >
            pep->pe_filesize) {
            *error = DW_DLE_FILE_TOO_SMALL;
            return DW_DLV_ERROR;
        }
        /*  VirtualSize > SizeOfRawData  if trailing zeros
            in the section were not written to disc.
            Malloc enough for the whole section, read in
            the bytes we have. */
        /*  A heuristic for corrupt data */
        if (sp->VirtualSize >= 2*pep->pe_filesize) {
            *error = DW_DLE_PE_SECTION_SIZE_ERROR;
            return DW_DLV_ERROR;
        }
        sp->loaded_data = malloc((size_t)sp->VirtualSize);
        if (!sp->loaded_data) {
            *error = DW_DLE_ALLOC_FAIL;
            return DW_DLV_ERROR;
        }
        res = _dwarf_object_read_random(pep->pe_fd,
            (char *)sp->loaded_data,
            sp->PointerToRawData, (size_t)read_length,
            pep->pe_filesize,
            error);
        if (res != DW_DLV_OK) {
            free(sp->loaded_data);
            sp->loaded_data = 0;
            return res;
        }
        if (sp->VirtualSize > read_length) {
            /*  Zero space that was allocated but
                truncated from the file */
            memset(sp->loaded_data + read_length, 0,
                (size_t)(sp->VirtualSize - read_length));
        }
        *return_data = sp->loaded_data;
        return DW_DLV_OK;
    }
    return DW_DLV_NO_ENTRY;
}

static void
_dwarf_destruct_pe_access(void* obj)
{
    struct Dwarf_Obj_Access_Interface_a_s * aip =
        (struct Dwarf_Obj_Access_Interface_a_s * )obj;
    dwarf_pe_object_access_internals_t *pep = 0;
    Dwarf_Unsigned i = 0;

    if (!aip) {
        return;
    }
    pep = (dwarf_pe_object_access_internals_t*)(aip->ai_object);
    if (pep->pe_destruct_close_fd && pep->pe_fd !=-1) {
        _dwarf_closer(pep->pe_fd);
        pep->pe_fd = -1;
    }
    free((char *)pep->pe_path);
    pep->pe_path = 0;
    if (pep->pe_sectionptr) {
        struct dwarf_pe_generic_image_section_header  *sp = 0;

        sp = pep->pe_sectionptr;
        for (i=0; i < pep->pe_section_count; ++i,++sp) {
            if (sp->loaded_data) {
                free(sp->loaded_data);
                sp->loaded_data = 0;
            }
            free(sp->name);
            sp->name = 0;
            free(sp->dwarfsectname);
            sp->dwarfsectname = 0;
        }
        free(pep->pe_sectionptr);
        pep->pe_section_count = 0;
    }
    free(pep->pe_string_table);
    pep->pe_string_table = 0;
    free(pep);
    free(aip);
    return;
}

static int
_dwarf_pe_load_dwarf_section_headers(
    dwarf_pe_object_access_internals_t *pep,int *errcode)
{
    Dwarf_Unsigned i = 0;
    Dwarf_Unsigned input_count =
        pep->pe_FileHeader.NumberOfSections;
    Dwarf_Unsigned offset_in_input = pep->pe_section_table_offset;
    Dwarf_Unsigned section_hdr_size = sizeof(IMAGE_SECTION_HEADER_dw);
    struct dwarf_pe_generic_image_section_header *sec_outp = 0;
    Dwarf_Unsigned cur_offset = offset_in_input;
    Dwarf_Unsigned past_end_hdrs = offset_in_input +
        section_hdr_size*input_count;

    /* internal sections include null initial section */
    pep->pe_section_count = input_count+1;

    if (past_end_hdrs > pep->pe_filesize) {
        *errcode = DW_DLE_FILE_TOO_SMALL;
        return DW_DLV_ERROR;
    }

    if (!offset_in_input) {
        *errcode = DW_DLE_PE_OFFSET_BAD;
        return DW_DLV_ERROR;
    }
    pep->pe_sectionptr =
        (struct dwarf_pe_generic_image_section_header * )
        calloc((size_t)pep->pe_section_count,
        sizeof(struct dwarf_pe_generic_image_section_header));
    if (!pep->pe_sectionptr) {
        *errcode = DW_DLE_ALLOC_FAIL;
        return DW_DLV_ERROR;
    }
    sec_outp = pep->pe_sectionptr;
    sec_outp->name = strdup("");
    sec_outp->dwarfsectname = strdup("");
    sec_outp++;
    for ( ;  i < input_count;
        ++i, cur_offset += section_hdr_size, sec_outp++) {

        int res = 0;
        IMAGE_SECTION_HEADER_dw filesect;
        char        safe_name[IMAGE_SIZEOF_SHORT_NAME +1];
        const char *expname = 0;
        int irrelevant = 0;

        res =  _dwarf_object_read_random(pep->pe_fd,
            (char *)&filesect,cur_offset,
            sizeof(filesect),
            pep->pe_filesize,
            errcode);
        if (res != DW_DLV_OK) {
            return res;
        }
        /*  The following is safe. filesect.Name is
            IMAGE_SIZEOF_SHORT_NAME bytes long and may
            not (not sure) have a NUL terminator. */
        _dwarf_safe_strcpy(safe_name,
            sizeof(safe_name),
            filesect.Name,
            IMAGE_SIZEOF_SHORT_NAME);
        /* Have NUL terminator now. */
        sec_outp->name = strdup(safe_name);
        res = pe_section_name_get(pep,
            safe_name,(unsigned int)strlen(safe_name),
            &expname,errcode);
        if (res != DW_DLV_OK) {
            return res;
        }
        if (expname) {
            sec_outp->dwarfsectname = strdup(expname);
        } else {
            sec_outp->dwarfsectname = strdup("<sec name missing>");
        }
        if ( !sec_outp->name || !sec_outp->dwarfsectname) {
            *errcode = DW_DLE_ALLOC_FAIL;
            return DW_DLV_ERROR;
        }
        sec_outp->SecHeaderOffset = cur_offset;
        ASNAR(pep->pe_copy_word,sec_outp->VirtualSize,
            filesect.Misc.VirtualSize);
        ASNAR(pep->pe_copy_word,sec_outp->VirtualAddress,
            filesect.VirtualAddress);
        ASNAR(pep->pe_copy_word,sec_outp->SizeOfRawData,
            filesect.SizeOfRawData);
        irrelevant = is_irrelevant_section(sec_outp->dwarfsectname,
            sec_outp->VirtualSize);
        sec_outp->section_irrelevant_to_dwarf = irrelevant;
        if (irrelevant) {
            continue;
        }
        {
            /*  A Heuristic, allowing large virtual size
                but not unlimited as we will malloc it
                later, as Virtualsize. */
            Dwarf_Unsigned limit = 100*pep->pe_filesize;
            if (limit < pep->pe_filesize) {
                /* An overflow. Bad. */
                *errcode = DW_DLE_PE_SECTION_SIZE_HEURISTIC_FAIL;
                return DW_DLV_ERROR;
            }
            if (sec_outp->VirtualSize >
                ((Dwarf_Unsigned)2000*
                (Dwarf_Unsigned)1000*
                (Dwarf_Unsigned)1000) &&
                (sec_outp->VirtualSize > pep->pe_filesize)) {
                /*  Likely unreasonable.
                    the hard limit written this way
                    simply for clarity.
                    Hard to know what to set it to. */
                *errcode = DW_DLE_PE_SECTION_SIZE_HEURISTIC_FAIL;
                return DW_DLV_ERROR;
            }
            if (sec_outp->VirtualSize > limit &&
                0 == pep->pe_is_64bit ) {
                /* Likely totally unreasonable. Bad. */
                *errcode = DW_DLE_PE_SECTION_SIZE_HEURISTIC_FAIL;
                return DW_DLV_ERROR;
            }
        }
        ASNAR(pep->pe_copy_word,sec_outp->PointerToRawData,
            filesect.PointerToRawData);
        if (sec_outp->SizeOfRawData > pep->pe_filesize ||
            sec_outp->PointerToRawData > pep->pe_filesize ||
            (sec_outp->SizeOfRawData+
                sec_outp->PointerToRawData > pep->pe_filesize)) {
            *errcode = DW_DLE_FILE_TOO_SMALL;
            return DW_DLV_ERROR;
        }
        ASNAR(pep->pe_copy_word,sec_outp->PointerToRelocations,
            filesect.PointerToRelocations);
        ASNAR(pep->pe_copy_word,sec_outp->PointerToLinenumbers,
            filesect.PointerToLinenumbers);
        ASNAR(pep->pe_copy_word,sec_outp->NumberOfRelocations,
            filesect.NumberOfRelocations);
        ASNAR(pep->pe_copy_word,sec_outp->NumberOfLinenumbers,
            filesect.NumberOfLinenumbers);
        ASNAR(pep->pe_copy_word,sec_outp->Characteristics,
            filesect.Characteristics);
        /* sec_outp->loaded data set when we load a section */
    }
    return DW_DLV_OK;
}

static int
_dwarf_load_pe_sections(
    dwarf_pe_object_access_internals_t *pep,int *errcode)
{
    struct dos_header_dw dhinmem;
    IMAGE_FILE_HEADER_dw ifh;
    void (*word_swap) (void *, const void *, unsigned long);
    unsigned locendian = 0;
    int res = 0;
    Dwarf_Unsigned dos_sig = 0;
    Dwarf_Unsigned nt_address = 0;
    char nt_sig_array[4];
    unsigned long nt_signature = 0;

    if ( (sizeof(ifh) + sizeof(dhinmem))  >= pep->pe_filesize) {
        /* corrupt object. */
        *errcode = DW_DLE_PE_SIZE_SMALL;
        return DW_DLV_ERROR;
    }
    res = _dwarf_object_read_random(pep->pe_fd,(char *)&dhinmem,
        0, sizeof(dhinmem),pep->pe_filesize, errcode);
    if (res != DW_DLV_OK) {
        return res;
    }
    dos_sig = magic_copy((char *)dhinmem.dh_mz,
        sizeof(dhinmem.dh_mz));
    if (dos_sig == IMAGE_DOS_SIGNATURE_dw) {
        /*  IMAGE_DOS_SIGNATURE_dw assumes bytes
            reversed by little-endian
            load, so we intrepet a match the other way. */
        /* BIG ENDIAN. From looking at hex characters in object  */
#ifdef WORDS_BIGENDIAN
        word_swap = _dwarf_memcpy_noswap_bytes;
#else  /* LITTLE ENDIAN */
        word_swap = _dwarf_memcpy_swap_bytes;
#endif /* LITTLE- BIG-ENDIAN */
        locendian = DW_END_big;
    } else if (dos_sig == IMAGE_DOS_REVSIGNATURE_dw) {
        /* raw load, so  intrepet a match the other way. */
        /* LITTLE ENDIAN */
#ifdef WORDS_BIGENDIAN
        word_swap = _dwarf_memcpy_swap_bytes;
#else  /* LITTLE ENDIAN */
        word_swap = _dwarf_memcpy_noswap_bytes;
#endif /* LITTLE- BIG-ENDIAN */
        locendian = DW_END_little;
    } else {
        /* Not dos header not a PE file we recognize */
        *errcode = DW_DLE_FILE_WRONG_TYPE;
        return DW_DLV_ERROR;
    }
    if (locendian != pep->pe_endian) {
        /*  Really this is a coding botch somewhere here,
            not an object corruption. */
        *errcode = DW_DLE_FILE_WRONG_TYPE;
        return DW_DLV_ERROR;
    }
    pep->pe_copy_word = word_swap;
    ASNAR(word_swap,nt_address,dhinmem.dh_image_offset);
    if (pep->pe_filesize < (nt_address + sizeof(nt_sig_array))) {
        /*  The nt_address is really a file offset. */
        *errcode = DW_DLE_FILE_TOO_SMALL;
        /* Not dos header not a PE file we recognize */
        return DW_DLV_ERROR;
    }

    res =  _dwarf_object_read_random(pep->pe_fd,
        (char *)&nt_sig_array[0],
        nt_address, sizeof(nt_sig_array),
        pep->pe_filesize,errcode);
    if (res != DW_DLV_OK) {
        return res;
    }
    {   unsigned long lsig = 0;

        ASNAR(word_swap,lsig,nt_sig_array);
        nt_signature = lsig;
    }
    if (nt_signature != IMAGE_NT_SIGNATURE_dw) {
        *errcode = DW_DLE_FILE_WRONG_TYPE;
        return DW_DLV_ERROR;
    }

    pep->pe_nt_header_offset = nt_address  + SIZEOFT32;
    if (pep->pe_filesize < (pep->pe_nt_header_offset +
        sizeof(ifh))) {
        *errcode = DW_DLE_FILE_TOO_SMALL;
        /* Not image header not a PE file we recognize */
        return DW_DLV_ERROR;
    }
    res = _dwarf_object_read_random(pep->pe_fd,(char *)&ifh,
        pep->pe_nt_header_offset, sizeof(ifh),
        pep->pe_filesize,errcode);
    if (res != DW_DLV_OK) {
        return res;
    }
    ASNAR(word_swap,pep->pe_FileHeader.Machine,ifh.Machine);
    ASNAR(word_swap,pep->pe_FileHeader.NumberOfSections,
        ifh.NumberOfSections);
    ASNAR(word_swap,pep->pe_FileHeader.TimeDateStamp,
        ifh.TimeDateStamp);
    ASNAR(word_swap,pep->pe_FileHeader.PointerToSymbolTable,
        ifh.PointerToSymbolTable);
    ASNAR(word_swap,pep->pe_FileHeader.NumberOfSymbols,
        ifh.NumberOfSymbols);
    ASNAR(word_swap,pep->pe_FileHeader.SizeOfOptionalHeader,
        ifh.SizeOfOptionalHeader);
    ASNAR(word_swap,pep->pe_FileHeader.Characteristics,
        ifh.Characteristics);
    pep->pe_machine = pep->pe_FileHeader.Machine;
    pep->pe_flags = pep->pe_FileHeader.Characteristics;
    pep->pe_optional_header_offset = pep->pe_nt_header_offset+
        sizeof(ifh);
    if (pep->pe_offsetsize == 32) {
        res = load_optional_header32(pep,
            pep->pe_optional_header_offset,errcode);
        pep->pe_optional_header_size =
            sizeof(IMAGE_OPTIONAL_HEADER32_dw);
    } else if (pep->pe_offsetsize == 64) {
        res = load_optional_header64(pep,
            pep->pe_optional_header_offset,errcode);
        pep->pe_optional_header_size =
            sizeof(IMAGE_OPTIONAL_HEADER64_dw);
    } else {
        *errcode = DW_DLE_OFFSET_SIZE;
        return DW_DLV_ERROR;
    }
    if (res != DW_DLV_OK) {
        return res;
    }

    pep->pe_section_table_offset = pep->pe_optional_header_offset
        + pep->pe_optional_header_size;
    pep->pe_symbol_table_offset =
        pep->pe_FileHeader.PointerToSymbolTable;
    if (pep->pe_symbol_table_offset >= pep->pe_filesize) {
        *errcode = DW_DLE_OFFSET_SIZE;
        return DW_DLV_ERROR;
    }
    if (pep->pe_symbol_table_offset) {
        pep->pe_string_table_offset  =
            pep->pe_symbol_table_offset +
            (pep->pe_FileHeader.NumberOfSymbols *
            IMAGE_SIZEOF_SYMBOL);
    }

    if (pep->pe_string_table_offset >= pep->pe_filesize) {
        *errcode = DW_DLE_OFFSET_SIZE;
        pep->pe_string_table_size = 0;
        return DW_DLV_ERROR;
    }
    if (pep->pe_string_table_offset) {
        /*  https://docs.microsoft.com/en-us/\
            windows/desktop/debug/pe-format#coff-string-table  */
        /* The first 4 bytes of the string table contain
            the size of the string table. */
        char size_field[4];

        if ((pep->pe_string_table_offset+sizeof(size_field)) >
            pep->pe_filesize) {
            *errcode = DW_DLE_FILE_TOO_SMALL;
            return DW_DLV_ERROR;
        }
        memset(size_field,0,sizeof(size_field));
        res =  _dwarf_object_read_random(pep->pe_fd,
            (char *)size_field, pep->pe_string_table_offset,
            sizeof(size_field),
            pep->pe_filesize,errcode);
        if (res != DW_DLV_OK) {
            return res;
        }
        ASNAR(pep->pe_copy_word,pep->pe_string_table_size,
            size_field);
        if (pep->pe_string_table_size >= pep->pe_filesize ) {
            *errcode = DW_DLE_PE_OFFSET_BAD;
            return DW_DLV_ERROR;
        }
        if ((pep->pe_string_table_offset+pep->pe_string_table_size) >
            pep->pe_filesize) {
            *errcode = DW_DLE_FILE_TOO_SMALL;
            return DW_DLV_ERROR;
        }
        /*  size+1 to ensure there is a terminating null character
            in memory so CoverityScan knows there is always a
            final null.  CoverityScan is not aware
            there may be multiple strings in the table.
            If there is a compiler bug the final string
            might be missing its intended null terminator! */
        pep->pe_string_table =
            (char *)calloc(1,(size_t)pep->pe_string_table_size+1);
        if (!pep->pe_string_table) {
            *errcode = DW_DLE_ALLOC_FAIL;
            return DW_DLV_ERROR;
        }
        res = _dwarf_object_read_random(pep->pe_fd,
            (char *)pep->pe_string_table,
            pep->pe_string_table_offset,
            (size_t)pep->pe_string_table_size,
            pep->pe_filesize,errcode);
        if (res != DW_DLV_OK) {
            free(pep->pe_string_table);
            pep->pe_string_table = 0;
            return res;
        }
        /*  Should pass coverity now. */
        pep->pe_string_table[pep->pe_string_table_size] = 0;
    }
    res = _dwarf_pe_load_dwarf_section_headers(pep,errcode);
    return res;
}

int
_dwarf_pe_setup(int fd,
    char *true_path,
    unsigned ftype,
    unsigned endian,
    unsigned offsetsize,
    size_t filesize,
    unsigned groupnumber,
    Dwarf_Handler errhand,
    Dwarf_Ptr errarg,
    Dwarf_Debug *dbg,Dwarf_Error *error)
{
    Dwarf_Obj_Access_Interface_a *binary_interface = 0;
    dwarf_pe_object_access_internals_t *pep = 0;
    int res = DW_DLV_OK;
    int localerrnum = 0;

    res = _dwarf_pe_object_access_init(
        fd,
        ftype,endian,offsetsize,filesize,
        &binary_interface,
        &localerrnum);
    if (res != DW_DLV_OK) {
        if (res == DW_DLV_NO_ENTRY) {
            return res;
        }
        _dwarf_error(NULL, error, localerrnum);
        return DW_DLV_ERROR;
    }
    /*  allocates and initializes Dwarf_Debug,
        generic code */
    res = dwarf_object_init_b(binary_interface, errhand, errarg,
        groupnumber, dbg, error);
    if (res != DW_DLV_OK){
        _dwarf_destruct_pe_access(binary_interface);
        return res;
    }
    pep = binary_interface->ai_object;
    (*dbg)->de_obj_flags = pep->pe_flags;
    (*dbg)->de_obj_machine = pep->pe_machine;
    pep->pe_path = strdup(true_path);
    return res;
}

static Dwarf_Obj_Access_Methods_a pe_methods = {
    pe_get_section_info,
    pe_get_byte_order,
    pe_get_length_size,
    pe_get_pointer_size,
    pe_get_file_size,
    pe_get_section_count,
    pe_load_section,
    0 /* ignore pe relocations. */,
    0 /* Not allowing use of mmap */,
    _dwarf_destruct_pe_access
};

/* On any error this frees internals. */
static int
_dwarf_pe_object_access_internals_init(
    dwarf_pe_object_access_internals_t * internals,
    int  fd,
    unsigned ftype,
    unsigned endian,
    unsigned offsetsize,
    size_t filesize,
    int *errcode)
{
    dwarf_pe_object_access_internals_t * intfc = internals;
    struct Dwarf_Obj_Access_Interface_a_s *localdoas = 0;
    int res = 0;

    /*  Must malloc as _dwarf_destruct_pe_access()
        forces that due to other uses. */
    localdoas = (struct Dwarf_Obj_Access_Interface_a_s *)
        malloc(sizeof(struct Dwarf_Obj_Access_Interface_a_s));
    if (!localdoas) {
        free(internals);
        *errcode = DW_DLE_ALLOC_FAIL;
        return DW_DLV_ERROR;
    }
    memset(localdoas,0,sizeof(struct Dwarf_Obj_Access_Interface_a_s));
    intfc->pe_ident[0]    = 'P';
    intfc->pe_ident[1]    = '1';
    intfc->pe_fd          = fd;
    intfc->pe_is_64bit    = ((offsetsize==64)?TRUE:FALSE);
    intfc->pe_offsetsize  = offsetsize;
    intfc->pe_pointersize = offsetsize;
    intfc->pe_filesize    = filesize;
    intfc->pe_ftype       = ftype;
    /* pe_path set by caller */

#ifdef WORDS_BIGENDIAN
    if (endian == DW_END_little) {
        intfc->pe_copy_word = _dwarf_memcpy_swap_bytes;
        intfc->pe_endian = DW_END_little;
    } else {
        intfc->pe_copy_word = _dwarf_memcpy_noswap_bytes;
        intfc->pe_endian = DW_END_big;
    }
#else  /* LITTLE ENDIAN */
    if (endian == DW_END_little) {
        intfc->pe_copy_word = _dwarf_memcpy_noswap_bytes;
        intfc->pe_endian = DW_END_little;
    } else {
        intfc->pe_copy_word = _dwarf_memcpy_swap_bytes;
        intfc->pe_endian = DW_END_big;
    }
#endif /* LITTLE- BIG-ENDIAN */
    res = _dwarf_load_pe_sections(intfc,errcode);
    if (res != DW_DLV_OK) {
        localdoas->ai_object = intfc;
        localdoas->ai_methods = 0;
        _dwarf_destruct_pe_access(localdoas);
        localdoas = 0;
        return res;
    }
    free(localdoas);
    localdoas = 0;
    return DW_DLV_OK;
}

static int
_dwarf_pe_object_access_init(
    int  fd,
    unsigned ftype,
    unsigned endian,
    unsigned offsetsize,
    size_t filesize,
    Dwarf_Obj_Access_Interface_a **binary_interface,
    int *localerrnum)
{

    int res = 0;
    dwarf_pe_object_access_internals_t *internals = 0;
    Dwarf_Obj_Access_Interface_a *intfc = 0;

    internals = malloc(sizeof(dwarf_pe_object_access_internals_t));
    if (!internals) {
        *localerrnum = DW_DLE_ALLOC_FAIL;
        /* Impossible case, we hope. Give up. */
        return DW_DLV_ERROR;
    }
    memset(internals,0,sizeof(*internals));
    res = _dwarf_pe_object_access_internals_init(internals,
        fd,
        ftype, endian, offsetsize, filesize,
        localerrnum);
    if (res != DW_DLV_OK){
        /* *err is already set. and the call freed internals */
        return DW_DLV_ERROR;
    }

    intfc = malloc(sizeof(Dwarf_Obj_Access_Interface_a));
    if (!intfc) {
        /* Impossible case, we hope. Give up. */
        free(internals);
        *localerrnum = DW_DLE_ALLOC_FAIL;
        return DW_DLV_ERROR;
    }
    /* Initialize the interface struct */
    intfc->ai_object = internals;
    intfc->ai_methods = &pe_methods;
    *binary_interface = intfc;
    return DW_DLV_OK;
}
