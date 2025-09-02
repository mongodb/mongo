/*
  Copyright (C) 2000-2006 Silicon Graphics, Inc.  All Rights Reserved.
  Portions Copyright 2007-2010 Sun Microsystems, Inc. All rights reserved.
  Portions Copyright 2008-2010 Arxan Technologies, Inc. All rights reserved.
  Portions Copyright 2011-2020 David Anderson. All rights reserved.
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
/*
Here is the deepest routes through dwarf_init_path_dl(),
depending on arguments.
It is called by dwarfdump to open an fd and return Dwarf_Debug.
Much of this is to handle GNU debuglink.
dwarf_init_path_dl(path true_path and globals, dbg1
    dwarf_object_detector_path_dSYM (dsym only(
        if returns DW_DLV_OK itis dSYM
    dwarf_object_detector_path_b( &debuglink with global paths.
        dwarf_object_detector_path_b  ftype
            check for dSYM if found it is the object to run on.
                dwarf_object_detector_fd (gets size ftype)
                return
            _dwarf_debuglink_finder_internal(TRUE passing
                in globals paths listr)
                new local dbg
                dwarf_init_path(path no dysm or debuglink
                    no global paths)
                    dwarf_object_detector_path_b( path  no dsym
                        or debuglink no global paths
                        dwarf_object_detector (path
                        dwarf_object_detector_fd (gets size ftype)
                    for each global pathin list, add to dbg
                    dwarf_gnu_debuglink(dbg
                        for each global path in debuglink list
                            _dwarf_debuglink_finder_internal(FALSE
                                no global paths)
                                if crc match return OK with
                                    pathname and fd returned
                                else return NO_ENTRY
*/

#include <config.h>

#include <stddef.h> /* size_t */
#include <stdlib.h> /* free() */
#include <string.h> /* strdup() */
#include <stdio.h> /* debugging */

#include "dwarf.h"
#include "libdwarf.h"
#include "dwarf_local_malloc.h"
#include "libdwarf_private.h"
#include "dwarf_base_types.h"
#include "dwarf_util.h"
#include "dwarf_opaque.h"
#include "dwarf_alloc.h"
#include "dwarf_error.h"
#include "dwarf_object_detector.h"

/*  The design of Dwarf_Debug_s data on --file-tied
data and how it is used.  See also dwarf_opaque.h
and dwarf_util.c

The fields involved are
de_dbg
de_primary_dbg
de_secondary_dbg
de_errors_dbg
de_tied_data.td_tied_object

On any init completing it will be considered
    primary, Call it p1.
    p1->de_dbg == p1
    p1->de_primary_dbg == p1
    p1->de_secondary_dbg == NULL
        p1->de_errors_dbg == p1
    p1->de_tied_data.td_tied_object = 0
Init a second object, call it p2 (settings as above).

Call dwarf_set_tied (p1,p2) (it is ok if p2 == NULL)
    p1 is as above except that
        p1->de_secondary_dbg == p2
        p1->de_tied_data.td_tied_object = p2;
    If p2 is non-null:
        p2->de_dbg == p2
        p2->de_primary_dbg = p1.
        p2->de_secondary_dbg = p2
        p2->de_errors_dbg = p1
All this is only useful if p1 has dwo/dwp sections
(split-dwarf) and p2 has the relevant TAG_skeleton(s)

If px->de_secondary_dbg is non-null
    and px->secondary_dbg == px
    then px is secondary.

If x->de_secondary_dbg is non-null
    and px->secondary_dbg != px
    then px is primary.

If px->de_secondary_dbg is null
    then px is a primary. and there
    is no secondary.

    Call dwarf_set_tied(p1,NULL) and both p1 and
    p2 are returned to initial conditions
    as before they were tied together. */

static int
set_global_paths_init(Dwarf_Debug dbg, Dwarf_Error* error)
{
    int res = 0;

    res = dwarf_add_debuglink_global_path(dbg,
        "/usr/lib/debug",error);
    return res;
}

/* New in September 2023. */
int
dwarf_init_path_a(const char *path,
    char            * true_path_out_buffer,
    unsigned          true_path_bufferlen,
    unsigned          groupnumber,
    unsigned          universalnumber,
    Dwarf_Handler     errhand,
    Dwarf_Ptr         errarg,
    Dwarf_Debug     * ret_dbg,
    Dwarf_Error     * error)
{
    return dwarf_init_path_dl_a(path,
        true_path_out_buffer,true_path_bufferlen,
        groupnumber,universalnumber,
        errhand,errarg,ret_dbg,
        0,0,0,
        error);
}

