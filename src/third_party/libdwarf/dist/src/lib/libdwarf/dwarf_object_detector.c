/*
Copyright (c) 2018-2020, David Anderson All rights reserved.

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

#include <stdlib.h> /* free() */
#include <stdio.h>  /* SEEK_END SEEK_SET */
#include <string.h> /* memset() strlen() */

#include "dwarf.h"
#include "libdwarf.h"
#include "dwarf_local_malloc.h"
#include "libdwarf_private.h"
#include "dwarf_base_types.h"
#include "dwarf_opaque.h"
#include "dwarf_memcpy_swap.h"
#include "dwarf_object_read_common.h"
#include "dwarf_object_detector.h"
#include "dwarf_macho_loader.h"
#include "dwarf_string.h"

/*  TYP, SIZEOFT32 and ASNAR
    mean we can use correctly-sized arrays of char for the
    struct members instead of determining a proper integer
    that size.

    We are dealing with carefully constructed structs
    that do not have any alignment-forced (hidden)
    unused bytes so reading lengths from the real structs
    works for each variable.  */

#define TYP(n,l) char (n)[(l)]
#define SIZEOFT32 4

#define DW_DLV_NO_ENTRY -1
#define DW_DLV_OK        0
#define DW_DLV_ERROR     1

#ifndef EI_NIDENT
#define EI_NIDENT 16
#define EI_CLASS  4
#define EI_DATA   5
#define EI_VERSION 6
#define ELFCLASS32 1
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2
#endif /* EI_NIDENT */

#define DSYM_SUFFIX ".dSYM/Contents/Resources/DWARF/"
#define PATHSIZE 2000

#ifndef  MH_MAGIC
/* mach-o 32bit */
#define MH_MAGIC        0xfeedface
#define MH_CIGAM        0xcefaedfe
#endif /*  MH_MAGIC */
#ifndef  MH_MAGIC_64
/* mach-o 64bit */
#define MH_MAGIC_64 0xfeedfacf
#define MH_CIGAM_64 0xcffaedfe
#endif /*  MH_MAGIC_64 */

/* A flag not public to users. */
static int _dwarf_global_debuglink_crc_suppress;

#if 0 /* debugging only */
/* debugging only */
static void
dump_bytes(char * msg,Dwarf_Small * start, long len)
{
    Dwarf_Small *end = start + len;
    Dwarf_Small *cur = start;

    printf("%s ",msg);
    for (; cur < end; cur++) {
        printf("%02x ", *cur);
    }
    printf("\n");
}
#endif

int
dwarf_suppress_debuglink_crc(int dw_suppress)
{
    int old = _dwarf_global_debuglink_crc_suppress;
    _dwarf_global_debuglink_crc_suppress = dw_suppress;
    return old;
}

int _dwarf_get_suppress_debuglink_crc(void)
{
    return _dwarf_global_debuglink_crc_suppress;
}

static unsigned long
magic_copy(unsigned char *d, unsigned len)
{
    unsigned i = 0;
    unsigned long v = 0;

    v = d[0];
    for (i = 1 ; i < len; ++i) {
        v <<= 8;
        v |=  d[i];
    }
    return v;
}

#define EI_NIDENT 16
/* An incomplete elf header, good for 32 and 64bit elf */
struct elf_header {
    unsigned char  e_ident[EI_NIDENT];
    TYP(e_type,2);
    TYP(e_machine,2);
    TYP(e_version,4);
};

/*  Windows. Certain PE objects.
    The following references may be of interest.
https://msdn.microsoft.com/library/windows/\
desktop/ms680547(v=vs.85).aspx
#PE format overview and various machine magic numbers

https://msdn.microsoft.com/en-us/library/\
ms809762.aspx
# describes some details of PE headers, basically an overview

https://msdn.microsoft.com/en-us/library/\
windows/desktop/aa383751(v=vs.85).aspx
#defines sizes of various types

https://msdn.microsoft.com/fr-fr/library/\
windows/desktop/ms680313(v=vs.85).aspx
#defines IMAGE_FILE_HEADER and Machine fields (32/64)

https://msdn.microsoft.com/fr-fr/library/\
windows/desktop/ms680305(v=vs.85).aspx
#defines IMAGE_DATA_DIRECTORY

https://msdn.microsoft.com/en-us/library/\
windows/desktop/ms680339(v=vs.85).aspx
#Defines IMAGE_OPTIONAL_HEADER and some magic numbers

https://msdn.microsoft.com/fr-fr/library/\
windows/desktop/ms680336(v=vs.85).aspx
# defines _IMAGE_NT_HEADERS 32 64

https://msdn.microsoft.com/en-us/library/\
windows/desktop/ms680341(v=vs.85).aspx
# defines _IMAGE_SECTION_HEADER

*/

