/*
  Copyright (C) 2017-2018 David Anderson. All Rights Reserved.

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

#include <stdlib.h> /* calloc() free() */
#include <string.h>  /* strcmp() */

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
#include "dwarf_util.h"
#include "dwarf_error.h"
#include "dwarf_tsearch.h"

#define HASHSEARCH

/*  It has not escaped our attention that the section-group
    tsearch hash table could
    be replaced by a simple array with space for each possible
    section number, each element being the group number.
    This would be much simpler than what follows here. */

/*  Each section number can appear in at most one record in the hash
    because each section belongs in only one group.
    Each group number appears as often as appropriate. */

struct Dwarf_Group_Map_Entry_s {
    unsigned  gm_key;  /* section number */
    unsigned  gm_group_number; /* What group number is. */

    /*  The name is from static storage or from elf,
        so there is nothing to free on record delete. */
    const char * gm_section_name;
};

static void *
grp_make_entry(unsigned section, unsigned group,const char *name)
{
    struct Dwarf_Group_Map_Entry_s *e = 0;
    e = calloc(1,sizeof(struct Dwarf_Group_Map_Entry_s));
    if (e) {
        e->gm_key =    section;
        e->gm_group_number = group;
        e->gm_section_name = name;
    }
    return e;
}

static DW_TSHASHTYPE
grp_data_hashfunc(const void *keyp)
{
    const struct Dwarf_Group_Map_Entry_s * enp = keyp;
    DW_TSHASHTYPE hashv = 0;

    hashv = enp->gm_key;
    return hashv;
}

static int
grp_compare_function(const void *l, const void *r)
{
    const struct Dwarf_Group_Map_Entry_s * lp = l;
    const struct Dwarf_Group_Map_Entry_s * rp = r;

    if (lp->gm_key < rp->gm_key) {
        return -1;
    }
    if (lp->gm_key > rp->gm_key) {
        return 1;
    }

    /* match. */
    return 0;
}

int
_dwarf_insert_in_group_map(Dwarf_Debug dbg,
    unsigned groupnum,
    unsigned section_index,
    const char *name,
    Dwarf_Error * error)
{
    struct Dwarf_Group_Data_s *grp = &dbg->de_groupnumbers;

    void *entry2 = 0;
    struct Dwarf_Group_Map_Entry_s * entry3 = 0;

    if (!grp->gd_map) {
        /*  Number of sections is a kind of decent guess
            as to how much space would be useful. */
        dwarf_initialize_search_hash(&grp->gd_map,
            grp_data_hashfunc,grp->gd_number_of_sections);
        if (!grp->gd_map) {
            /*  It's really an error I suppose. */
            return DW_DLV_NO_ENTRY;
        }
    }
    entry3 = grp_make_entry(section_index,groupnum,name);
    if (!entry3) {
        _dwarf_error(dbg, error, DW_DLE_GROUP_MAP_ALLOC);
        return DW_DLV_ERROR;
    }
    entry2 = dwarf_tsearch(entry3,&grp->gd_map,grp_compare_function);
    if (!entry2) {
        free(entry3);
        _dwarf_error(dbg, error, DW_DLE_GROUP_MAP_ALLOC);
        return DW_DLV_ERROR;
    } else {
        struct Dwarf_Group_Map_Entry_s *re = 0;
        re = *(struct Dwarf_Group_Map_Entry_s **)entry2;
        if (re != entry3) {
            free(entry3);
            _dwarf_error(dbg, error, DW_DLE_GROUP_MAP_DUPLICATE);
            return DW_DLV_ERROR;
        } else {
            ++grp->gd_map_entry_count;
            /* OK. Added. Fall thru */
        }
    }
    return DW_DLV_OK;
}

