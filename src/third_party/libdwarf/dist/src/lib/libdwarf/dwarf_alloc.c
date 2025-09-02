/*
  Copyright (C) 2000-2005 Silicon Graphics, Inc.  All Rights Reserved.
  Portions Copyright (C) 2007-2022  David Anderson. All Rights Reserved.

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
/*  To see the full set of DW_DLA types and nothing
    else  try:
    grep DW_DLA dwarf_alloc.c | grep 0x
*/

#include <config.h>

#include <stdio.h>  /* fclose() */
#include <stdlib.h> /* malloc() free() getenv() */
#include <string.h> /* memset() */
#if HAVE_FULL_MMAP
#include <unistd.h> /* sysconf() */
#include <sys/mman.h> /* mmap() munmap() */
#endif

#if defined(_WIN32) && defined(HAVE_STDAFX_H)
#include "stdafx.h"
#endif /* HAVE_STDAFX_H */

#ifdef HAVE_STDINT_H
#include <stdint.h> /* uintptr_t */
#endif /* HAVE_STDINT_H */

#include "dwarf.h"
#include "libdwarf.h"
#include "dwarf_local_malloc.h"
#include "libdwarf_private.h"
#include "dwarf_base_types.h"
#include "dwarf_opaque.h"
#include "dwarf_error.h"
#include "dwarf_alloc.h"
/*  These files are included to get the sizes
    of structs for malloc.
*/
#include "dwarf_util.h"
#include "dwarf_line.h"
#include "dwarf_global.h"
#include "dwarf_arange.h"
#include "dwarf_abbrev.h"
#include "dwarf_debugaddr.h"
#include "dwarf_die_deliv.h"
#include "dwarf_frame.h"
#include "dwarf_loc.h"
#include "dwarf_harmless.h"
#include "dwarf_tsearch.h"
#include "dwarf_gdbindex.h"
#include "dwarf_gnu_index.h"
#include "dwarf_xu_index.h"
#include "dwarf_macro5.h"
#include "dwarf_debugnames.h"
#include "dwarf_rnglists.h"
#include "dwarf_dsc.h"
#include "dwarf_string.h"
#include "dwarf_str_offsets.h"

/* if DEBUG_ALLOC is defined a lot of stdout is generated here. */
#undef DEBUG_ALLOC
/*  Some allocations are simple some not. These reduce
    the issue of determining which sort of thing to a simple
    test. See ia_multiply_count
    Usually when MULTIPLY_NO is set the count
    is 1, so MULTIPY_CT would work as well.  */
#define MULTIPLY_NO 0
#define MULTIPLY_CT 1
#define MULTIPLY_SP 2
/*  This translates into de_alloc_hdr into a per-instance size
    and allows room for a constructor/destructor pointer.
    Rearranging the DW_DLA values would break binary compatibility
    so that is not an option.
*/
struct ial_s {
    /*  In bytes, one struct instance.  */
    short ia_struct_size;

    /*  Not a count, but a MULTIPLY{_NO,_CT,_SP} value. */
    short ia_multiply_count;

    /*  When we really need a constructor/destructor
        these make applying such quite simple. */
    int (*specialconstructor) (Dwarf_Debug, void *);
    void (*specialdestructor) (void *);
};

/*  Used as a way to return meaningful errors when
    the malloc arena is exhausted (when malloc returns NULL).
    Not normally used.
    New in December 2014.*/
struct Dwarf_Error_s _dwarf_failsafe_error = {
    DW_DLE_FAILSAFE_ERRVAL,
    0,
    1
};

/*  If non-zero (the default) de_alloc_tree (see dwarf_alloc.c)
    is used normally.  If zero then dwarf allocations
    are not tracked by libdwarf and dwarf_finish() cannot
    clean up any per-Dwarf_Debug allocations the
    caller forgot to dealloc. */
static signed char global_de_alloc_tree_on = 1;

/*  Defined March 7 2020. Allows a caller to
    avoid most tracking by the de_alloc_tree hash
    table if called with v of zero.
    Returns the value the flag was before this call. */
int dwarf_set_de_alloc_flag(int v)
{
    int ov = global_de_alloc_tree_on;
    global_de_alloc_tree_on = (char)v;
    return ov;
}

void
_dwarf_error_destructor(void *m)
{
    Dwarf_Error er = (Dwarf_Error)m;
    dwarfstring *erm = (dwarfstring *)er->er_msg;
    if (! erm) {
        return;
    }
#if DEBUG_ALLOC
    printf("libdwarfdetector DEALLOC Now destruct error "
        "string %s\n",dwarfstring_string(erm));
    fflush(stdout);
#endif /* DEBUG_ALLOC */
    dwarfstring_destructor(erm);
    free(erm);
    er->er_msg = 0;
    return;
}

/*  To do destructors we need some extra data in every
    _dwarf_get_alloc situation. */
/* Here is the extra we malloc for a prefix. */
struct reserve_size_s {
    void *dummy_rsv1;
    void *dummy_rsv2;
};
/* Here is how we use the extra prefix area. */
struct reserve_data_s {
    void *rd_dbg;
    /*  rd_length can only record correctly for short
        allocations, but that's not a problem im practice
        as the value is only for debugging and to
        ensure this struct length is correct. */
    unsigned short rd_length;
    unsigned short rd_type;
};
#define DW_RESERVE sizeof(struct reserve_size_s)

/*  In rare cases (bad object files) an error is created
    via malloc with no dbg to attach it to.
    We do not expect this except on corrupt objects.

    In all cases the user is *supposed* to dealloc
    the returned Dwarf_Error, and if it is in case
    of a NULL Dwarf_Debug the code were will find it
    in this special array and free and zero it.
    Hence no leak.
*/

#define STATIC_ALLOWED 10 /* arbitrary, must be > 2, see below*/
static unsigned static_used = 0;
/*  entries in this list point to allocations of
    type DW_DLA_ERROR. */
static Dwarf_Error staticerrlist[STATIC_ALLOWED];

/*  Clean this out if found */
static void
dw_empty_errlist_item(Dwarf_Error e_in)
{
    unsigned i = 0;
    if (!e_in) {
        return;
    }
    for ( ; i <static_used; ++i) {
        Dwarf_Error e = staticerrlist[i];
        if (e != e_in) {
            continue;
        }
        if (e->er_static_alloc == DE_MALLOC) {
            /* e is the returned address, not
                the base. Free by the base.  */
            void *mallocaddr = 0;

            if ( (uintptr_t)e > DW_RESERVE) {
                mallocaddr = (char*)e - DW_RESERVE;
            } else {
                /*  Impossible */
                continue;
            }
            _dwarf_error_destructor(e);
            free(mallocaddr);
        }
        staticerrlist[i] = 0;
    }
}