/* ===== START pe structures */

struct dos_header {
    TYP(dh_mz,2);
    TYP(dh_dos_data,58);
    TYP(dh_image_offset,4);
};

#define IMAGE_DOS_SIGNATURE_dw      0x5A4D
#define IMAGE_DOS_REVSIGNATURE_dw   0x4D5A
#define IMAGE_NT_SIGNATURE_dw       0x00004550
#define IMAGE_FILE_MACHINE_I386_dw  0x14c
#define IMAGE_FILE_MACHINE_IA64_dw  0x200
#define IMAGE_FILE_MACHINE_AMD64_dw 0x8664

struct pe_image_file_header {
    TYP(im_machine,2);
    TYP(im_sectioncount,2);
    TYP(im_ignoring,(3*4));
    TYP(im_opt_header_size,2);
    TYP(im_ignoringb,2);
};

/* ===== END pe structures */

/*  For following MacOS file naming convention */
static const char *
getseparator (const char *f)
{
    const char *p = 0;
    const char *q = 0;
    char c = 0;;

    p = NULL;
    q = f;
    do  {
        c = *q++;
        if (c == '\\' || c == '/' || c == ':') {
            p = q;
        }
    } while (c);
    return p;
}

static const char *
getbasename (const char *f)
{
    const char *pseparator = getseparator (f);
    if (!pseparator) {
        return f;
    }
    return pseparator;
}

/*  Not a standard function. */
static int
dw_stpcpy(char *dest,const char *src,char **destend, char *endpoint)
{
    const char *cp = src;
    char *dp = dest;

    for ( ; *cp; ++cp,++dp) {
        if (dp >= endpoint) {
            return DW_DLV_ERROR;
        }
        *dp = *cp;
    }
    if (dp >= endpoint) {
        return DW_DLV_ERROR;
    }
    *dp = 0;
    *destend = dp;
    return DW_DLV_OK;
}

/* This started like Elf, so check initial fields. */
static int
fill_in_elf_fields(struct elf_header *h,
    unsigned *endian,
    /*  Size of the object file offsets, not DWARF offset
        size. */
    unsigned *objoffsetsize,
    int *errcode)
{
    unsigned locendian = 0;
    unsigned locoffsetsize = 0;

    switch(h->e_ident[EI_CLASS]) {
    case ELFCLASS32:
        locoffsetsize = 32;
        break;
    case ELFCLASS64:
        locoffsetsize = 64;
        break;
    default:
        *errcode = DW_DLE_ELF_CLASS_BAD;
        return DW_DLV_ERROR;
    }
    switch(h->e_ident[EI_DATA]) {
    case ELFDATA2LSB:
        locendian = DW_END_little;
        break;
    case ELFDATA2MSB:
        locendian = DW_END_big;
        break;
    default:
        *errcode = DW_DLE_ELF_ENDIAN_BAD;
        return DW_DLV_ERROR;
    }
    if (h->e_ident[EI_VERSION] != 1 /* EV_CURRENT */) {
        *errcode = DW_DLE_ELF_VERSION_BAD;
        return DW_DLV_ERROR;
    }
    *endian = locendian;
    *objoffsetsize = locoffsetsize;
    return DW_DLV_OK;
}
static char archive_magic[8] = {
'!','<','a','r','c','h','>',0x0a
};
static int
is_archive_magic(struct elf_header *h) {
    int i = 0;
    int len = sizeof(archive_magic);
    const char *cp = (const char *)h;
    for ( ; i < len; ++i) {
        if (cp[i] != archive_magic[i]) {
            return FALSE;
        }
    }
    return TRUE;
}