int dwarf_init_path(const char *path,
    char            * true_path_out_buffer,
    unsigned          true_path_bufferlen,
    unsigned          groupnumber,
    Dwarf_Handler     errhand,
    Dwarf_Ptr         errarg,
    Dwarf_Debug     * ret_dbg,
    Dwarf_Error     * error)
{
    unsigned int universalnumber = 0;
    return dwarf_init_path_dl_a(path,
        true_path_out_buffer,true_path_bufferlen,
        groupnumber,universalnumber,
        errhand,errarg,ret_dbg,
        0,0,0,
        error);
}

static void
final_common_settings(Dwarf_Debug dbg,
    const char *file_path,
    int fd,
    unsigned char lpath_source,
    unsigned char *path_source,
    Dwarf_Error *error)
{
    int res = 0;

    dbg->de_path = strdup(file_path);
    dbg->de_fd = fd;
    dbg->de_owns_fd = TRUE;
    dbg->de_path_source = lpath_source;
    if (path_source) {
        *path_source = lpath_source;
    }
    dbg->de_owns_fd = TRUE;
    res = set_global_paths_init(dbg,error);
    if (res == DW_DLV_ERROR && error) {
        dwarf_dealloc_error(dbg,*error);
        *error = 0;
    }
    return;
}
/*  New October 2020
    Given true_path_out_buffer (and true_path_bufferlen)
    non-zero this finds a dSYM (if such exists) with the
    file name in true_path_out_buffer

    If not a dSYM it follows debuglink rules to try to find a file
    that matches requirements. If found returns DW_DLV_OK and
    copies the name to true_path_out_buffer;
    If none of the above found, it copies path into true_path
    and returns DW_DLV_OK, we know the name is good;

    The pathn_fd is owned by libdwarf and is in the created dbg->de_fd
    field.
*/
int
dwarf_init_path_dl(const char *path,
    char            * true_path_out_buffer,
    unsigned        true_path_bufferlen,
    unsigned        groupnumber,
    Dwarf_Handler   errhand,
    Dwarf_Ptr       errarg,
    Dwarf_Debug     * ret_dbg,
    char            ** dl_path_array,
    unsigned int    dl_path_count,
    unsigned char   * path_source,
    Dwarf_Error     * error)
{
    unsigned int universalnumber = 0;
    int res = 0;

    res = dwarf_init_path_dl_a(path,
        true_path_out_buffer, true_path_bufferlen,
        groupnumber,universalnumber,
        errhand,errarg,ret_dbg, dl_path_array,
        dl_path_count,path_source,error);
    return res;
}

#if 0 /*  for debugging */
static void
dump_header_fields(const char *w,Dwarf_Debug dbg)
{
    printf("Dumping certain fields of %s\n",w);
    printf("ftype         : %d\n",dbg->de_ftype);
    printf("machine       : %llu\n",dbg->de_obj_machine);
    printf("flags         : 0x%llx\n",dbg->de_obj_flags);
    printf("pointer size  : %u\n",dbg->de_pointer_size);
    printf("big_endian?   : %u\n",dbg->de_big_endian_object);
    printf("ubcount       : %u\n",dbg->de_universalbinary_count);
    printf("ubindex       : %u\n",dbg->de_universalbinary_index);
    printf("ub offset     : %llu\n",dbg->de_obj_ub_offset);
    printf("path source   : %u\n",dbg->de_path_source);
    printf("comdat group# : %u\n",dbg->de_groupnumber);
    exit(0);
}
#endif