/*  If the user calls dwarf_dealloc on an error
    out of a dwarf_init*() call, this will find
    it in the static err list. Here dbg is NULL
    so not mentioned.  */
void
_dwarf_add_to_static_err_list(Dwarf_Error error)
{
    unsigned i = 0;
    if (!error) {
        return;
    }
#ifdef DEBUG_ALLOC
    printf("\nlibdwarfdetector add to static err list "
        " 0x%lx\n",(unsigned long)(uintptr_t)error);
    fflush(stdout);
#endif /* DEBUG_ALLOC */
    for ( ; i <static_used; ++i) {
        Dwarf_Error e = staticerrlist[i];
        if (e) {
            continue;
        }
#ifdef DEBUG_ALLOC
        printf("libdwarfdetector add to static err list at %u\n",
            i);
        fflush(stdout);
#endif /* DEBUG_ALLOC */
        staticerrlist[i] = error;
        return;
    }
    if (static_used < STATIC_ALLOWED) {
        staticerrlist[static_used] = error;
        ++static_used;
    }
}
/*  See libdwarf vulnerability DW202402-002
    for the motivation.
*/
static void
_dwarf_remove_from_staticerrlist(Dwarf_Ptr *space)
{
    unsigned i = 0;
    if (!space) {
        return;
    }
#ifdef DEBUG_ALLOC
    printf("\nlibdwarfdetector remove from static err list "
        " 0x%lx\n",(unsigned long)(uintptr_t)space);
    fflush(stdout);
#endif /* DEBUG_ALLOC */
    for ( ; i <static_used; ++i) {
        Dwarf_Error e = staticerrlist[i];
        if (!e) {
            continue;
        }
        if ((void *)e == space) {
#ifdef DEBUG_ALLOC
            printf("libdwarfdetector rm from static err list at %u\n",
            i);
            fflush(stdout);
#endif /* DEBUG_ALLOC */
            staticerrlist[i] = 0;
            return;
        }
    }
}

/*  This will free everything in the staticerrlist,
    but that is ok */
void
_dwarf_free_static_errlist(void)
{
    unsigned i = 0;

    for ( ; i <static_used; ++i) {
        Dwarf_Error e = staticerrlist[i];
        if (e) {
            dw_empty_errlist_item(e);
            staticerrlist[i] = 0;
        }
    }
}

static const
struct ial_s alloc_instance_basics[ALLOC_AREA_INDEX_TABLE_MAX] = {
    /* 0  none */
    { 1,MULTIPLY_NO, 0, 0},

    /* 0x1 x1 DW_DLA_STRING */
    { 1,MULTIPLY_CT, 0, 0},

    /* 0x2 DW_DLA_LOC */
    {1/* sizeof(Dwarf_Loc)*/,MULTIPLY_NO, 0, 0} ,

    /* x3 DW_DLA_LOCDESC */
    {1/* sizeof(Dwarf_Locdesc)*/,MULTIPLY_NO, 0, 0},

    /* 0x4 DW_DLA_ELLIST */ /* not used */
    { 1,MULTIPLY_NO, 0, 0},

    /* 0x5 DW_DLA_BOUNDS */ /* not used */
    { 1,MULTIPLY_NO, 0, 0},

    /* 0x6 DW_DLA_BLOCK */
    { sizeof(Dwarf_Block),MULTIPLY_NO,  0, 0},

    /* x7 DW_DLA_DEBUG */
    /* the actual dwarf_debug structure */
    { 1,MULTIPLY_NO, 0, 0} ,

    /* x8 DW_DLA_DIE */
    {sizeof(struct Dwarf_Die_s),MULTIPLY_NO, 0, 0},

    /* x9 DW_DLA_LINE */
    {sizeof(struct Dwarf_Line_s),MULTIPLY_NO, 0, 0},

    /* 0xa  10 DW_DLA_ATTR */
    {sizeof(struct Dwarf_Attribute_s),MULTIPLY_NO,  0, 0},

    /* 0xb DW_DLA_TYPE *//* not used */
    {1,MULTIPLY_NO,  0, 0},

    /* 0xc DW_DLA_SUBSCR *//* not used */
    {1,MULTIPLY_NO,  0, 0},

    /* 0xd 13 DW_DLA_GLOBAL */
    {sizeof(struct Dwarf_Global_s),MULTIPLY_NO,  0, 0},

    /* 0xe 14 DW_DLA_ERROR */
    {sizeof(struct Dwarf_Error_s),MULTIPLY_NO,  0,
        _dwarf_error_destructor},

    /* 0xf DW_DLA_LIST */
    {sizeof(Dwarf_Ptr),MULTIPLY_CT, 0, 0},

    /* 0x10 DW_DLA_LINEBUF */ /* not used */
    {1,MULTIPLY_NO, 0, 0},

    /* 0x11 17 DW_DLA_ARANGE */
    {sizeof(struct Dwarf_Arange_s),MULTIPLY_NO,  0, 0},

    /* 0x12 18 DW_DLA_ABBREV Used by dwarf_get_abbrev() */
    {sizeof(struct Dwarf_Abbrev_s),MULTIPLY_NO,  0, 0},

    /* 0x13 19 DW_DLA_FRAME_INSTR_HEAD */
    {sizeof(struct Dwarf_Frame_Instr_Head_s),MULTIPLY_NO,  0,
        _dwarf_frame_instr_destructor} ,

    /* 0x14  20 DW_DLA_CIE */
    {sizeof(struct Dwarf_Cie_s),MULTIPLY_NO,  0, 0},

    /* 0x15 DW_DLA_FDE */
    {sizeof(struct Dwarf_Fde_s),MULTIPLY_NO,  0,
        _dwarf_fde_destructor},

    /* 0x16 DW_DLA_LOC_BLOCK */
    {1 /*sizeof(Dwarf_Loc)*/,MULTIPLY_CT, 0, 0},

    /* 0x17 DW_DLA_FRAME_OP  UNUSED  */
    {sizeof(int),MULTIPLY_NO, 0, 0},

    /* 0x18 DW_DLA_FUNC UNUSED */
    {sizeof(struct Dwarf_Global_s),MULTIPLY_NO,  0, 0},

    /* 0x19 DW_DLA_UARRAY */
    {sizeof(Dwarf_Off),MULTIPLY_CT,  0, 0},