/*  A bit unusual in that it always sets *is_pe_flag
    Return of DW_DLV_OK  it is a PE file we recognize. */
static int
is_pe_object(int fd,
    Dwarf_Unsigned filesize,
    unsigned *endian,
    unsigned *offsetsize,
    int *errcode)
{
    unsigned dos_sig = 0;
    unsigned locendian = 0;
    void (*word_swap) (void *, const void *, unsigned long);
    unsigned long nt_address = 0;
    struct dos_header dhinmem;
    char nt_sig_array[4];
    unsigned long nt_sig = 0;
    struct pe_image_file_header ifh;
    int res = 0;

    if (filesize < (sizeof (struct dos_header) +
        SIZEOFT32 + sizeof(struct pe_image_file_header))) {
        *errcode = DW_DLE_FILE_TOO_SMALL;
        return DW_DLV_ERROR;
    }
    res = _dwarf_object_read_random(fd,(char *)&dhinmem,
        0,sizeof(dhinmem),filesize,errcode);
    if (res != DW_DLV_OK) {
        return res;
    }
    /* No swap here, want it as in the file */
    dos_sig = magic_copy((unsigned char *)dhinmem.dh_mz,
        sizeof(dhinmem.dh_mz));
    if (dos_sig == IMAGE_DOS_SIGNATURE_dw) {
        /*  IMAGE_DOS_SIGNATURE_dw assumes bytes
            reversed by little-endian
            load, so we intrepet a match the other way. */
        /* BIG ENDIAN. From looking at hex characters in object  */
#ifdef WORDS_BIGENDIAN
        word_swap = _dwarf_memcpy_noswap_bytes;
#else  /* LITTLE ENDIAN */
        word_swap =  _dwarf_memcpy_swap_bytes;
#endif /* LITTLE- BIG-ENDIAN */
        locendian = DW_END_big;
    } else if (dos_sig == IMAGE_DOS_REVSIGNATURE_dw) {
        /* raw load, so  intrepet a match the other way. */
        /* LITTLE ENDIAN */
#ifdef WORDS_BIGENDIAN
        word_swap =  _dwarf_memcpy_swap_bytes;
#else  /* LITTLE ENDIAN */
        word_swap = _dwarf_memcpy_noswap_bytes;
#endif /* LITTLE- BIG-ENDIAN */
        locendian = DW_END_little;
    } else {
        /* Not dos header not a PE file we recognize */
        *errcode = DW_DLE_FILE_WRONG_TYPE;
        return DW_DLV_ERROR;
    }
    ASNAR(word_swap,nt_address, dhinmem.dh_image_offset);
    if (filesize < nt_address) {
        /* Not dos header not a PE file we recognize */
        *errcode = DW_DLE_FILE_TOO_SMALL;
        return DW_DLV_ERROR;
    }
    if (filesize < (nt_address + SIZEOFT32 +
        sizeof(struct pe_image_file_header))) {
        *errcode = DW_DLE_FILE_TOO_SMALL;
        /* Not dos header not a PE file we recognize */
        return DW_DLV_ERROR;
    }
    res =  _dwarf_object_read_random(fd,(char *)&nt_sig_array[0],
        nt_address, sizeof(nt_sig_array),filesize,errcode);
    if (res != DW_DLV_OK) {
        return res;
    }
    {   unsigned long lsig = 0;

        ASNAR(word_swap,lsig,nt_sig_array);
        nt_sig = lsig;
    }
    if (nt_sig != IMAGE_NT_SIGNATURE_dw) {
        *errcode = DW_DLE_FILE_WRONG_TYPE;
        return DW_DLV_ERROR;
    }
    res = _dwarf_object_read_random(fd,(char *)&ifh,
        nt_address + SIZEOFT32,
        sizeof(struct pe_image_file_header),
        filesize,
        errcode);
    if (res != DW_DLV_OK) {
        return res;
    }
    {
        unsigned long machine = 0;

        ASNAR(word_swap,machine,ifh.im_machine);
        switch(machine) {
        case IMAGE_FILE_MACHINE_I386_dw:
            *offsetsize = 32;
            *endian = locendian;
            return DW_DLV_OK;
        case IMAGE_FILE_MACHINE_IA64_dw:
        case IMAGE_FILE_MACHINE_AMD64_dw:
            *offsetsize = 64;
            *endian = locendian;
            return DW_DLV_OK;
        default: break;
        }
    }
    *errcode = DW_DLE_IMAGE_FILE_UNKNOWN_TYPE;
    return DW_DLV_ERROR;
}