int
dwarf_init_path_dl_a(const char *path,
    char            * true_path_out_buffer,
    unsigned        true_path_bufferlen,
    unsigned        groupnumber,
    unsigned        universalnumber,
    Dwarf_Handler   errhand,
    Dwarf_Ptr       errarg,
    Dwarf_Debug     * ret_dbg,
    char            ** dl_path_array,
    unsigned int    dl_path_count,
    unsigned char   * path_source,
    Dwarf_Error     * error)
{
    unsigned       ftype = 0;
    unsigned       endian = 0;
    unsigned       offsetsize = 0;
    Dwarf_Unsigned filesize = 0;
    int res =  DW_DLV_ERROR;
    int errcode = 0;
    int fd = -1;
    Dwarf_Debug dbg = 0;
    char *file_path = 0;
    unsigned char  lpath_source = DW_PATHSOURCE_basic;
    enum Dwarf_Sec_Alloc_Pref preferred_load_type =
        Dwarf_Alloc_Malloc;

    if (!ret_dbg) {
        DWARF_DBG_ERROR(NULL,DW_DLE_DWARF_INIT_DBG_NULL,
            DW_DLV_ERROR);
    }
    /*  Non-null *ret_dbg will cause problems dealing with
        DW_DLV_ERROR */
    *ret_dbg = 0;
    if (!path) {
        /* Oops. Null path */
        _dwarf_error_string(NULL,
            error,DW_DLE_STRING_PTR_NULL,
            "DW_DLE_STRING_PTR_NULL: Passing a"
            " null path argument to "
            "dwarf_init_path or dwarf_init_path_dl"
            " cannot work. Error.");
        return DW_DLV_ERROR;
    }

    /*  Determine the type of section allocations:
        mmap or malloc.  Sets global alloc type as side effect.
        DW_LOAD_PREF_MALLOC or DW_LOAD_PREF_MMAP*/
    preferred_load_type = _dwarf_determine_section_allocation_type();

    /* a special dsym call so we only check once. */
    if (true_path_out_buffer) {
        res = dwarf_object_detector_path_dSYM(path,
            true_path_out_buffer,
            true_path_bufferlen,
            dl_path_array,dl_path_count,
            &ftype,&endian,&offsetsize,&filesize,
            &lpath_source,
            &errcode);
        if (res != DW_DLV_OK) {
            if (res == DW_DLV_ERROR) {
                /* ignore error. Look further. */
                errcode = 0;
            }
        }
    }
    if (res != DW_DLV_OK) {
        res = dwarf_object_detector_path_b(path,
            true_path_out_buffer,
            true_path_bufferlen,
            dl_path_array,dl_path_count,
            &ftype,&endian,&offsetsize,&filesize,
            &lpath_source,
            &errcode);
        if (res != DW_DLV_OK ) {
            if (res == DW_DLV_ERROR) {
                errcode = 0;
            }
        }
    }
    if (res != DW_DLV_OK) {
        /*  So as a last resort in case
            of data corruption in the object.
            Lets try without
            investigating debuglink  or dSYM. */
        res = dwarf_object_detector_path_b(path,
            0,
            0,
            dl_path_array,dl_path_count,
            &ftype,&endian,&offsetsize,&filesize,
            &lpath_source,
            &errcode);
    }
    if (res != DW_DLV_OK) {
        /* impossible. The last above *had* to work */
        if (res == DW_DLV_ERROR) {
            _dwarf_error(NULL, error, errcode);
        }
        return res;
    }
    /*  ASSERT: lpath_source != DW_PATHSOURCE_unspecified  */
    if (lpath_source != DW_PATHSOURCE_basic &&
        true_path_out_buffer && *true_path_out_buffer) {
        /* MacOS dSYM or GNU debuglink */
        file_path = true_path_out_buffer;
        fd = _dwarf_openr(true_path_out_buffer);
    } else {
        /*  ASSERT: lpath_source = DW_PATHSOURCE_basic */
        file_path = (char *)path;
        fd = _dwarf_openr(path);
    }

    if (fd == -1) {
        DWARF_DBG_ERROR(NULL, DW_DLE_FILE_UNAVAILABLE,
            DW_DLV_ERROR);
    }

    switch(ftype) {
    case DW_FTYPE_ELF: {
        res = _dwarf_elf_nlsetup(fd,
            file_path,
            ftype,endian,offsetsize,filesize,
            groupnumber,errhand,errarg,&dbg,error);
        if (res != DW_DLV_OK) {
            _dwarf_closer(fd);
            return res;
        }
        final_common_settings(dbg,file_path,fd,
            lpath_source,path_source,error);
        dbg->de_ftype =  (Dwarf_Small)ftype;
        dbg->de_preferred_load_type = preferred_load_type;
        *ret_dbg = dbg;
        return res;
    }
    case DW_FTYPE_APPLEUNIVERSAL:
    case DW_FTYPE_MACH_O: {
        res = _dwarf_macho_setup(fd,
            file_path,
            universalnumber,
            ftype,endian,offsetsize,filesize,
            groupnumber,errhand,errarg,&dbg,error);
        if (res != DW_DLV_OK) {
            _dwarf_closer(fd);
            return res;
        }
        final_common_settings(dbg,file_path,fd,
            lpath_source,path_source,error);
        dbg->de_ftype =  (Dwarf_Small)ftype;
        dbg->de_preferred_load_type = preferred_load_type;
        *ret_dbg = dbg;
        return res;
    }
    case DW_FTYPE_PE: {
        res = _dwarf_pe_setup(fd,
            file_path,
            ftype,endian,offsetsize,filesize,
            groupnumber,errhand,errarg,&dbg,error);
        if (res != DW_DLV_OK) {
            _dwarf_closer(fd);
            return res;
        }
        final_common_settings(dbg,file_path,fd,
            lpath_source,path_source,error);
        dbg->de_ftype =  (Dwarf_Small)ftype;
        dbg->de_preferred_load_type = preferred_load_type;
        *ret_dbg = dbg;
        return res;
    }
    default:
        _dwarf_closer(fd);
        DWARF_DBG_ERROR(NULL, DW_DLE_FILE_WRONG_TYPE,
            DW_DLV_ERROR);
        /* Macro returns, cannot reach this line. */
    }
    /* Cannot reach this line */
}

