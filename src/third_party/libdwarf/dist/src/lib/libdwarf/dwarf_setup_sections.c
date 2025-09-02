/*
  Copyright (C) 2000-2005 Silicon Graphics, Inc. All Rights Reserved.
  Portions Copyright (C) 2008-2010 Arxan Technologies, Inc. All Rights Reserved.
  Portions Copyright (C) 2009-2022 David Anderson. All Rights Reserved.
  Portions Copyright (C) 2010-2012 SN Systems Ltd. All Rights Reserved.

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
#include <string.h> /* memset() strcmp() strncmp() strlen() */
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
#include "dwarf_string.h"
#include "dwarf_secname_ck.h"
#include "dwarf_setup_sections.h"

/*  Used to add the specific information for a debug related section
    Called on each section of interest by section name.
    DWARF_MAX_DEBUG_SECTIONS must be large enough to allow
    that all sections of interest fit in the table.
    returns DW_DLV_ERROR or DW_DLV_OK.
    */
static int
add_debug_section_info(Dwarf_Debug dbg,
    /* Name as seen in object file. */
    const char *name,
    const char *standard_section_name,
    Dwarf_Unsigned obj_sec_num,
    struct Dwarf_Section_s *secdata,
    unsigned groupnum,
    /*  The have_dwarf flag is a somewhat imprecise
        way to determine if there is at least one 'meaningful'
        DWARF information section present in the object file.
        If not set on some section we claim (later) that there
        is no DWARF info present. see 'foundDwarf' in this file */
    int duperr,int emptyerr,int have_dwarf,
    int havezdebug,
    int *err)
{
    unsigned total_entries = dbg->de_debug_sections_total_entries;
    if (secdata->dss_is_in_use) {
        *err = duperr;
        return DW_DLV_ERROR;
    }
    if (total_entries < DWARF_MAX_DEBUG_SECTIONS) {
        struct Dwarf_dbg_sect_s *debug_section =
            &dbg->de_debug_sections[total_entries];
        secdata->dss_is_in_use = TRUE;
        debug_section->ds_name = name;
        debug_section->ds_number = obj_sec_num;
        debug_section->ds_secdata = secdata;
        debug_section->ds_groupnumber =  groupnum;
        secdata->dss_name = name; /* Actual name from object file. */
        secdata->dss_standard_name = standard_section_name;
        secdata->dss_number = obj_sec_num;
        secdata->dss_zdebug_requires_decompress =
            (Dwarf_Small)havezdebug;
        secdata->dss_computed_mmap_offset = 0;
        secdata->dss_computed_mmap_len = 0;
        secdata->dss_mmap_realarea = 0;
        secdata->dss_was_alloc = FALSE;
        /*  Just gets current global pref */
#ifndef TESTING
        secdata->dss_load_preference = dwarf_set_load_preference(0);
#endif /* TESTING*/
        /* We don't yet know about SHF_COMPRESSED */
        debug_section->ds_duperr = duperr;
        debug_section->ds_emptyerr = emptyerr;
        debug_section->ds_have_dwarf = have_dwarf;
        debug_section->ds_have_zdebug = havezdebug;
        ++dbg->de_debug_sections_total_entries;
        return DW_DLV_OK;
    }
    /*  This represents a bug in libdwarf.
        Mis-setup-DWARF_MAX_DEBUG_SECTIONS.
        Or possibly a use of section groups that is
        not supported.  */
    *err = DW_DLE_TOO_MANY_DEBUG;
    return DW_DLV_ERROR;
}

/*  Avoid adding offest to null s2.
    This function avoids a compiler warning:
    error: 'strcmp' reading 1 or more bytes
    from a region of size 0
    Offset is a fixed small positive number. */
static int
both_strings_nonempty(const char *s1, const char *s2, int offset)
{
    const char *s3 = 0;
    if (!s1 || !s2) {
        return FALSE;
    }
    s3 = s2 + offset;
    if (!s1[0] || !s3[0]) {
        return FALSE;
    }
    return TRUE;
}

/*  Return DW_DLV_OK etc.
    PRECONDITION: secname and targname are non-null
        pointers to strings. */