static int
is_mach_o_universal(struct elf_header *h,
    unsigned *endian,
    unsigned *offsetsize)
{
    unsigned long magicval = 0;
    unsigned locendian = 0;
    unsigned locoffsetsize = 0;

    /*  No swapping here. Need to match size of
        the universal-object  magic field. */
    magicval = magic_copy(h->e_ident,4);
    if (magicval == FAT_MAGIC) {
        locendian = DW_END_big;
        locoffsetsize = 32;
    } else if (magicval == FAT_CIGAM) {
        locendian = DW_END_little;
        locoffsetsize = 32;
    }else if (magicval == FAT_MAGIC_64) {
        locendian = DW_END_big;
        locoffsetsize = 64;
    } else if (magicval == FAT_CIGAM_64) {
        locendian = DW_END_little;
        locoffsetsize = 64;
    } else {
        return FALSE;
    }
    *endian = locendian;
    *offsetsize = locoffsetsize;
    return TRUE;
}

static int
is_mach_o_magic(struct elf_header *h,
    unsigned *endian,
    unsigned *offsetsize)
{
    unsigned long magicval = 0;
    unsigned locendian = 0;
    unsigned locoffsetsize = 0;

    /*  No swapping here. Need to match size of
        Mach-o magic field. */
    magicval = magic_copy(h->e_ident,4);
    if (magicval == MH_MAGIC) {
        locendian = DW_END_big;
        locoffsetsize = 32;
    } else if (magicval == MH_CIGAM) {
        locendian = DW_END_little;
        locoffsetsize = 32;
    }else if (magicval == MH_MAGIC_64) {
        locendian = DW_END_big;
        locoffsetsize = 64;
    } else if (magicval == MH_CIGAM_64) {
        locendian = DW_END_little;
        locoffsetsize = 64;
    } else {
        return FALSE;
    }
    *endian = locendian;
    *offsetsize = locoffsetsize;
    return TRUE;
}

/*  This is part of the public API. Not usable
    to detect inner binaries from
    a MacOS universal binary. */
int
dwarf_object_detector_fd(int fd,
    unsigned *ftype,
    unsigned *endian,
    unsigned *offsetsize,
    Dwarf_Unsigned  *filesize,
    int *errcode)
{
    Dwarf_Unsigned fileoffsetbase = 0;
    int res = 0;

    res = _dwarf_object_detector_fd_a(fd,
        ftype,endian,offsetsize,
        fileoffsetbase,filesize,
        errcode);
    return res;
}