    /* 0x1a DW_DLA_VAR UNUSED  */
    {sizeof(struct Dwarf_Global_s),MULTIPLY_NO,  0, 0},

    /* 0x1b DW_DLA_WEAK UNUSED */
    {sizeof(struct Dwarf_Global_s),MULTIPLY_NO,  0, 0},

    /* 0x1c DW_DLA_ADDR */
    {1,MULTIPLY_SP, 0, 0},

    /* 0x1d DW_DLA_RANGES */
    {sizeof(Dwarf_Ranges),MULTIPLY_CT, 0,0 },

    /*  The following DW_DLA data types
        are known only inside libdwarf.  */

    /* 0x1e DW_DLA_ABBREV_LIST No longer alloc'd,
        just plain calloc instead.
        Found in each CU_Context  These
        form a singly-linked list. */
    { sizeof(struct Dwarf_Abbrev_List_s),MULTIPLY_NO, 0,0},

    /* 0x1f DW_DLA_CHAIN */
    {sizeof(struct Dwarf_Chain_s),MULTIPLY_NO, 0, 0},

    /* 0x20 DW_DLA_CU_CONTEXT */
    {sizeof(struct Dwarf_CU_Context_s),MULTIPLY_NO,  0, 0},

    /* 0x21 DW_DLA_FRAME */
    {sizeof(struct Dwarf_Frame_s),MULTIPLY_NO,
        _dwarf_frame_constructor,
        _dwarf_frame_destructor},

    /* 0x22 DW_DLA_GLOBAL_CONTEXT */
    {sizeof(struct Dwarf_Global_Context_s),MULTIPLY_NO,  0, 0},

    /* 0x23 DW_DLA_FILE_ENTRY */
    {sizeof(struct Dwarf_File_Entry_s),MULTIPLY_NO,  0, 0},

    /* 0x24 DW_DLA_LINE_CONTEXT */
    {sizeof(struct Dwarf_Line_Context_s),MULTIPLY_NO,
        _dwarf_line_context_constructor,
        _dwarf_line_context_destructor},

    /* 0x25 DW_DLA_LOC_CHAIN */
    {sizeof(struct Dwarf_Loc_Chain_s),MULTIPLY_NO,  0, 0},

    /*  0x26 0x26 DW_DLA_HASH_TABLE No longer used
        as dealloc or dwarf_get_alloc */
    {sizeof(int),MULTIPLY_NO,  0, 0},

    /*  The following really use Global struct: used to be
    unique struct per type, but now merged (11/99).  The
    opaque types are visible in the interface.
    The types  for
    DW_DLA_FUNC, DW_DLA_TYPENAME, DW_DLA_VAR, DW_DLA_WEAK
    also use the global types.  */

    /* 0x27 DW_DLA_FUNC_CONTEXT */
    {sizeof(struct Dwarf_Global_Context_s),MULTIPLY_NO,  0, 0},

    /* 0x28 40 DW_DLA_TYPENAME_CONTEXT */
    {sizeof(struct Dwarf_Global_Context_s),MULTIPLY_NO,  0, 0},

    /* 0x29 41 DW_DLA_VAR_CONTEXT */
    {sizeof(struct Dwarf_Global_Context_s),MULTIPLY_NO,  0, 0},

    /* 0x2a 42 DW_DLA_WEAK_CONTEXT */
    {sizeof(struct Dwarf_Global_Context_s),MULTIPLY_NO,  0, 0},

    /* 0x2b 43 DW_DLA_PUBTYPES_CONTEXT DWARF3 */
    {sizeof(struct Dwarf_Global_Context_s),MULTIPLY_NO,  0, 0},

    /* 0x2c 44 DW_DLA_HASH_TABLE_ENTRY. No longer used. */
    {sizeof(int),MULTIPLY_NO,  0, 0},

    /* 0x2d - 0x34 reserved */
    {sizeof(int),MULTIPLY_NO,  0, 0},

    /* 0x2e 46 reserved for future use  */
    {sizeof(int),MULTIPLY_NO,  0, 0},

    /* 0x2f 47  reserved for future use  */
    {sizeof(int),MULTIPLY_NO,  0, 0},

    /* 0x30 reserved for future internal use */
    {sizeof(int),MULTIPLY_NO,  0, 0},

    /* 0x31 reserved for future internal use */
    {sizeof(int),MULTIPLY_NO,  0, 0},

    /* 0x32 50 reserved for future internal use */
    {sizeof(int),MULTIPLY_NO,  0, 0},

    /* 0x33 51 reserved for future internal use */
    {sizeof(int),MULTIPLY_NO,  0, 0},

    /* 0x34 52 reserved for future internal use */
    {sizeof(int),MULTIPLY_NO,  0, 0},

    /* 0x35 53 Used starting July 2020 DW_DLA_GNU_INDEX_HEAD */
    {sizeof(struct Dwarf_Gnu_Index_Head_s),MULTIPLY_NO,  0,
        _dwarf_gnu_index_head_destructor},

    /* 0x36 54 Used starting May 2020  DW_DLA_RNGLISTS_HEAD */
    {sizeof(struct Dwarf_Rnglists_Head_s),MULTIPLY_NO,  0,
        _dwarf_rnglists_head_destructor},

    /*  now,  we have types that are public. */
    /* 0x37 55.  New in June 2014. Gdb. */
    {sizeof(struct Dwarf_Gdbindex_s),MULTIPLY_NO,  0, 0},

    /* 0x38 56.  New in July 2014. */
    /* DWARF5 DebugFission dwp file sections
        .debug_cu_index and .debug_tu_index . */
    {sizeof(struct Dwarf_Xu_Index_Header_s),MULTIPLY_NO,  0, 0},

    /*  These required by new features in DWARF5. Also usable
        for DWARF2,3,4. */
    /* 0x39 57 DW_DLA_LOC_BLOCK_C DWARF5 */
    {sizeof(struct Dwarf_Loc_Expr_Op_s),MULTIPLY_CT, 0, 0},

    /* 0x3a 58  DW_DLA_LOCDESC_C */
    {sizeof(struct Dwarf_Locdesc_c_s),MULTIPLY_CT,
        _dwarf_locdesc_c_constructor, 0},

    /* 0x3b 59 DW_DLA_LOC_HEAD_C  */
    {sizeof(struct Dwarf_Loc_Head_c_s),MULTIPLY_NO, 0,
        _dwarf_loclists_head_destructor},