/*  New March 2017, this provides for reading
    object files with multiple elf section groups.
    If you are unsure about group_number, use
    DW_GROUPNUMBER_ANY  as groupnumber.
*/
int
dwarf_init_b(int fd,
    unsigned        group_number,
    Dwarf_Handler   errhand,
    Dwarf_Ptr       errarg,
    Dwarf_Debug *   ret_dbg,
    Dwarf_Error *   error)
{
    unsigned ftype = 0;
    unsigned endian = 0;
    unsigned offsetsize = 0;
    unsigned universalnumber = 0;
    Dwarf_Unsigned   filesize = 0;
    int res = 0;
    int errcode = 0;

    if (!ret_dbg) {
        DWARF_DBG_ERROR(NULL,DW_DLE_DWARF_INIT_DBG_NULL,DW_DLV_ERROR);
    }
    /*  Non-null *ret_dbg will cause problems dealing with
        DW_DLV_ERROR */
    *ret_dbg = 0;

    res = _dwarf_object_detector_fd_a(fd,
        &ftype,
        &endian,&offsetsize,0,
        &filesize,&errcode);
    if (res == DW_DLV_NO_ENTRY) {
        return res;
    }
    if (res == DW_DLV_ERROR) {
        /* This macro does a return. */
        DWARF_DBG_ERROR(NULL, DW_DLE_FILE_WRONG_TYPE, DW_DLV_ERROR);
    }
    switch(ftype) {
    case DW_FTYPE_ELF: {
        int res2 = 0;

        res2 = _dwarf_elf_nlsetup(fd,"",
            ftype,endian,offsetsize,filesize,
            group_number,errhand,errarg,ret_dbg,error);
        if (res2 != DW_DLV_OK) {
            return res2;
        }
        set_global_paths_init(*ret_dbg,error);
        return res2;
        }
    case DW_FTYPE_APPLEUNIVERSAL:
    case DW_FTYPE_MACH_O: {
        int resm = 0;

        resm = _dwarf_macho_setup(fd,"",
            universalnumber,
            ftype,endian,offsetsize,filesize,
            group_number,errhand,errarg,ret_dbg,error);
        if (resm != DW_DLV_OK) {
            return resm;
        }
        set_global_paths_init(*ret_dbg,error);
        return resm;
        }

    case DW_FTYPE_PE: {
        int resp = 0;

        resp = _dwarf_pe_setup(fd,
            "",
            ftype,endian,offsetsize,filesize,
            group_number,errhand,errarg,ret_dbg,error);
        if (resp != DW_DLV_OK) {
            return resp;
        }
        set_global_paths_init(*ret_dbg,error);
        return resp;
        }
    default: break;
    }
    DWARF_DBG_ERROR(NULL, DW_DLE_FILE_WRONG_TYPE, DW_DLV_ERROR);
    /* Macro above returns. cannot reach here. */
}