int
_dwarf_object_detector_fd_a(int fd,
    unsigned *ftype,
    unsigned *endian,
    unsigned *offsetsize,
    Dwarf_Unsigned fileoffsetbase,
    Dwarf_Unsigned  *filesize_out,
    int *errcode)
{
    struct elf_header h;
    Dwarf_Unsigned readlen = sizeof(h);
    Dwarf_Unsigned filesize = 0;
    Dwarf_Unsigned remaininglen  = 0;
    int            res = 0;

    {
        res = _dwarf_seekr(fd,0,SEEK_END,&filesize);
        if (res != DW_DLV_OK) {
            *errcode = DW_DLE_SEEK_ERROR;
            return DW_DLV_ERROR;
        }
    }
    if (filesize <= readlen) {
        /* Not a real object file */
        *errcode = DW_DLE_FILE_TOO_SMALL;
        return DW_DLV_ERROR;
    }
    if (filesize <= fileoffsetbase) {
        *errcode = DW_DLE_SEEK_ERROR;
        return DW_DLV_ERROR;
    }
    remaininglen = filesize - fileoffsetbase;
    if (remaininglen <= readlen) {
        /* Not a real object file */
        *errcode = DW_DLE_FILE_TOO_SMALL;
        return DW_DLV_ERROR;
    }
    /*  fileoffsetbase is non zero iff we have
        an Apple Universal Binary. */
    res = _dwarf_object_read_random(fd, (char *)&h,
        fileoffsetbase,
        readlen, filesize,errcode);
    if (h.e_ident[0] == 0x7f &&
        h.e_ident[1] == 'E' &&
        h.e_ident[2] == 'L' &&
        h.e_ident[3] == 'F') {
        /* is ELF */

        res = fill_in_elf_fields(&h,endian,offsetsize,errcode);
        if (res != DW_DLV_OK) {
            return res;
        }
        *ftype = DW_FTYPE_ELF;
        *filesize_out = (Dwarf_Unsigned)filesize;
        return DW_DLV_OK;
    }
    if (is_mach_o_universal(&h,endian,offsetsize)) {
        *ftype = DW_FTYPE_APPLEUNIVERSAL;
        *filesize_out = (Dwarf_Unsigned)filesize;
        return DW_DLV_OK;
    }
    if (is_mach_o_magic(&h,endian,offsetsize)) {
        *ftype = DW_FTYPE_MACH_O;
        *filesize_out = (Dwarf_Unsigned)filesize;
        return DW_DLV_OK;
    }
    if (is_archive_magic(&h)) {
        *ftype = DW_FTYPE_ARCHIVE;
        *filesize_out = (Dwarf_Unsigned)filesize;
        return DW_DLV_OK;
    }
    res = is_pe_object(fd,filesize,endian,offsetsize,errcode);
    if (res == DW_DLV_OK ) {
        *ftype = DW_FTYPE_PE;
        *filesize_out = (Dwarf_Unsigned)filesize;
        return DW_DLV_OK;
    }
    /* Unknown object format. */
    return DW_DLV_NO_ENTRY;
}

int
dwarf_object_detector_path_dSYM(
    const char  *path,
    char *outpath, unsigned long outpath_len,
    char ** gl_pathnames,
    unsigned gl_pathcount,
    unsigned *ftype,
    unsigned *endian,
    unsigned *offsetsize,
    Dwarf_Unsigned  *filesize,
    unsigned char *pathsource,
    int *errcode)
{
    char *cp = 0;
    size_t plen = strlen(path);
    size_t dsprefixlen = sizeof(DSYM_SUFFIX);
    int fd = -1;
    int res = 0;
    int have_outpath = outpath && outpath_len;

    (void)gl_pathnames;
    (void)gl_pathcount;
    if (have_outpath) {
        /*   Looking for MacOS dSYM */
        if ((2*plen + dsprefixlen +2) >= (size_t)outpath_len) {
            *errcode =  DW_DLE_PATH_SIZE_TOO_SMALL;
            return DW_DLV_ERROR;
        }
        res = dw_stpcpy(outpath,path,&cp,outpath+outpath_len);
        if (res == DW_DLV_ERROR) {
            *errcode =  DW_DLE_PATH_SIZE_TOO_SMALL;
            return DW_DLV_ERROR;
        }
        res = dw_stpcpy(cp,DSYM_SUFFIX,&cp,outpath+outpath_len);
        if (res == DW_DLV_ERROR) {
            *errcode =  DW_DLE_PATH_SIZE_TOO_SMALL;
            return DW_DLV_ERROR;
        }
        res= dw_stpcpy(cp,getbasename(path),&cp,outpath+outpath_len);
        if (res == DW_DLV_ERROR) {
            *errcode =  DW_DLE_PATH_SIZE_TOO_SMALL;
            return DW_DLV_ERROR;
        }
        fd = _dwarf_openr(outpath);
        if (fd < 0) {
            outpath[0] = 0;
            return DW_DLV_NO_ENTRY;
        }
        *pathsource = DW_PATHSOURCE_dsym;
        res = _dwarf_object_detector_fd_a(fd,
            ftype,endian,offsetsize,0,
            filesize,errcode);
        if (res != DW_DLV_OK) {
            _dwarf_closer(fd);
            return res;
        }
        _dwarf_closer(fd);
        return DW_DLV_OK;
    }
    return DW_DLV_NO_ENTRY;
}