    /* 0x3c 60 DW_DLA_MACRO_CONTEXT */
    {sizeof(struct Dwarf_Macro_Context_s),MULTIPLY_NO,
        _dwarf_macro_constructor,0},

    /* 0x3d 61 DW_DLA_CHAIN_2 */
    {sizeof(struct Dwarf_Chain_o),MULTIPLY_NO, 0, 0},

    /* 0x3e 62 DW_DLA_DSC_HEAD */
    {sizeof(struct Dwarf_Dsc_Head_s),MULTIPLY_NO, 0,
        _dwarf_dsc_destructor},

    /* 0x3f 63 DW_DLA_DNAMES_HEAD */
    {sizeof(struct Dwarf_Dnames_Head_s),MULTIPLY_NO, 0,
        _dwarf_dnames_destructor},

    /* 0x40 64 DW_DLA_STR_OFFSETS */
    {sizeof(struct Dwarf_Str_Offsets_Table_s),MULTIPLY_NO, 0,0},

    /* 0x41 65 DW_DLA_DEBUG_ADDR */
    {sizeof(struct Dwarf_Debug_Addr_Table_s),MULTIPLY_NO, 0,0},
};

/*  We are simply using the incoming pointer as the key-pointer.
*/

static DW_TSHASHTYPE
simple_value_hashfunc(const void *keyp)
{
    DW_TSHASHTYPE up = (DW_TSHASHTYPE)(uintptr_t)keyp;
    return up;
}
/*  We did alloc something but not a fixed-length thing.
    Instead, it starts with some special data we noted.
    The incoming pointer is to the caller data, we
    destruct based on caller, but find the special
    extra data in a prefix area. */
static void
tdestroy_free_node(void *nodep)
{
    char                  *m = 0;
    char                  *malloc_addr =  0;
    struct reserve_data_s *reserve =  0;
    unsigned int           type = 0;

    m = (char *)nodep;
    if ((uintptr_t)m > DW_RESERVE) {
        malloc_addr = m - DW_RESERVE;
    } else {
        /* impossible */
        return;
    }
    reserve = (struct reserve_data_s *)malloc_addr;
    type = reserve->rd_type;
    if (type >= ALLOC_AREA_INDEX_TABLE_MAX) {
        /* Internal error, corrupted data. */
        return;
    }
    if (!reserve->rd_dbg) {
        /*  Unused (corrupted?) node in the tree.
            Should never happen. */
        return;
    }
    if (!reserve->rd_type) {
        /*  Unused (corrupted?) node in the tree.
            Should never happen. */
        return;
    }
    if (alloc_instance_basics[type].specialdestructor) {
        alloc_instance_basics[type].specialdestructor(m);
    }
    free(malloc_addr);
}

/*  The sort of hash table entries result in very simple
    helper functions. */
static int
simple_compare_function(const void *l, const void *r)
{
    DW_TSHASHTYPE lp = (DW_TSHASHTYPE)(uintptr_t)l;
    DW_TSHASHTYPE rp = (DW_TSHASHTYPE)(uintptr_t)r;
    if (lp < rp) {
        return -1;
    }
    if (lp > rp) {
        return 1;
    }
    return 0;
}

/*  This function returns a pointer to a region
    of memory.  For alloc_types that are not
    strings or lists of pointers, only 1 struct
    can be requested at a time.  This is indicated
    by an input count of 1.  For strings, count
    equals the length of the string it will
    contain, i.e it the length of the string
    plus 1 for the terminating null.  For lists
    of pointers, count is equal to the number of
    pointers.  For DW_DLA_FRAME_BLOCK, DW_DLA_RANGES, and
    DW_DLA_LOC_BLOCK allocation types also, count
    is the count of the number of structs needed.

    This function cannot be used to allocate a
    Dwarf_Debug_s struct.  */
/* coverity[+alloc] */
char *
_dwarf_get_alloc(Dwarf_Debug dbg,
    Dwarf_Small alloc_type, Dwarf_Unsigned count)
{
    char * alloc_mem = 0;
    Dwarf_Unsigned basesize = 0;
    Dwarf_Unsigned size = 0;
    unsigned int type = alloc_type;
    short action = 0;

    if (IS_INVALID_DBG(dbg)) {
#if DEBUG_ALLOC
        printf("libdwarfdetector ALLOC dbg null  "
            "ret NULL type 0x%x size %lu line %d %s\n",
            (unsigned)alloc_type,(unsigned long)size,
            __LINE__,__FILE__);
        fflush(stdout);
#endif /* DEBUG_ALLOC */
        return NULL;
    }
    if (type >= ALLOC_AREA_INDEX_TABLE_MAX) {
        /* internal error */
#if DEBUG_ALLOC
        printf("libdwarfdetector ALLOC type bad ret null  "
            "ret NULL type 0x%x size %lu line %d %s\n",
            (unsigned)alloc_type,(unsigned long)size,
            __LINE__,__FILE__);
        fflush(stdout);
#endif /* DEBUG_ALLOC */
        return NULL;
    }
    basesize = alloc_instance_basics[alloc_type].ia_struct_size;
    action = alloc_instance_basics[alloc_type].ia_multiply_count;
    if (action == MULTIPLY_NO) {
        /* Usually count is 1, but do not assume it. */
        size = basesize;
    } else if (action == MULTIPLY_CT) {
        size = basesize * count;
    }  else {
        /* MULTIPLY_SP */
        /* DW_DLA_ADDR.. count * largest size */
        size = count *
            (sizeof(Dwarf_Addr) > sizeof(Dwarf_Off) ?
            sizeof(Dwarf_Addr) : sizeof(Dwarf_Off));
    }
    size += DW_RESERVE;
    alloc_mem = malloc(size);
    if (!alloc_mem) {
        return NULL;
    }
    {
        char * ret_mem = alloc_mem + DW_RESERVE;
        void *key = ret_mem;
        struct reserve_data_s *r = (struct reserve_data_s*)alloc_mem;
        void *result = 0;

        memset(alloc_mem, 0, size);
        /* We are not actually using rd_dbg, we are using rd_type. */
        r->rd_dbg = dbg;
        r->rd_type = (unsigned short)alloc_type;
        /*  The following is wrong for large records, but
            it's not important, so let it be truncated.*/
        r->rd_length = (unsigned short)size;
        if (alloc_instance_basics[type].specialconstructor) {
            int res = alloc_instance_basics[type].
                specialconstructor(dbg, ret_mem);
            if (res != DW_DLV_OK) {
                /*  We leak what we allocated in
                    _dwarf_find_memory when
                    constructor fails. */
#if DEBUG_ALLOC
    printf("libdwarfdetector ALLOC constructor fails ret NULL "
        "type 0x%x size %lu line %d %s\n",
        (unsigned)alloc_type,(unsigned long)size,__LINE__,__FILE__);
    fflush(stdout);
#endif /* DEBUG_ALLOC */
                return NULL;
            }
        }
        /*  See global flag.
            If zero then caller chooses not
            to track allocations, so dwarf_finish()
            is unable to free anything the caller
            omitted to dealloc. Normally
            the global flag is non-zero */
        /*  As of March 14, 2020 it's
            not necessary to test for alloc type, but instead
            only call tsearch if de_alloc_tree_on. */
        if (global_de_alloc_tree_on) {
            result = dwarf_tsearch((void *)key,
                &dbg->de_alloc_tree,simple_compare_function);
            if (!result) {
                /*  Something badly wrong. Out of memory.
                    pretend all is well. */
            }
        }
#if DEBUG_ALLOC
        printf("\nlibdwarfdetector ALLOC ret 0x%lx type 0x%x "
            "size %lu line %d %s\n",
            (unsigned long)ret_mem,(unsigned)alloc_type,
            (unsigned long)size,__LINE__,__FILE__);
        fflush(stdout);
#endif /* DEBUG_ALLOC */
        return (ret_mem);
    }
}