/*
    Frees all memory that was not previously freed
    by dwarf_dealloc.
    Aside from certain categories.

    Applicable when dwarf_init() or dwarf_elf_init()
    or the -b() form was used to init 'dbg'.
*/
int
dwarf_finish(Dwarf_Debug dbg)
{
#ifdef LIBDWARF_MALLOC
    _libdwarf_finish();
#endif
    if (IS_INVALID_DBG(dbg)) {
        _dwarf_free_static_errlist();
        return DW_DLV_OK;
    }
    if (dbg->de_obj_file) {
        /*  The initial character of a valid
            dbg->de_obj_file->object struct is a letter:
            E, F, M, or P, but E will not happen. */
        char otype  = *(char *)(dbg->de_obj_file->ai_object);

        switch(otype) {
        case 'E': /* libelf. Impossible for years now. */
            break;
        case 'F':
            /* Non-libelf elf access */
        case 'M':
        case 'P':
            /* These take care of data alloc/mmap
                by object type. */
            dbg->de_obj_file->ai_methods->om_finish(dbg->de_obj_file);
        default:
            /*  Do nothing. A serious internal error */
            break;
        }
    }
    if (dbg->de_owns_fd) {
        _dwarf_closer(dbg->de_fd);
        dbg->de_owns_fd = FALSE;
    }
    free((void *)dbg->de_path);
    dbg->de_path = 0;
    /*  dwarf_object_finish() also frees de_path,
        but that is safe because we set it to zero
        here so no duplicate free will occur.
        It never returns DW_DLV_ERROR.
        Not all code uses libdwarf exactly as we do
        hence the free() there.
        This free/munmap as appropriate from the
        DWARF data point of view independent of
        object details. */
    return dwarf_object_finish(dbg);
}

/*
    tieddbg should be the executable or .o
    that has the .debug_addr section that
    the base dbg refers to. See Split Objects in DWARF5.
    Or in DWARF5  maybe .debug_rnglists or .debug_loclists.

    Allows calling with NULL though we really just set
    primary_dbg->ge_primary to de_primary_dbg, thus cutting
    links between main and any previous tied-file setup.
    New September 2015.
    Logic revised Nov 2024. See dwarf_opaque.h
*/
int
dwarf_set_tied_dbg(Dwarf_Debug primary_dbg,
    Dwarf_Debug secondary_dbg,
    Dwarf_Error*error)
{
    CHECK_DBG(primary_dbg,error,"dwarf_set_tied_dbg()");
    if (secondary_dbg == primary_dbg) {
        _dwarf_error_string(primary_dbg,error,
            DW_DLE_NO_TIED_FILE_AVAILABLE,
            "DW_DLE_NO_TIED_FILE_AVAILABLE: bad argument to "
            "dwarf_set_tied_dbg(), tied and main must not be the "
            "same pointer!");
        return DW_DLV_ERROR;
    }
    if (secondary_dbg) {
        if (primary_dbg->de_secondary_dbg ) {
            _dwarf_error_string(primary_dbg,error,
                DW_DLE_NO_TIED_FILE_AVAILABLE,
                "DW_DLE_NO_TIED_FILE_AVAILABLE: bad argument to "
                "dwarf_set_tied_dbg(), primary_dbg already has"
                " a secondary_dbg!");
            return DW_DLV_ERROR;
        }
        primary_dbg->de_tied_data.td_tied_object = secondary_dbg;
        primary_dbg->de_secondary_dbg = secondary_dbg;
        secondary_dbg->de_secondary_dbg = secondary_dbg;
        secondary_dbg->de_errors_dbg = primary_dbg;
        CHECK_DBG(secondary_dbg,error,"dwarf_set_tied_dbg() "
            "dw_secondary_dbg"
            "is invalid");
        primary_dbg->de_secondary_dbg = secondary_dbg;
        return DW_DLV_OK;
    } else {
        primary_dbg->de_secondary_dbg = 0;
        primary_dbg->de_tied_data.td_tied_object = 0;
    }
    return DW_DLV_OK;
}

/*  New September 2015.
    As of Aug 2023 this correctly returns tied_dbg
    whether main or tied passed in. Before this
    it would return the dbg passed in.
    If there is no tied-dbg this returns main dbg. */
int
dwarf_get_tied_dbg(Dwarf_Debug dw_dbg,
    Dwarf_Debug *dw_secondary_dbg_out,
    Dwarf_Error *dw_error)
{
    CHECK_DBG(dw_dbg,dw_error,"dwarf_get_tied_dbg()");
    *dw_secondary_dbg_out = 0;
    if (DBG_IS_PRIMARY(dw_dbg)) {
        if (!dw_dbg->de_secondary_dbg) {
            *dw_secondary_dbg_out = dw_dbg;
            return DW_DLV_OK;
        }
        *dw_secondary_dbg_out = dw_dbg->de_secondary_dbg;
        return DW_DLV_OK;
    }
    if (DBG_IS_SECONDARY(dw_dbg)) {
        *dw_secondary_dbg_out = dw_dbg;
        return DW_DLV_OK;
    }
    /*  Leave returned secondary_dbg_out NULL,
        this should not happen */
    return DW_DLV_OK;
}