static int
blockmatch(unsigned char *l,
    unsigned char* r,
    unsigned length)
{
    unsigned int i = 0;
    for ( ; i < length; ++i) {
        if (l[i] != r[i]) {
            return FALSE;
        }
    }
    return TRUE;
}

/*  The debug version we expect not to have debuglink,
    checking here if buildid matches.
    Returns TRUE or FALSE  */
static Dwarf_Bool
match_buildid(
    unsigned char *  crc_base,
    unsigned         buildid_length_base,
    unsigned  char  *buildid_base,
    /*  *_base is executable info while
        *_debug is the debug object. */
    unsigned char  *crc_debug,
    unsigned        buildid_length_debug,
    unsigned  char *buildid_debug)
{
    if (!_dwarf_get_suppress_debuglink_crc() && \
        crc_debug && crc_base) {
        /* crc available for both */
        if (!blockmatch(crc_debug,crc_base,4)) {
            return FALSE;
        }
        return TRUE;
    }
    if (!blockmatch(buildid_base,buildid_debug,
        buildid_length_base)) {
        return FALSE;
    }
    if (buildid_length_base != buildid_length_debug) {
        return FALSE;
    }
    return TRUE;
}

/*  we need the crc byte order to match that
    of the object file so a comparison works.
    Here we fix up when there is a mismatch */
static int
_dwarf_debuglink_finder_newpath(
    char         * path_in,
    unsigned char *crc_in,
    unsigned       buildid_len_in,
    unsigned char *buildid_in,
    dwarfstring    *m,
    int * fd_out)
{
    unsigned char  lcrc[4] = {0,0,0,0};
    unsigned char  newcrc[4] = {0,0,0,0};
    char          *debuglinkpath = 0; /* must be freed */
    unsigned char *crc = 0;
    char          *debuglinkfullpath = 0;
    unsigned       debuglinkfullpath_strlen = 0;
    unsigned       buildid_type = 0;
    char         * buildidownername = 0;
    unsigned char *buildid = 0;
    unsigned       buildid_length = 0;
    char        ** paths = 0; /* must be freed */
    unsigned       paths_count = 0;
    Dwarf_Debug dbg = 0;
    Dwarf_Error error = 0;
    char *path = path_in;
    Dwarf_Bool didmatch = FALSE;
    int res = 0;

    res = dwarf_init_path(path,
        0,0,
        DW_GROUPNUMBER_ANY,
        0,0, &dbg,&error);
    if (res == DW_DLV_ERROR) {
        /* ASSERT:  dbg is NULL as init failed */
        dwarf_dealloc_error(dbg,error);
        error = 0;
        return DW_DLV_NO_ENTRY;
    }
    if (res == DW_DLV_NO_ENTRY) {
        /* should never happen */
        return DW_DLV_NO_ENTRY;
    }
    res = dwarf_gnu_debuglink(dbg,
        &debuglinkpath,
        &crc, &debuglinkfullpath, &debuglinkfullpath_strlen,
        &buildid_type, &buildidownername,
        &buildid, &buildid_length,
        &paths, &paths_count, &error);
    if (res == DW_DLV_ERROR) {
        dwarf_dealloc_error(dbg,error);
        dwarf_finish(dbg);
        error = 0;
        dbg = 0;
        return DW_DLV_NO_ENTRY;
    }
    if (res == DW_DLV_NO_ENTRY) {
        /*  There is no debuglink section */
        dwarf_finish(dbg);
        dbg = 0;
        return DW_DLV_NO_ENTRY;
    }
    free(paths);
    paths = 0;

    if (!_dwarf_get_suppress_debuglink_crc() &&crc_in && !crc) {
        int res1 = 0;

        res1 = dwarf_crc32(dbg,lcrc,&error);
        if (res1 == DW_DLV_ERROR) {
            paths = 0;
            free(debuglinkfullpath);
            dwarf_dealloc_error(dbg,error);
            dwarf_finish(dbg);
            error = 0;
            dbg = 0;
            /*  Cannot match the crc_in, give up. */
            return DW_DLV_NO_ENTRY;
        }
        if (res1 == DW_DLV_OK) {
            dbg->de_copy_word((void *)newcrc,(void *)lcrc,4);
            crc=&newcrc[0];
        }
    }
    free(debuglinkfullpath);
    didmatch = match_buildid(
        /* This is about the executable */
        crc_in,buildid_len_in,buildid_in,
        /* pass in local so we can calculate the missing crc */
        /* following is the target, ie, debug */
        crc,buildid_length,buildid);
    if (error) {
        /*  This should never happen. It would mean
            error was set without DW_DLV_ERROR */
        dwarf_dealloc_error(dbg,error);
        error = 0;
    }
    if (didmatch) {
        dwarfstring_append(m,path);
        *fd_out = dbg->de_fd;
        dbg->de_owns_fd = FALSE;
        dwarf_finish(dbg);
        dbg = 0;
        return DW_DLV_OK;
    }
    dwarf_finish(dbg);
    return DW_DLV_NO_ENTRY;
}