/*  This was once a long list of tests using dss_data
    and dss_size to see if 'space' was inside a debug section.
    This tfind approach removes that maintenance headache. */
static int
string_is_in_debug_section(Dwarf_Debug dbg,void * space)
{
    /*  See dwarf_line.c dwarf_srcfiles()
        for one way we can wind up with
        a DW_DLA_STRING string that may or may not be malloc-ed
        by _dwarf_get_alloc().

        dwarf_formstring(), for example, returns strings
        which point into .debug_info or .debug_types but
        dwarf_dealloc is never supposed to be applied
        to strings dwarf_formstring() returns!

        Lots of calls returning strings
        have always been documented as requiring
        dwarf_dealloc(...DW_DLA_STRING) when the code
        just returns a pointer to a portion of a loaded section!
        It is too late to change the documentation. */

    void *result = 0;

    /* The alloc tree can be in main or tied or both. */
    result = dwarf_tfind((void *)space,
        &dbg->de_alloc_tree,simple_compare_function);
    if (!result) {
        /*  Not in the tree, so not malloc-ed
            Nothing to delete. */
        return TRUE;
    }
    /*  We found the address in the tree, so it is NOT
        part of .debug_info or any other dwarf section,
        but is space malloc-d in _dwarf_get_alloc(). */
    return FALSE;
}

static enum Dwarf_Sec_Alloc_Pref _dwarf_global_load_preference =
    Dwarf_Alloc_Malloc;

/*  If zero passed in this just returns the current
    global preference, setting nothing */
enum Dwarf_Sec_Alloc_Pref
dwarf_set_load_preference(
    enum Dwarf_Sec_Alloc_Pref dw_load_preference)
{
    enum Dwarf_Sec_Alloc_Pref prev_load_pref =
        _dwarf_global_load_preference;
#ifdef HAVE_FULL_MMAP
    /*  Only set the preference if MMAP is available. */
    switch(dw_load_preference) {
    case  Dwarf_Alloc_Malloc:
    case  Dwarf_Alloc_Mmap:
        _dwarf_global_load_preference = dw_load_preference;
        break;
    case  Dwarf_Alloc_None:
        break; /* ignore */
    default: break;
    }
#else
    (void)dw_load_preference;
#endif
    return prev_load_pref;
}
int
dwarf_get_mmap_count(Dwarf_Debug dbg,
    Dwarf_Unsigned *dw_mmap_count,
    Dwarf_Unsigned *dw_mmap_size,
    Dwarf_Unsigned *dw_malloc_count,
    Dwarf_Unsigned *dw_malloc_size)
{
    unsigned long  total_entries =
        dbg->de_debug_sections_total_entries;
    unsigned  long i = 0;
    Dwarf_Unsigned mma_count = 0;
    Dwarf_Unsigned mma_size = 0;
    Dwarf_Unsigned mal_count = 0;
    Dwarf_Unsigned mal_size = 0;

    for ( ; i < total_entries; ++i) {
        struct Dwarf_Section_s *sec =
            dbg->de_debug_sections[i].ds_secdata;

        if (!sec->dss_size) {
            continue;
        }
        switch(sec->dss_actual_load_type) {
        case  Dwarf_Alloc_Malloc:
            mal_count++;
            mal_size += sec->dss_size;
            break;
        case  Dwarf_Alloc_Mmap:
            mma_count++;
            mma_size += sec->dss_size;
            break;
        case  Dwarf_Alloc_None:
        default:
            break;
        }
    }
    if (dw_mmap_count) {
        *dw_mmap_count = mma_count;
    }
    if (dw_mmap_size) {
        *dw_mmap_size = mma_size;
    }
    if (dw_malloc_count) {
        *dw_malloc_count = mal_count;
    }
    if (dw_malloc_size) {
        *dw_malloc_size = mal_size;
    }
    return DW_DLV_OK;
}

enum Dwarf_Sec_Alloc_Pref
_dwarf_determine_section_allocation_type(void)
{
#ifndef HAVE_FULL_MMAP
    return _dwarf_global_load_preference;
#else
    char *whichalloc = getenv("DWARF_WHICH_ALLOC");

    if (whichalloc) {
        if (!strcmp(whichalloc,"mmap")) {
            dwarf_set_load_preference(Dwarf_Alloc_Mmap);
            return Dwarf_Alloc_Mmap;
        }
        if (!strcmp(whichalloc,"malloc")) {
            dwarf_set_load_preference(Dwarf_Alloc_Malloc);
            return Dwarf_Alloc_Malloc;
        }
    }
    return _dwarf_global_load_preference;
#endif /* HAVE_FULL_MMAP */
}

/*  These wrappers for dwarf_dealloc enable type-checking
    at call points. */