int
_dwarf_section_get_target_group_from_map(Dwarf_Debug dbg,
    unsigned   obj_section_index,
    unsigned * groupnumber_out,
    Dwarf_Error    * error)
{
    struct Dwarf_Group_Map_Entry_s entry;
    struct Dwarf_Group_Map_Entry_s *entry2;
    struct Dwarf_Group_Data_s *grp = &dbg->de_groupnumbers;

    (void)error;
    if (!grp->gd_map) {
        return DW_DLV_NO_ENTRY;
    }
    entry.gm_key = obj_section_index;
    entry.gm_group_number = 0; /* FAKE */
    entry.gm_section_name = ""; /* FAKE */

    entry2 = dwarf_tfind(&entry, &grp->gd_map,grp_compare_function);
    if (entry2) {
        struct Dwarf_Group_Map_Entry_s *e2 =
            *(struct Dwarf_Group_Map_Entry_s **)entry2;;
        *groupnumber_out = e2->gm_group_number;
        return DW_DLV_OK;
    }
    return DW_DLV_NO_ENTRY;
}

/*  New May 2017.  So users can find out what groups (dwo or COMDAT)
    are in the object and how much to allocate so one can get the
    group-section map data. */
int
dwarf_sec_group_sizes(Dwarf_Debug dbg,
    Dwarf_Unsigned * section_count_out,
    Dwarf_Unsigned * group_count_out,
    Dwarf_Unsigned * selected_group_out,
    Dwarf_Unsigned * map_entry_count_out,
    Dwarf_Error    * error)
{
    struct Dwarf_Group_Data_s *grp = 0;

    CHECK_DBG(dbg,error,"dwarf_sec_group_sizes()");
    grp = &dbg->de_groupnumbers;
    *section_count_out   = grp->gd_number_of_sections;
    *group_count_out     = grp->gd_number_of_groups;
    *selected_group_out  = dbg->de_groupnumber;
    *map_entry_count_out = grp->gd_map_entry_count;
    return DW_DLV_OK;
}

static Dwarf_Unsigned map_reccount = 0;
static struct temp_map_struc_s {
    Dwarf_Unsigned section;
    Dwarf_Unsigned group;
    const char *name;
} *temp_map_data;

static void
grp_walk_map(const void *nodep,
    const DW_VISIT which,
    const int depth)
{
    struct Dwarf_Group_Map_Entry_s *re = 0;

    (void)depth;
    re = *(struct Dwarf_Group_Map_Entry_s **)nodep;
    if (which == dwarf_postorder || which == dwarf_endorder) {
        return;
    }
    temp_map_data[map_reccount].group   = re->gm_group_number;
    temp_map_data[map_reccount].section = re->gm_key;
    temp_map_data[map_reccount].name = re->gm_section_name;
    map_reccount += 1;
}

/* Looks better sorted by group then sec num. */
static int
map_sort_compar(const void*l, const void*r)
{
    struct temp_map_struc_s *lv = (struct temp_map_struc_s *)l;
    struct temp_map_struc_s *rv = (struct temp_map_struc_s *)r;

    if (lv->group < rv->group) {
        return -1;
    }
    if (lv->group > rv->group) {
        return 1;
    }
    if (lv->section < rv->section) {
        return -1;
    }
    if (lv->section > rv->section) {
        return 1;
    }
    /* Should never get here! */
    return 0;

}

/*  New May 2017. Reveals the map between group numbers
    and section numbers.
    Caller must allocate the arrays with space for 'map_entry_count'
    values and this function fills in the array entries.
    Output ordered by group number and section number.
    */