static int
_dwarf_debuglink_finder_internal(
    char         **gl_pathnames,
    unsigned int   gl_pathcount,
    char         * path_in,
    dwarfstring    *m,
    int * fd_out,
    int * errcode)
{
    int res = 0;
    /*  This local dbg is opened and then dwarf_finish()
        here.  No dbg in the arguments! */
    Dwarf_Debug    dbg = 0;
    char         * path = 0;
    Dwarf_Error    error = 0;
    unsigned int   p = 0;
    char          *debuglinkpath = 0;
    unsigned char *crc = 0;
    char          *debuglinkfullpath = 0; /* must be freed*/
    unsigned       debuglinkfullpath_strlen = 0;
    unsigned       buildid_type = 0;
    char         * buildidownername = 0;
    unsigned char *buildid = 0;
    unsigned       buildid_length = 0;
    char        ** paths = 0; /* must be freed */
    unsigned       paths_count = 0;
    unsigned       i = 0;

    path = path_in;
    /*  This path will work.
        Already know the file is there. */
    res = dwarf_init_path(path,
        0,0,
        DW_GROUPNUMBER_ANY,
        0,0, &dbg, &error);
    if (res == DW_DLV_ERROR) {
        /* error codes all >=0 && < 2000 */
        *errcode = (int)dwarf_errno(error);
        dwarf_dealloc_error(dbg,error);
        error = 0;
        return res;
    }
    if (res == DW_DLV_NO_ENTRY) {
        return res;
    }
    /*  If an exe (with debuglink, possibly) has no DWARF
        sections we should not give up till we look for
        debuglink stuff, which as of gitbub issue 297,
        no longer results in DW_DLV_NO_ENTRY. */
    for (p = 0;  p < gl_pathcount; ++p) {
        const char *lpath = 0;

        lpath = (const char *)gl_pathnames[p];
        res = dwarf_add_debuglink_global_path(dbg,
            lpath, &error);
        if (res != DW_DLV_OK){
            if (res == DW_DLV_ERROR) {
                *errcode = (int)dwarf_errno(error);
                dwarf_dealloc_error(dbg,error);
                error = 0;
            }
            dwarf_finish(dbg);
            return res;
        }
    }
    res = dwarf_gnu_debuglink(dbg,
        &debuglinkpath,
        &crc, &debuglinkfullpath, &debuglinkfullpath_strlen,
        &buildid_type, &buildidownername,
        &buildid, &buildid_length,
        &paths, &paths_count, &error);
    if (res == DW_DLV_ERROR) {
        *errcode = (int)dwarf_errno(error);
        dwarf_dealloc_error(dbg,error);
        dwarf_finish(dbg);
        return DW_DLV_NO_ENTRY;
    }
    if (res == DW_DLV_NO_ENTRY) {
        /*  There is no debuglink buildid   section? */
        dwarf_finish(dbg);
        return DW_DLV_NO_ENTRY;
    }
    for (i =0; i < paths_count; ++i) {
        char *pa =     paths[i];
        int pfd = 0;

        /*  First, open the file to determine if it exists.
            If not, loop again */

        pfd = _dwarf_openr(pa);
        if (pfd  < 0) {
            /*  This is the usual path. */
            continue;
        }
        _dwarf_closer(pfd);
        /* ASSERT: never returns DW_DLV_ERROR */
        res = _dwarf_debuglink_finder_newpath(
            pa,crc,buildid_length, buildid,
            m,fd_out);
        if (res == DW_DLV_OK) {
            free(debuglinkfullpath);
            free(paths);
            paths = 0;
            dwarf_finish(dbg);
            return DW_DLV_OK;
        }
        *errcode = 0;
        continue;
    }
    free(debuglinkfullpath);
    free(paths);
    paths = 0;
    dwarf_finish(dbg);
    return DW_DLV_NO_ENTRY;
}