static int
set_up_section(Dwarf_Debug dbg,
    /*  Section name from object format.
        Might start with .zdebug not .debug if compressed section. */
    const char *secname,
    /*  Standard section name, such as .debug_info */
    const char *sec_standard_name,
    /*  Section number from object format  */
    Dwarf_Unsigned obj_sec_num,
    /*  The name associated with this secdata in libdwarf */
    const char *targname,
    /*  DW_GROUPNUMBER_ANY or BASE or DWO or some other group num */
    unsigned  groupnum_of_sec,
    struct Dwarf_Section_s *secdata,
    int duperr,int emptyerr,int have_dwarf,
    int *err)
{
    /*  Here accommodate the .debug or .zdebug version, (and of
        course non- .debug too, but those never zlib) .
        SECNAMEMAX should be a little bigger than any section
        name we care about as possibly compressed, which
        is to say bigger than any standard section name. */
#define SECNAMEMAX 30
    size_t secnamelen = strlen(secname);
    /* static const char *dprefix = ".debug_"; */
#define DPREFIXLEN 7
    static const char *zprefix = ".zdebug_";
#define ZPREFIXLEN 8
    int havezdebug = FALSE;
    int namesmatch = FALSE;
    const char *postzprefix = 0;

    /*  For example, if the secname is .zdebug_info
        we update the finaltargname to .debug_info
        to match with the particular (known, predefined)
        object section name.
        We add one character, so check
        to see if it will, in the end, fit.
        See the SET_UP_SECTION macro.  */

    if (secnamelen >= SECNAMEMAX) {
        /*  This is not the target section.
            our caller will keep looking. */
        return DW_DLV_NO_ENTRY;
    }
    havezdebug = !strncmp(secname,zprefix,ZPREFIXLEN);
    if (havezdebug) {
        postzprefix = secname+ZPREFIXLEN;
    }
    /*  With Alpine gcc
        12.2.1_git20220924-r4) 12.2.1 20220924
        and some other gcc versions when compiling
        with -Werror and -fsanitize:
        we get
        error: 'strcmp' reading 1 or more bytes
        from a region of size 0 [-Werror=stringop-overread]
        So we add -Wnostringop-overread to the build as the error is
        a false positive. We had to drop stringop-overread
        references in compiler options, such turned off
        valuable warnings. Oct 2024
        refined the test to notice empty string */
    if (both_strings_nonempty(postzprefix,targname,DPREFIXLEN) &&
        !strcmp(postzprefix,targname+DPREFIXLEN)) {
            /*  zprefix version matches the object section
                name so the section is compressed and is
                the section this targname applies to. */
        namesmatch = TRUE;
    } else if (!strcmp(secname,targname)) {
        namesmatch = TRUE;
    } else { /*  Fall to next statement */ }
#undef ZPREFIXLEN
#undef DPREFIXLEN
#undef SECNAMEMAX
    if (!namesmatch) {
        /*  This is not the target section.
            our caller will keep looking. */
            return DW_DLV_NO_ENTRY;
    }

    /* SETUP_SECTION. See also BUILDING_SECTIONS, BUILDING_MAP  */
    {
        /*  The section name is a match with targname, or
            the .zdebug version of targname. */
        int sectionerr = 0;

        sectionerr = add_debug_section_info(dbg,secname,
            sec_standard_name,
            obj_sec_num,
            secdata,
            groupnum_of_sec,
            duperr,emptyerr, have_dwarf,
            havezdebug,err);
        if (sectionerr != DW_DLV_OK) {
            /* *err is set already */
            return sectionerr;
        }
    }
    return DW_DLV_OK;
}

#define SET_UP_SECTION(mdbg,mname,mtarg,mgrp,minfo,me1,me2,mdw,mer) \
    {                                           \
    int lerr = 0;                               \
    lerr =  set_up_section((mdbg),              \
        (mname),  /* actual section name */     \
        (mtarg),    /* std section name */      \
        /* scn_number from macro use context */ \
        scn_number,(mtarg),(mgrp),              \
        (minfo),                                \
        (me1),(me2),(mdw),(mer));               \
    if (lerr != DW_DLV_NO_ENTRY) {              \
        return lerr;                            \
    }    /* else fall through. */               \
    }

/*  If running this long set of tests is slow
    enough to matter one could set up a local
    tsearch tree with all this content and search
    it instead of this set of sequential tests.
    Or use a switch(){} here with a search tree
    to to turn name into index for the switch(). */