void
dwarf_dealloc_error(Dwarf_Debug dbg, Dwarf_Error err)
{
    dwarf_dealloc(dbg,err,DW_DLA_ERROR);
}
void
dwarf_dealloc_die( Dwarf_Die die)
{
    Dwarf_Debug dbg = 0;
    Dwarf_CU_Context context = 0;

    if (!die) {
#ifdef DEBUG_ALLOC
        printf("DEALLOC die does nothing, die NULL line %d %s\n",
            __LINE__,__FILE__);
        fflush(stdout);
#endif /* DEBUG_ALLOC */
        return;
    }
    context = die->di_cu_context;
    if (!context) {
#ifdef DEBUG_ALLOC
        printf("DEALLOC die does nothing, context NULL line %d %s\n",
            __LINE__,__FILE__);
        fflush(stdout);
#endif /* DEBUG_ALLOC */
        return;
    }
    dbg = context->cc_dbg;
    if (IS_INVALID_DBG(dbg)) {
        return;
    }
    dwarf_dealloc(dbg,die,DW_DLA_DIE);
}

void
dwarf_dealloc_attribute(Dwarf_Attribute attr)
{
    Dwarf_Debug dbg = 0;

    if (!attr) {
#ifdef DEBUG_ALLOC
        printf("DEALLOC does nothing, attr is NULL line %d %s\n",
            __LINE__,__FILE__);
        fflush(stdout);
#endif /* DEBUG_ALLOC */
        return;
    }
    dbg = attr->ar_dbg;
    dwarf_dealloc(dbg,attr,DW_DLA_ATTR);
}
/*
    This function is used to deallocate a region of memory
    that was obtained by a call to _dwarf_get_alloc.  Note
    that though dwarf_dealloc() is a public function,
    _dwarf_get_alloc() isn't.

    For lists, typically arrays of pointers, it is assumed
    that the space was allocated by a direct call to malloc,
    and so a straight free() is done.  This is also the case
    for variable length blocks such as DW_DLA_FRAME_BLOCK
    and DW_DLA_LOC_BLOCK and DW_DLA_RANGES.

    For strings, the pointer might point to a string in
    .debug_info or .debug_string.  After this is checked,
    and if found not to be the case, a free() is done,
    again on the assumption that a malloc was used to
    obtain the space.

    This function does not return anything.
    The _dwarf_error_destructor() will be called
    to free the er_msg string
    (if this is a Dwarf_Error) just before the
    Dwarf_Error is freed here. See...specialdestructor()
    below.

*/
/* coverity[+free : arg-1] */
void
dwarf_dealloc(Dwarf_Debug dbg,
    Dwarf_Ptr space, Dwarf_Unsigned alloc_type)
{
    unsigned int type = 0;
    char * malloc_addr = 0;
    struct reserve_data_s * r = 0;

    if (!space) {
#ifdef DEBUG_ALLOC
        printf("DEALLOC does nothing, space NULL line %d %s\n",
            __LINE__,__FILE__);
        fflush(stdout);
#endif /* DEBUG_ALLOC*/
        return;
    }
    if (IS_INVALID_DBG(dbg)) {
        /*  App error, or an app that failed in a
            dwarf_init*() or dwarf_elf_init*() call.

        */
        dw_empty_errlist_item(space);
#ifdef DEBUG_ALLOC
        printf( "DEALLOC dbg NULL line %d %s\n",
            __LINE__,__FILE__);
        fflush(stdout);
#endif /* DEBUG_ALLOC*/
        return;
    }
    if (space == (Dwarf_Ptr)&_dwarf_failsafe_error) {
#ifdef DEBUG_ALLOC
        printf("DEALLOC failsafe requested at 0x%lx. "
            "ignore. line %d %s\n",
            (unsigned long)space,
            __LINE__,__FILE__);
        fflush(stdout);
        return;
#endif /* DEBUG_ALLOC*/
    }
    if (dbg && alloc_type == DW_DLA_ERROR) {
        dbg = dbg->de_errors_dbg;
    }
    if (dbg && dbg->de_alloc_tree) {
        /*  If it's a string in debug_info etc doing
            (char *)space - DW_RESERVE is totally bogus. */
        if (alloc_type == DW_DLA_STRING &&
            string_is_in_debug_section(dbg,space)) {
            /*  A string pointer may point into .debug_info or
                .debug_string etc.
                So must not be freed.  And strings have
                no need of a specialdestructor().
                Mostly a historical mistake here.
                Corrected in libdwarf March 14,2020. */
#ifdef DEBUG_ALLOC
            printf( "DEALLOC string in section, no dealloc "
                "line %d %s\n", __LINE__,__FILE__);
            fflush(stdout);
#endif /* DEBUG_ALLOC*/
            return;
        }
    }
    /*  Otherwise it might be allocated string so it is ok
        do the (char *)space - DW_RESERVE  */

    /*  If it's a DW_DLA_STRING case and erroneous
        the following pointer operations might
        result in a coredump if the pointer
        is to the beginning of a string section.
        If not DW_DLA_STRING
        no correctly written caller could coredump
        here.  */
    if ((uintptr_t)space > DW_RESERVE) {
        malloc_addr = (char *)space - DW_RESERVE;
    } else {
        /* Impossible */
        return;
    }
    r =(struct reserve_data_s *)malloc_addr;
    if (dbg && dbg != r->rd_dbg) {
        /*  Mixed up or originally a no_dbg alloc */
#ifdef DEBUG_ALLOC
        printf("DEALLOC find was NULL  dbg 0x%lx "
            "rd_dbg 0x%lx space 0x%lx line %d %s\n",
            (unsigned long)dbg,
            (unsigned long)r->rd_dbg,
            (unsigned long)space,
            __LINE__,__FILE__);
        fflush(stdout);
#endif /* DEBUG_ALLOC*/
    }
    if (dbg && alloc_type != r->rd_type) {
        /*  Something is mixed up. */
#ifdef DEBUG_ALLOC
        printf("DEALLOC does nothing, type 0x%lx rd_type 0x%lx"
            " space 0x%lx line %d %s\n",
            (unsigned long)alloc_type,
            (unsigned long)r->rd_type,
            (unsigned long)space,
            __LINE__,__FILE__);
        fflush(stdout);
#endif /* DEBUG_ALLOC*/
        return;
    }
    if (alloc_type == DW_DLA_ERROR) {
        Dwarf_Error ep = (Dwarf_Error)space;

        if (ep->er_static_alloc == DE_STATIC) {
            /*  This is special, malloc arena
                was exhausted or a NULL dbg
                was used for the error because the real
                dbg was unavailable.
                There is nothing to delete, really.
                Set er_errval to signal that the
                space was dealloc'd. */
            _dwarf_failsafe_error.er_errval =
                DW_DLE_FAILSAFE_ERRVAL;
            _dwarf_error_destructor(ep);
#ifdef DEBUG_ALLOC
            printf("DEALLOC does nothing, DE_STATIC line %d %s\n",
                __LINE__,__FILE__);
            fflush(stdout);
#endif /* DEBUG_ALLOC*/
            return;
        }
        if (ep->er_static_alloc == DE_MALLOC) {
            /*  This is special, we had no arena
                but have a full special area as normal. */
#ifdef DEBUG_ALLOC
            printf("DEALLOC does free, DE_MALLOC line %d %s\n",
                __LINE__,__FILE__);
            fflush(stdout);
#endif /* DEBUG_ALLOC*/
            _dwarf_remove_from_staticerrlist(space);
        }
        /* Was normal alloc, use normal dealloc. */
        /* DW_DLA_ERROR has a specialdestructor */
    }
    /*  alloc types are a defined library-private
        set of integers. Less than 256 of them. */
    type = (unsigned int)alloc_type;
#if DEBUG_ALLOC
    if (dbg != r->rd_dbg) {
        printf("DEALLOC  dbg != rd_dbg"
            " going ahead line %d %s\n",
            __LINE__,__FILE__);
        fflush(stdout);
    }
    printf("libdwarfdetector DEALLOC ret 0x%lx type 0x%x "
        "size %lu line %d %s\n",
        (unsigned long)space,(unsigned)type,
        (unsigned long)r->rd_length,__LINE__,__FILE__);
#endif /* DEBUG_ALLOC*/
    if (type >= ALLOC_AREA_INDEX_TABLE_MAX) {
        /* internal or user app error */
#ifdef DEBUG_ALLOC
        printf("DEALLOC does nothing, type too big %lu line %d %s\n",
            (unsigned long)type,
            __LINE__,__FILE__);
        fflush(stdout);
#endif /* DEBUG_ALLOC*/
        return;
    }
    if (alloc_instance_basics[type].specialdestructor) {
        alloc_instance_basics[type].specialdestructor(space);
    }
    if (dbg && dbg->de_alloc_tree) {
        /*  The 'space' pointer we get points after the
            reserve space.  The key is 'space'
            and address to free
            is just a few bytes before 'space'. */
        void *key = space;

        dwarf_tdelete(key,&dbg->de_alloc_tree,
            simple_compare_function);
        /*  If dwarf_tdelete returns NULL it might mean
            a) tree is empty.
            b) If hashsearch, then a single chain might
                now be empty,
                so we do not know of a 'parent node'.
            c) We did not find that key, we did nothing.

            In any case, we simply don't worry about it.
            Not Supposed To Happen. */
    }
    r->rd_dbg  = (void *)(uintptr_t)0xfeadbeef;
    r->rd_length = 0;
    r->rd_type = 0;
    free(malloc_addr);
    return;
}