int
dwarf_sec_group_map(Dwarf_Debug dbg,
    Dwarf_Unsigned   map_entry_count,
    Dwarf_Unsigned * group_numbers_array,
    Dwarf_Unsigned * sec_numbers_array,
    const char    ** sec_names_array,
    Dwarf_Error    * error)
{
    Dwarf_Unsigned i = 0;
    struct Dwarf_Group_Data_s *grp = 0;

    CHECK_DBG(dbg,error,"dwarf_sec_group_map()");
    if (temp_map_data) {
        _dwarf_error(dbg,error,DW_DLE_GROUP_INTERNAL_ERROR);
        return DW_DLV_ERROR;
    }
    map_reccount = 0;
    grp = &dbg->de_groupnumbers;
    if (map_entry_count < grp->gd_map_entry_count) {
        _dwarf_error(dbg,error,DW_DLE_GROUP_COUNT_ERROR);
        return DW_DLV_ERROR;
    }
    temp_map_data = calloc(map_entry_count,
        sizeof(struct temp_map_struc_s));
    if (!temp_map_data) {
        _dwarf_error(dbg,error,DW_DLE_GROUP_MAP_ALLOC);
        return DW_DLV_ERROR;
    }
    dwarf_twalk(grp->gd_map,grp_walk_map);
    if (map_reccount != grp->gd_map_entry_count) {
        /*  Impossible. */
        _dwarf_error(dbg,error,DW_DLE_GROUP_INTERNAL_ERROR);
        return DW_DLV_ERROR;
    }

    qsort(temp_map_data,map_reccount,sizeof(struct temp_map_struc_s),
        map_sort_compar);
    for (i =0 ; i < map_reccount; ++i) {
        sec_numbers_array[i] = temp_map_data[i].section;
        group_numbers_array[i] = temp_map_data[i].group;
        sec_names_array[i] = temp_map_data[i].name;
    }
    free(temp_map_data);
    map_reccount = 0;
    temp_map_data = 0;
    return DW_DLV_OK;
}

static const char *dwo_secnames[] = {
".debug_info.dwo",
".debug_types.dwo",
".debug_abbrev.dwo",
".debug_line.dwo",
".debug_loc.dwo",
".debug_str.dwo",
".debug_loclists.dwo",
".debug_rnglists.dwo",
".debug_str_offsets.dwo",
".debug_macro.dwo",
".debug_cu_index",
".debug_tu_index",
0 };

/*  Assumption: dwo sections are never in a COMDAT group
    (groupnumber >2)
    and by definition here are never group 1.
    Assumption: the map of COMDAT groups (not necessarily all
    sections, but at least all COMDAT) is complete. */
int
_dwarf_dwo_groupnumber_given_name(
    const char *name,
    unsigned *grpnum_out)
{
    const char **s = 0;

    for (s = dwo_secnames; *s; s++) {
        if (!strcmp(name,*s)) {
            *grpnum_out = DW_GROUPNUMBER_DWO;
            return DW_DLV_OK;
        }
    }
    return DW_DLV_NO_ENTRY;
}

static unsigned target_group = 0;
static int found_name_in_group = 0;
const char *lookfor_name = 0;

static void
grp_walk_for_name(const void *nodep,
    const DW_VISIT which,
    const int depth)
{
    struct Dwarf_Group_Map_Entry_s *re = 0;

    (void)depth;
    re = *(struct Dwarf_Group_Map_Entry_s **)nodep;
    if (which == dwarf_postorder || which == dwarf_endorder) {
        return;
    }
    if (re->gm_group_number == target_group) {
        if (!strcmp(lookfor_name,re->gm_section_name)) {
            found_name_in_group = TRUE;
        }
    }
}

/* returns TRUE or FALSE */
int
_dwarf_section_in_group_by_name(Dwarf_Debug dbg,
    const char * scn_name,
    unsigned groupnum)
{
    struct Dwarf_Group_Data_s *grp = 0;

    grp = &dbg->de_groupnumbers;
    found_name_in_group = FALSE;
    target_group = groupnum;
    lookfor_name = scn_name;
    dwarf_twalk(grp->gd_map,grp_walk_for_name);
    return found_name_in_group;
}

static void
_dwarf_grp_destroy_free_node(void*nodep)
{
    struct Dwarf_Group_Map_Entry_s * enp = nodep;
    free(enp);
    return;
}

void
_dwarf_destroy_group_map(Dwarf_Debug dbg)
{
    dwarf_tdestroy(dbg->de_groupnumbers.gd_map,
        _dwarf_grp_destroy_free_node);
    dbg->de_groupnumbers.gd_map = 0;
}