int
dwarf_object_detector_path_b(
    const char  * path,
    char *        outpath,
    unsigned long outpath_len,
    char **       gl_pathnames,
    unsigned      gl_pathcount,
    unsigned *    ftype,
    unsigned *    endian,
    unsigned *    offsetsize,
    Dwarf_Unsigned * filesize_out,
    unsigned char *  pathsource,
    int *errcode)
{
    int fd = -1;
    int res = 0;
    int have_outpath = outpath && outpath_len;
    unsigned char lpathsource = DW_PATHSOURCE_basic;

    if (pathsource) {
        lpathsource = *pathsource;
    }
    if (lpathsource == DW_PATHSOURCE_basic && have_outpath) {
        /*  On return from the following call  we could well
            close the fd above and open a new one. */
        int debuglink_fd = -1;
        size_t dllenszt = 0;
        char *cp = 0;
        dwarfstring m;

        dwarfstring_constructor(&m);
        res = _dwarf_debuglink_finder_internal(
            gl_pathnames,gl_pathcount,
            (char *)path, &m,&debuglink_fd, errcode);
        if (res == DW_DLV_ERROR) {
            dwarfstring_destructor(&m);
            if (debuglink_fd != -1) {
                _dwarf_closer(debuglink_fd);
            }
            return res;
        }
        if (res == DW_DLV_NO_ENTRY) {
            /*  We did not find an alternative path */
            res = dw_stpcpy(outpath,path,&cp,outpath+outpath_len);
            if (res != DW_DLV_OK) {
                *errcode =  DW_DLE_PATH_SIZE_TOO_SMALL;
                return DW_DLV_ERROR;
            }
            lpathsource = DW_PATHSOURCE_basic;
        } else {
            if (debuglink_fd != -1) {
                _dwarf_closer(debuglink_fd);
                debuglink_fd = -1;
            }
            dllenszt = dwarfstring_strlen(&m)+1;
            if (dllenszt >= (size_t)outpath_len) {
                *errcode = DW_DLE_DEBUGLINK_PATH_SHORT;
                return DW_DLV_ERROR;
            }
            res = dw_stpcpy(outpath,dwarfstring_string(&m),
                &cp,outpath+outpath_len);
            if (res != DW_DLV_OK) {
                *errcode = DW_DLE_DEBUGLINK_PATH_SHORT;
                return DW_DLV_ERROR;
            }
            lpathsource = DW_PATHSOURCE_debuglink;
        }
        dwarfstring_destructor(&m);
        fd = _dwarf_openr(outpath);
        /* fall through to get filesize etc */
    } else {
        lpathsource = DW_PATHSOURCE_basic;
        fd = _dwarf_openr(path);
    }
    if (fd < 0) {
        if (pathsource) {
            *pathsource = DW_PATHSOURCE_unspecified;
        }
        return DW_DLV_NO_ENTRY;
    }
    res = _dwarf_object_detector_fd_a(fd,
        ftype,endian,offsetsize,0,
        filesize_out,errcode);
    if (res != DW_DLV_OK) {
        lpathsource = DW_PATHSOURCE_unspecified;
    }
    if (pathsource) {
        *pathsource = lpathsource;
    }
    _dwarf_closer(fd);
    return res;
}