/*
    Allocates space for a Dwarf_Debug_s struct,
    since one does not exist.
*/
Dwarf_Debug
_dwarf_get_debug(Dwarf_Unsigned filesize)
{
    Dwarf_Debug dbg;

    dbg = (Dwarf_Debug) malloc(sizeof(struct Dwarf_Debug_s));
    if (!dbg) {
        return NULL;
    }
    memset(dbg, 0, sizeof(struct Dwarf_Debug_s));
    /* Set up for a dwarf_tsearch hash table */
    dbg->de_magic = DBG_IS_VALID;

    /*  See also dwarf_tsearchhash.c the prime number
        table 'primes[]'. */
#define INIT_HASH_INIT_LIMIT 2000000
    if (global_de_alloc_tree_on) {
        /*  The type of the dwarf_initialize_search_hash
            initial-size argument */
        unsigned long size_est = (unsigned long)(filesize/30);

        if (size_est > INIT_HASH_INIT_LIMIT) {
            size_est = INIT_HASH_INIT_LIMIT;
        }
#ifdef TESTINGHASHTAB
        printf("debugging: src filesize %lu hashtab init %lu\n",
            (unsigned long)filesize,size_est);
#endif
        dwarf_initialize_search_hash(&dbg->de_alloc_tree,
            simple_value_hashfunc,size_est);
    }
    return dbg;
}

/*  In the 'rela' relocation case  or in case
    of compressed sections we might have malloc'd
    space (to ensure it is read-write or to decompress it
    respectively, or both). In that case, free the space.
    */
void
_dwarf_malloc_section_free(struct Dwarf_Section_s * sec)
{
    /*  Compressed sections will be malloc not mmap
        by the time we get here.
        No matter what the preference was.  */
    switch(sec->dss_actual_load_type) {
    case Dwarf_Alloc_Malloc:
        if (sec->dss_was_alloc) {
            free(sec->dss_data);
        }
        break;
    case Dwarf_Alloc_Mmap:
#ifdef HAVE_FULL_MMAP
        if (sec->dss_was_alloc) {
            int res = munmap(sec->dss_mmap_realarea,
                sec->dss_computed_mmap_len);
#ifdef DEBUG_ALLOC
            if (res) {
                printf("FAILED to munmap!\n");
                fflush(stdout);
            }
#endif /* DEBUG_ALLOC */
            (void)res; /* To avoid compiler warning. */
        }
#endif /* HAVE_FULL_MMAP */
        break;
    case Dwarf_Alloc_None:
    default:
    break;
    }
    sec->dss_data = 0;
    /* sec->dss_size = 0; */
    sec->dss_was_alloc = FALSE;
    sec->dss_mmap_realarea = 0;
    sec->dss_computed_mmap_len = 0;
    sec->dss_computed_mmap_offset = 0;
}

static void
freecontextlist(Dwarf_Debug dbg, Dwarf_Debug_InfoTypes dis)
{
    Dwarf_CU_Context context = 0;
    Dwarf_CU_Context nextcontext = 0;
    for (context = dis->de_cu_context_list;
        context; context = nextcontext) {
        Dwarf_Hash_Table hash_table = 0;

        hash_table = context->cc_abbrev_hash_table;

        _dwarf_free_abbrev_hash_table_contents(hash_table,
            FALSE);
        hash_table->tb_entries = 0;
        nextcontext = context->cc_next;
        context->cc_next = 0;
        /*  See also  local_dealloc_cu_context() in
            dwarf_die_deliv.c */
        free(hash_table);
        context->cc_abbrev_hash_table = 0;
        dwarf_dealloc(dbg, context, DW_DLA_CU_CONTEXT);
    }
    dis->de_cu_context_list = 0;
}