int
_dwarf_enter_section_in_de_debug_sections_array(Dwarf_Debug dbg,
    const char *scn_name,
    /* This is the number of the section in the object file. */
    Dwarf_Unsigned scn_number,
    unsigned group_number,
    int *err)
{
    /*  Setup the table that contains the basic information about the
        sections that are DWARF related. The entries are very unlikely
        to change very often. */
    SET_UP_SECTION(dbg,scn_name,".debug_info",
        group_number,
        &dbg->de_debug_info,
        DW_DLE_DEBUG_INFO_DUPLICATE,DW_DLE_DEBUG_INFO_NULL,
        TRUE,err);
    SET_UP_SECTION(dbg,scn_name,".debug_info.dwo",
        DW_GROUPNUMBER_DWO,
        &dbg->de_debug_info,
        DW_DLE_DEBUG_INFO_DUPLICATE,DW_DLE_DEBUG_INFO_NULL,
        TRUE,err);
    SET_UP_SECTION(dbg,scn_name,".debug_types",
        group_number,
        &dbg->de_debug_types,
        DW_DLE_DEBUG_TYPES_DUPLICATE,DW_DLE_DEBUG_TYPES_NULL,
        TRUE,err);
    /* types.dwo  is non-standard. DWARF4 GNU maybe. */
    SET_UP_SECTION(dbg,scn_name,".debug_types.dwo",
        DW_GROUPNUMBER_DWO,
        &dbg->de_debug_types,
        DW_DLE_DEBUG_TYPES_DUPLICATE,DW_DLE_DEBUG_TYPES_NULL,
        TRUE,err);
    SET_UP_SECTION(dbg,scn_name,".debug_abbrev",
        group_number,
        &dbg->de_debug_abbrev, /*03*/
        DW_DLE_DEBUG_ABBREV_DUPLICATE,DW_DLE_DEBUG_ABBREV_NULL,
        TRUE,err);
    SET_UP_SECTION(dbg,scn_name,".debug_abbrev.dwo",
        DW_GROUPNUMBER_DWO,
        &dbg->de_debug_abbrev, /*03*/
        DW_DLE_DEBUG_ABBREV_DUPLICATE,DW_DLE_DEBUG_ABBREV_NULL,
        TRUE,err);
    SET_UP_SECTION(dbg,scn_name,".debug_aranges",
        group_number,
        &dbg->de_debug_aranges,
        DW_DLE_DEBUG_ARANGES_DUPLICATE,0,
        FALSE,err);
    SET_UP_SECTION(dbg,scn_name,".debug_line",
        group_number,
        &dbg->de_debug_line,
        DW_DLE_DEBUG_LINE_DUPLICATE,0,
        TRUE,err);
    /* DWARF5 */
    SET_UP_SECTION(dbg,scn_name,".debug_line_str",
        group_number,
        &dbg->de_debug_line_str,
        DW_DLE_DEBUG_LINE_DUPLICATE,0,
        FALSE,err);
    SET_UP_SECTION(dbg,scn_name,".debug_line.dwo",
        DW_GROUPNUMBER_DWO,
        &dbg->de_debug_line,
        DW_DLE_DEBUG_LINE_DUPLICATE,0,
        TRUE,err);
    SET_UP_SECTION(dbg,scn_name,".debug_frame",
        group_number,
        &dbg->de_debug_frame,
        DW_DLE_DEBUG_FRAME_DUPLICATE,0,
        TRUE,err);
    /* gnu egcs-1.1.2 data */
    SET_UP_SECTION(dbg,scn_name,".eh_frame",
        group_number,
        &dbg->de_debug_frame_eh_gnu,
        DW_DLE_DEBUG_FRAME_DUPLICATE,0,
        TRUE,err);
    SET_UP_SECTION(dbg,scn_name,".debug_loc",
        group_number,
        &dbg->de_debug_loc,
        DW_DLE_DEBUG_LOC_DUPLICATE,0,
        FALSE,err);
    /*  .debug_loc.dwo would be non-standard. */
    SET_UP_SECTION(dbg,scn_name,".debug_loc.dwo",
        DW_GROUPNUMBER_DWO,
        &dbg->de_debug_loc,
        DW_DLE_DEBUG_LOC_DUPLICATE,0,
        FALSE,err);
    SET_UP_SECTION(dbg,scn_name,".debug_pubnames",
        group_number,
        &dbg->de_debug_pubnames,
        DW_DLE_DEBUG_PUBNAMES_DUPLICATE,0,
        FALSE,err);
    SET_UP_SECTION(dbg,scn_name,".debug_str",
        group_number,
        &dbg->de_debug_str,
        DW_DLE_DEBUG_STR_DUPLICATE,0,
        FALSE,err);
    SET_UP_SECTION(dbg,scn_name,".debug_str.dwo",
        DW_GROUPNUMBER_DWO,
        &dbg->de_debug_str,
        DW_DLE_DEBUG_STR_DUPLICATE,0,
        FALSE,err);
    /* Section new in DWARF3.  */
    SET_UP_SECTION(dbg,scn_name,".debug_pubtypes",
        group_number,
        &dbg->de_debug_pubtypes,
        /*13*/ DW_DLE_DEBUG_PUBTYPES_DUPLICATE,0,
        FALSE,err);
    /* DWARF5 */
    SET_UP_SECTION(dbg,scn_name,".debug_loclists",
        group_number,
        &dbg->de_debug_loclists,
        /*13*/ DW_DLE_DEBUG_LOClISTS_DUPLICATE,0,
        FALSE,err);
    /* DWARF5 */
    SET_UP_SECTION(dbg,scn_name,".debug_loclists.dwo",
        DW_GROUPNUMBER_DWO,
        &dbg->de_debug_loclists,
        /*13*/ DW_DLE_DEBUG_LOClISTS_DUPLICATE,0,
        FALSE,err);
    /* DWARF5 */
    SET_UP_SECTION(dbg,scn_name,".debug_rnglists",
        group_number,
        &dbg->de_debug_rnglists,
        /*13*/ DW_DLE_DEBUG_RNGLISTS_DUPLICATE,0,
        FALSE,err);
    /* DWARF5 */
    SET_UP_SECTION(dbg,scn_name,".debug_rnglists.dwo",
        DW_GROUPNUMBER_DWO,
        &dbg->de_debug_rnglists,
        /*13*/ DW_DLE_DEBUG_RNGLISTS_DUPLICATE,0,
        FALSE,err);
    /* DWARF5 */
    SET_UP_SECTION(dbg,scn_name,".debug_str_offsets",
        group_number,
        &dbg->de_debug_str_offsets,
        DW_DLE_DEBUG_STR_OFFSETS_DUPLICATE,0,
        FALSE,err);
    /* DWARF5 */
    SET_UP_SECTION(dbg,scn_name,".debug_str_offsets.dwo",
        DW_GROUPNUMBER_DWO,
        &dbg->de_debug_str_offsets,
        DW_DLE_DEBUG_STR_OFFSETS_DUPLICATE,0,
        FALSE,err);

    /* SGI IRIX-only. */
    SET_UP_SECTION(dbg,scn_name,".debug_funcnames",
        group_number,
        &dbg->de_debug_funcnames,
        /*11*/ DW_DLE_DEBUG_FUNCNAMES_DUPLICATE,0,
        FALSE,err);
    /*  SGI IRIX-only, created years before DWARF3. Content
        essentially identical to .debug_pubtypes.  */
    SET_UP_SECTION(dbg,scn_name,".debug_typenames",
        group_number,
        &dbg->de_debug_typenames,
        /*12*/ DW_DLE_DEBUG_TYPENAMES_DUPLICATE,0,
        FALSE,err);
    /* SGI IRIX-only.  */
    SET_UP_SECTION(dbg,scn_name,".debug_varnames",
        group_number,
        &dbg->de_debug_varnames,
        DW_DLE_DEBUG_VARNAMES_DUPLICATE,0,
        FALSE,err);
    /* SGI IRIX-only. */
    SET_UP_SECTION(dbg,scn_name,".debug_weaknames",
        group_number,
        &dbg->de_debug_weaknames,
        DW_DLE_DEBUG_WEAKNAMES_DUPLICATE,0,
        FALSE,err);

    SET_UP_SECTION(dbg,scn_name,".debug_macinfo",
        group_number,
        &dbg->de_debug_macinfo,
        DW_DLE_DEBUG_MACINFO_DUPLICATE,0,
        TRUE,err);
    /*  ".debug_macinfo.dwo" is not allowed.  */

    /* DWARF5 */
    SET_UP_SECTION(dbg,scn_name,".debug_macro",
        group_number,
        &dbg->de_debug_macro,
        DW_DLE_DEBUG_MACRO_DUPLICATE,0,
        TRUE,err);
    /* DWARF5 */
    SET_UP_SECTION(dbg,scn_name,".debug_macro.dwo",
        DW_GROUPNUMBER_DWO,
        &dbg->de_debug_macro,
        DW_DLE_DEBUG_MACRO_DUPLICATE,0,
        TRUE,err);
    SET_UP_SECTION(dbg,scn_name,".debug_ranges",
        group_number,
        &dbg->de_debug_ranges,
        DW_DLE_DEBUG_RANGES_DUPLICATE,0,
        TRUE,err);
    /*  No .debug_ranges.dwo allowed. */

    /* New DWARF5 */
    SET_UP_SECTION(dbg,scn_name,".debug_sup",
        group_number,
        &dbg->de_debug_sup,
        DW_DLE_DEBUG_SUP_DUPLICATE,0,
        TRUE,err);
    /* No .debug_sup.dwo allowed. */

    /*  .symtab and .strtab have to be in any group.  */
    SET_UP_SECTION(dbg,scn_name,".symtab",
        group_number,
        &dbg->de_elf_symtab,
        DW_DLE_DEBUG_SYMTAB_ERR,0,
        FALSE,err);
    SET_UP_SECTION(dbg,scn_name,".strtab",
        group_number,
        &dbg->de_elf_strtab,
        DW_DLE_DEBUG_STRTAB_ERR,0,
        FALSE,err);

    /* New DWARF5 */
    SET_UP_SECTION(dbg,scn_name,".debug_addr",
        group_number,
        &dbg->de_debug_addr,
        DW_DLE_DEBUG_ADDR_DUPLICATE,0,
        TRUE,err);
    /*  No .debug_addr.dwo allowed.  */

    /* gdb added this. */
    SET_UP_SECTION(dbg,scn_name,".gdb_index",
        group_number,
        &dbg->de_debug_gdbindex,
        DW_DLE_DUPLICATE_GDB_INDEX,0,
        FALSE,err);

    /* New DWARF5 */
    SET_UP_SECTION(dbg,scn_name,".debug_names",
        group_number,
        &dbg->de_debug_names,
        /*13*/ DW_DLE_DEBUG_NAMES_DUPLICATE,0,
        FALSE,err);
    /* No .debug_names.dwo allowed. */

    /* gdb added this in DW4. It is in standard DWARF5  */
    SET_UP_SECTION(dbg,scn_name,".debug_cu_index",
        DW_GROUPNUMBER_DWO,
        &dbg->de_debug_cu_index,
        DW_DLE_DUPLICATE_CU_INDEX,0,
        FALSE,err);
    /* gdb added this in DW4. It is in standard DWARF5 */
    SET_UP_SECTION(dbg,scn_name,".debug_tu_index",
        DW_GROUPNUMBER_DWO,
        &dbg->de_debug_tu_index,
        DW_DLE_DUPLICATE_TU_INDEX,0,
        FALSE,err);

    /*  GNU added this. It is not part of DWARF, but we will
        consider it is so that debuglink can work.
        Force have_dwarf TRUE github issue 297 */
    SET_UP_SECTION(dbg,scn_name,".gnu_debuglink",
        DW_GROUPNUMBER_DWO,
        &dbg->de_gnu_debuglink,
        DW_DLE_DUPLICATE_GNU_DEBUGLINK,0,
        TRUE,err);

    /*  GNU added this. It is not part of DWARF, but we will
        consider it is so that debuglink can work,
        Force have_dwarf TRUE github issue 297 */
    SET_UP_SECTION(dbg,scn_name,".note.gnu.build-id",
        DW_GROUPNUMBER_DWO,
        &dbg->de_note_gnu_buildid,
        DW_DLE_DUPLICATE_NOTE_GNU_BUILD_ID,0,
        TRUE,err);

    /* GNU added this. It is not part of DWARF */
    SET_UP_SECTION(dbg,scn_name,".debug_gnu_pubtypes.dwo",
        DW_GROUPNUMBER_DWO,
        &dbg->de_debug_gnu_pubtypes,
        DW_DLE_DUPLICATE_GNU_DEBUG_PUBTYPES,0,
        FALSE,err);
    SET_UP_SECTION(dbg,scn_name,".debug_gnu_pubtypes",
        group_number,
        &dbg->de_debug_gnu_pubtypes,
        DW_DLE_DUPLICATE_GNU_DEBUG_PUBTYPES,0,
        FALSE,err);
    SET_UP_SECTION(dbg,scn_name,".debug_gnu_pubnames.dwo",
        DW_GROUPNUMBER_DWO,
        &dbg->de_debug_gnu_pubnames,
        DW_DLE_DUPLICATE_GNU_DEBUG_PUBNAMES,0,
        FALSE,err);
    SET_UP_SECTION(dbg,scn_name,".debug_gnu_pubnames",
        group_number,
        &dbg->de_debug_gnu_pubnames,
        DW_DLE_DUPLICATE_GNU_DEBUG_PUBNAMES,0,
        FALSE,err);
    return DW_DLV_NO_ENTRY;
}