/*
    Used to free all space allocated for this Dwarf_Debug.
    The caller should assume that the Dwarf_Debug pointer
    itself is no longer valid upon return from this function.

    NEVER returns DW_DLV_ERROR.

    In case of difficulty, this function simply returns quietly.
*/
int
_dwarf_free_all_of_one_debug(Dwarf_Debug dbg)
{
    unsigned g = 0;

    if (IS_INVALID_DBG(dbg)) {
        _dwarf_free_static_errlist();
        return DW_DLV_NO_ENTRY;
    }
    /*  To do complete validation that we have no surprising
        missing or erroneous deallocs it is advisable to do
        the dwarf_deallocs here
        that are not things the user can otherwise request.
        Housecleaning.  */
    if (dbg->de_cu_hashindex_data) {
        dwarf_dealloc_xu_header(dbg->de_cu_hashindex_data);
        dbg->de_cu_hashindex_data = 0;
    }
    if (dbg->de_tu_hashindex_data) {
        dwarf_dealloc_xu_header(dbg->de_tu_hashindex_data);
        dbg->de_tu_hashindex_data = 0;
    }
    if (dbg->de_printf_callback_null_device_handle) {
        fclose(dbg->de_printf_callback_null_device_handle);
        dbg->de_printf_callback_null_device_handle = 0;
    }
    freecontextlist(dbg,&dbg->de_info_reading);
    freecontextlist(dbg,&dbg->de_types_reading);
    /* Housecleaning done. Now really free all the space. */
    _dwarf_malloc_section_free(&dbg->de_debug_info);
    _dwarf_malloc_section_free(&dbg->de_debug_types);
    _dwarf_malloc_section_free(&dbg->de_debug_abbrev);
    _dwarf_malloc_section_free(&dbg->de_debug_line);
    _dwarf_malloc_section_free(&dbg->de_debug_line_str);
    _dwarf_malloc_section_free(&dbg->de_debug_loc);
    _dwarf_malloc_section_free(&dbg->de_debug_aranges);
    _dwarf_malloc_section_free(&dbg->de_debug_macinfo);
    _dwarf_malloc_section_free(&dbg->de_debug_macro);
    _dwarf_malloc_section_free(&dbg->de_debug_names);
    _dwarf_malloc_section_free(&dbg->de_debug_pubnames);
    _dwarf_malloc_section_free(&dbg->de_debug_str);
    _dwarf_malloc_section_free(&dbg->de_debug_sup);
    _dwarf_malloc_section_free(&dbg->de_debug_frame);
    _dwarf_malloc_section_free(&dbg->de_debug_frame_eh_gnu);
    _dwarf_malloc_section_free(&dbg->de_debug_pubtypes);
    _dwarf_malloc_section_free(&dbg->de_debug_funcnames);
    _dwarf_malloc_section_free(&dbg->de_debug_typenames);
    _dwarf_malloc_section_free(&dbg->de_debug_varnames);
    _dwarf_malloc_section_free(&dbg->de_debug_weaknames);
    _dwarf_malloc_section_free(&dbg->de_debug_ranges);
    _dwarf_malloc_section_free(&dbg->de_debug_str_offsets);
    _dwarf_malloc_section_free(&dbg->de_debug_addr);
    _dwarf_malloc_section_free(&dbg->de_debug_gdbindex);
    _dwarf_malloc_section_free(&dbg->de_debug_cu_index);
    _dwarf_malloc_section_free(&dbg->de_debug_tu_index);
    _dwarf_malloc_section_free(&dbg->de_debug_loclists);
    _dwarf_malloc_section_free(&dbg->de_debug_rnglists);
    _dwarf_malloc_section_free(&dbg->de_gnu_debuglink);
    _dwarf_malloc_section_free(&dbg->de_note_gnu_buildid);
    _dwarf_harmless_cleanout(&dbg->de_harmless_errors);

    _dwarf_dealloc_rnglists_context(dbg);
    _dwarf_dealloc_loclists_context(dbg);
    if (dbg->de_printf_callback.dp_buffer &&
        !dbg->de_printf_callback.dp_buffer_user_provided ) {
        free(dbg->de_printf_callback.dp_buffer);
    }

    _dwarf_destroy_group_map(dbg);
    /*  de_alloc_tree might be NULL if
        global_de_alloc_tree_on is zero. */
    if (dbg->de_alloc_tree) {
        dbg->de_in_tdestroy = TRUE;
        dwarf_tdestroy(dbg->de_alloc_tree,tdestroy_free_node);
        dbg->de_in_tdestroy = FALSE;
        dbg->de_alloc_tree = 0;
    }
    _dwarf_free_static_errlist();
    /*  first, walk the search and free()
        contents. */
    /*  Now  do the search tree itself */
    if (dbg->de_tied_data.td_tied_search) {
        dwarf_tdestroy(dbg->de_tied_data.td_tied_search,
            _dwarf_tied_destroy_free_node);
        dbg->de_tied_data.td_tied_search = 0;
    }
    free((void *)dbg->de_path);
    dbg->de_path = 0;
    for (g = 0; g < dbg->de_gnu_global_path_count; ++g) {
        free((char *)dbg->de_gnu_global_paths[g]);
        dbg->de_gnu_global_paths[g] = 0;
    }
    free((void*)dbg->de_gnu_global_paths);
    dbg->de_gnu_global_paths = 0;
    dbg->de_gnu_global_path_count = 0;
    memset(dbg, 0, sizeof(*dbg)); /* Prevent accidental use later. */
    free(dbg);
    return DW_DLV_OK;
}
/*  A special case: we have no dbg, no alloc header etc.
    So create something out of thin air that we can recognize
    in dwarf_dealloc.
    Something with the prefix (prefix space hidden from caller).

    Only applies to DW_DLA_ERROR, and  making up an error record.

    dwarf_error.c calls this and it adds to the staticerrlist
    all of which is freed by free_static_errlist();
*/
struct Dwarf_Error_s *
_dwarf_special_no_dbg_error_malloc(void)
{
    Dwarf_Error e = 0;
    Dwarf_Unsigned len =  0;
    struct reserve_data_s *base = 0;
    char *mem = 0;

    len = sizeof(struct Dwarf_Error_s) + DW_RESERVE;
    mem = (char *)malloc((size_t)len);
    if (!mem) {
        return 0;
    }
    memset(mem, 0, len);
    base = (struct reserve_data_s *)mem;
    base->rd_dbg = 0;
    base->rd_length = (unsigned short)sizeof(struct Dwarf_Error_s);
    base->rd_type = DW_DLA_ERROR;
    e = (Dwarf_Error)(mem+DW_RESERVE);
    e->er_static_alloc = DE_MALLOC;
    return e;
}
