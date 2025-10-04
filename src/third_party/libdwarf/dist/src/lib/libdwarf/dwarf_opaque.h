/*
  Copyright (C) 2000-2005 Silicon Graphics, Inc.  All Rights Reserved.
  Portions Copyright (C) 2007-2024 David Anderson. All Rights Reserved.
  Portions Copyright (C) 2008-2010 Arxan Technologies, Inc. All Rights Reserved.

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
/*  Following the nomenclature of the DWARF standard
    Section Version Numbers (DWARF 5):
    * means not applicable.
    - means not defined in that version.
    The version numbers for .debug_info is the same as .debug_info.dwo
    (etc for the other dwo sections).
    The versions applicable by section are:
    .                  DWARF2 DWARF3 DWARF4 DWARF5
    .debug_abbrev           *      *      *      *
    .debug_addr             -      -      -      5
    .debug_aranges          2      2      2      2
    .debug_frame            1      3      4      4
    .debug_info             2      3      4      5
    .debug_line             2      3      4      5
    .debug_line_str         -      -      -      5
    .debug_loc              *      *      *      -
    .debug_loclists         -      -      -      5
    .debug_macinfo          *      *      *      -
    .debug_macro            -      -      -      5
    .debug_names            -      -      -      5
    .debug_pubnames         2      2      2      -
    .debug_pubtypes         -      2      2      -
    .debug_ranges           -      *      *      -
    .debug_rnglists         -      -      -      5
    .debug_str              *      *      *      *
    .debug_str_offsets      -      -      -      5
    .debug_sup              -      -      -      5
    .debug_types            -      -      4      -

    .debug_abbrev.dwo       -      -      -      *
    .debug_info.dwo         -      -      -      5
    .debug_line.dwo         -      -      -      5
    .debug_loc.dwo          -      -      -      -
    .debug_loclists.dwo     -      -      -      5
    .debug_macro.dwo        -      -      -      5
    .debug_rnglists.dwo     -      -      -      5
    .debug_str.dwo          -      -      -      *
    .debug_str_offsets.dwo  -      -      -      5

    .debug_cu_index         -      -      -      5
    .debug_tu_index         -      -      -      5

*/
#define DBG_IS_SECONDARY(p) ((p) && (p)->de_secondary_dbg &&   \
    ((p)->de_secondary_dbg == (p)))
#define DBG_IS_PRIMARY(p) ((p) && ((!(p)->de_secondary_dbg) ||  \
    ((p)->de_secondary_dbg && ((p)->de_secondary_dbg != (p)))))
#define DBG_HAS_SECONDARY(p) (DBG_IS_PRIMARY(p) && \
    (DBG_IS_SECONDARY((p)->de_secondary_dbg)))

/*  For debugging. */
const char * _dwarf_basename(const char *full);

#undef  DEBUG_PRIMARY_DBG
#ifdef DEBUG_PRIMARY_DBG
void
_dwarf_print_is_primary(const char *msg,Dwarf_Debug p,int line,
    const char *filepath);
void
_dwarf_dump_prim_sec(const char *msg,Dwarf_Debug p, int line,
    const char *filepath);
void
_dwarf_dump_optional_fields(const char *msg,
    Dwarf_CU_Context context,
    int line,
    const char *filepath);
#endif /* DEBUG_PRIMARY_DBG */

struct Dwarf_Rnglists_Context_s;
typedef struct Dwarf_Rnglists_Context_s *Dwarf_Rnglists_Context;
struct Dwarf_Loclists_Context_s;
typedef struct Dwarf_Loclists_Context_s *Dwarf_Loclists_Context;

struct Dwarf_Die_s {
    Dwarf_Byte_Ptr    di_debug_ptr;
    Dwarf_Abbrev_List di_abbrev_list;
    Dwarf_CU_Context  di_cu_context;
    /*  Abbrev codes are expected to be smallish numbers,
        but the Standard does not require smallish numbers. */
    Dwarf_Unsigned    di_abbrev_code;
    /* TRUE if part of debug_info. FALSE if part of .debug_types. */
    Dwarf_Bool di_is_info;
};

struct Dwarf_Attribute_s {
    Dwarf_Half ar_attribute;             /* Attribute Value. */
    Dwarf_Half ar_attribute_form;        /* Attribute Form. */
    Dwarf_Half ar_attribute_form_direct;
        /*  *form_direct Identical to ar_attribute_form
            except that if
            the original form uleb was DW_FORM_indirect,
            ar_attribute_form_direct contains DW_FORM_indirect
            but ar_attribute_form contains the true form. */

    Dwarf_CU_Context ar_cu_context;
        /*  The following points to either debug_info or debug_types
            depending on if context is cc_is_info  or not. */
    Dwarf_Small *ar_debug_ptr;
        /*  If DW_FORM_implicit const, the value is here, not
            in the DIE. */
    Dwarf_Signed ar_implicit_const;
    Dwarf_Debug  ar_dbg; /* dbg owning the attr */

    Dwarf_Die ar_die;/* Access to the DIE owning the attribute */
    Dwarf_Attribute ar_next;
};

#define CC_PROD_METROWERKS 1
#define CC_PROD_Apple      2 /* Apple clang */

/*
    This structure provides the context for a compilation unit.
    Thus, it contains the Dwarf_Debug, cc_dbg, that this cu
    belongs to.  It contains the information
    in the compilation unit header, cc_length,
    cc_version_stamp, cc_abbrev_offset,
    and cc_address_size, in the .debug_info section for that cu.
    In addition, it contains the count, cc_count_cu, of the cu
    number of that cu in the list of cu's in the .debug_info.
    The count starts at 1, ie cc_count_cu is 1 for the first cu,
    2 for the second and so on.  This struct also contains a
    pointer, to a list of pairs of abbrev code
    and a pointer to the start of that abbrev
    in the .debug_abbrev section.

    Each die will also contain a pointer to such a struct to
    record the context for that die.

    Notice that a pointer to the CU DIE itself is
    Dwarf_Off off2 = cu_context->cc_debug_info_offset;
    cu_die_info_ptr = dbg->de_debug_info.dss_data +
        off2 + _dwarf_length_of_cu_header(dbg, off2);
    Or similar for de_debug_types.

    **Updated by dwarf_next_cu_header in dwarf_die_deliv.c
*/
struct Dwarf_CU_Context_s {
    Dwarf_Debug cc_dbg;
    /*  The sum of cc_length, cc_length_size, and cc_extension_size
        is the total length of the CU including its header.
        cc_length is the length of the compilation unit excluding
        cc_length_size and cc_extension_size.  */
    Dwarf_Unsigned cc_length;

    /*  cc_length_size is the size in bytes of an offset.
        Should probably be renamed cc_offset_size.
        4 for 32bit dwarf, 8 for 64bit dwarf (whether MIPS/IRIX
        64bit dwarf or standard 64bit dwarf using the extension
        mechanism). */
    Dwarf_Small cc_length_size;

    /*  cc_extension_size is zero unless this is standard
        DWARF3 and later 64bit dwarf using the extension mechanism.
        64bit DWARF3 and later: cc_extension_size is 4.
        64bit DWARF2 MIPS/IRIX: cc_extension_size is zero.
        32bit DWARF:            cc_extension_size is zero.  */
    Dwarf_Small cc_extension_size;

    /*  cc_version_stamp is the DWARF version number applicable
        to the  DWARF in this compilation unit. 2,3,4,... */
    Dwarf_Half cc_version_stamp;
    /*  cc_abbrev_offset is the section-global offset
        of the .debug_abbrev section this CU uses.
        Data from CU header. Includes DWP adjustment made
        as soon as we create a cu_context. */
    Dwarf_Unsigned cc_abbrev_offset;

    /*  cc_address_size is the size of an address in this
        compilation unit. */
    Dwarf_Small cc_address_size;
    Dwarf_Small cc_segment_selector_size;
    /*  Normally zero, For the defective Metrowerks
        compiler is set to CC_PROD_METROW */
    Dwarf_Small cc_producer;

    /*  cc_debug_offset is the global offset in the section
        of the area length field of the CU.
        The CU header of the CU is at offset
        cc_debug_offset+cc_length_size+cc_extension_size;
        This is a section global offset.
        May be debug_info or debug_types.
        Even in DWP this is set to true global offset
        right away when cu_context created.
        See cc_is_info flag. */
    Dwarf_Unsigned cc_debug_offset;

    /* === START DEBUG FISSION (Split Dwarf) data
        cc_signature is in the TU header
        of a type unit of a TU DIE (or for DW5 in the
        skeleton or split_compile header is a dwo_id).
        Ignore this field if cc_signature_present is zero.
        (TU CUs signature is not the same namespace
        as DW_AT_dwo_id signatures. The two must be
        kept separate (for DWARF5))

        If cc_unit_type == DW_UT_compile or DW_UT_partial
            the signature is a CU signature (dwo_id).
        Some early DW5 drafts encouraged DWARF4 output
            of some compilers to include dwo_id, but
            in a messier way(lacking DW_UT_*).
        If cc_unit_type ==  DW_UT_split_type
            the signature is a type signature. */
    Dwarf_Half  cc_cu_die_tag;

    Dwarf_Sig8  cc_signature;

    /*  cc_type_signature_offset contains the
        section-local DIE offset of the type
        the signature applies to if the cc_unit_type
        is DW_UT_type or DW_UT_split_type. */
    Dwarf_Unsigned cc_signature_offset;

    /*  For each CU and each TU
        in a dwp package file there is
        is a hash and
        a set of offsets indexed by DW_SECT_* id.
        Only one such set per CU or TU.
        The data on all that is in cc_dwp_offsets

        If it is a TU the signature in cc_dwp_offsets
        must match the signature in cc_signature.
        */
    struct Dwarf_Debug_Fission_Per_CU_s  cc_dwp_offsets;

    Dwarf_Bool cc_signature_present; /* Meaning type signature
        in TU header or, for CU header, signature in CU DIE. */

    /*  cc_low_pc[_present] is applied as base address of
        of rnglists and loclists when reading an rle_head,
        compied into cc_cu_base_address. Comes from
        CU_DIE, not rnglists or loclists */
    Dwarf_Bool cc_low_pc_present;

    /*  From CU_DIE. Copied from cc_low_pc_present.
        Used as default base address for rnglists, loclists.
        in a DWARF5 dwo, inherited from skeleton (tieddbg). */
    Dwarf_Bool cc_base_address_present;

    Dwarf_Bool cc_cu_die_has_children;
    Dwarf_Bool cc_dwo_name_present;
    Dwarf_Bool cc_at_strx_present;

    /*  Non zero if this context is a dwo section. Either
        dwo or dwp file. */
    Dwarf_Bool cc_is_dwo;

    /*  cc_cu_die_offset_present is non-zero if
        cc_cu_die_global_sec_offset is meaningful.  */
    Dwarf_Bool     cc_cu_die_offset_present;
    Dwarf_Bool     cc_at_ranges_offset_present;
    /*  About: DW_AT_addr_base in CU DIE,
        offset to .debug_addr table */
    Dwarf_Bool     cc_addr_base_offset_present;

    /*  If present, is base address of CU.  In DWARF2
        nothing says what attribute is the base address.
        DW_AT_producer 4.2.1 (Based on Apple Inc. build 5658)
        (LLVM build 2336.1.00) uses DW_AT_entry_pc as the
        base address.  DW_AT_entry_pc first appears
        in DWARF3.
        We allow  DW_AT_entry_pc as an extension,
        as a 'low_pc' if there is DW_AT_entry_pc with
        no DW_AT_low_pc. 19 May 2022.
        In DWARF3, DWARF4 DW_AT_low_pc is specifically
        mentioned as the base address.  */
    Dwarf_Unsigned cc_low_pc;

    /*  from DW_AT_addr_base in CU DIE, offset to .debug_addr table */
    Dwarf_Unsigned cc_addr_base_offset;  /* Zero in .dwo */

    /*  From cc_low_pc, used as initial base_address
        in processing loclists and rnglists  */
    Dwarf_Unsigned  cc_base_address;

    /*  DW_SECT_LINE */
    Dwarf_Bool     cc_line_base_present;     /*DW5 */
    Dwarf_Unsigned cc_line_base;             /*DW5 */
    Dwarf_Unsigned cc_line_base_contr_size;  /*DW5 */

    /*  From DW_AT_loclists_base or
        computed from DW_SECT_LOCLISTS */
    Dwarf_Unsigned cc_loclists_base;
    Dwarf_Unsigned cc_loclists_base_contr_size;
    Dwarf_Bool     cc_loclists_base_present;
    /*  via_at means there was a DW_AT_loclists_base */
    Dwarf_Bool     cc_loclists_base_via_at;
    Dwarf_Bool     cc_loclists_header_length_present;

    /* ======= str_offsets table data  =======*/
    /*  header_offset is global offset in str_offsets section
        of an array of string offsets. Not a header offset
        at all.
        from DW_AT_str_offsets_base DW5 page 66 item 13.
        Not related to the Name Table */
    /*  Set from DW_AT_str_offsets_base. Global offset. */
    Dwarf_Bool     cc_str_offsets_array_offset_present;
    Dwarf_Unsigned cc_str_offsets_array_offset;

    /*  Set from str_offsets header, means all the relevant
        data from the header is present. Does not mean
        the data from DW_AT_str_offsets_base is present */
    Dwarf_Bool     cc_str_offsets_tab_present;
    /*  Set from str_offsets header. */
    Dwarf_Unsigned cc_str_offsets_header_offset;

    /*  Set by reading str_offsets header. Local offset
        from header to its array. */
    Dwarf_Unsigned cc_str_offsets_tab_to_array;

    /*  The following three set but not used. Might
        be useful for error checking. */
    Dwarf_Unsigned cc_str_offsets_offset_size;
    /*  The size of the table from table header
        to end of this table. Not the size of array
        in the table */
    Dwarf_Unsigned cc_str_offsets_table_size;
    Dwarf_Half     cc_str_offsets_version;
    /* ======= end str_offsets table data  =======*/

    /*  DW_SECT_MACRO */
    Dwarf_Unsigned cc_macro_base;    /*DW5 */
    Dwarf_Unsigned cc_macro_base_contr_size;    /*DW5 */
    Dwarf_Bool     cc_macro_base_present;
    Dwarf_Bool     cc_macro_header_length_present;

    /*  For quick access to .debug_ranges from a CU DIE. */
    Dwarf_Unsigned cc_at_ranges_offset;
    /*  DW_SECT_RNGLISTS  */
    /*  DW_AT_GNU_ranges_base was a GNU extension that appeared
        but was unused. See dwarf_die_deliv.c for details. */
    Dwarf_Unsigned cc_ranges_base;
    /*  DW_AT_GNU_ranges_base is a GNU extension, DW4  */
    Dwarf_Bool     cc_ranges_base_present;
    /* .debug_rnglists */
    Dwarf_Unsigned cc_rnglists_base;    /*DW5 */
    Dwarf_Unsigned cc_rnglists_base_contr_size;    /*DW5 */
    Dwarf_Bool     cc_rnglists_base_present; /* DW5 */
    /*  via_at means there was a DW_AT_rnglists_base */
    Dwarf_Bool     cc_rnglists_base_via_at;
    Dwarf_Bool     cc_rnglists_header_length_present;

    char *         cc_dwo_name;
    /* === END DEBUG FISSION (Split Dwarf) data */

    /*  Global section offset to the bytes of the CU die for this CU.
        Set when the CU die is accessed by dwarf_siblingof_b(). */
    Dwarf_Unsigned cc_cu_die_global_sec_offset;

    Dwarf_Byte_Ptr   cc_last_abbrev_ptr;
    Dwarf_Byte_Ptr   cc_last_abbrev_endptr;
    Dwarf_Hash_Table cc_abbrev_hash_table;
    Dwarf_Unsigned   cc_highest_known_code;
    Dwarf_CU_Context cc_next;

    Dwarf_Bool cc_is_info;    /* TRUE means context is
        in debug_info, FALSE means is in debug_types.
        FALSE only possible for DWARF4 .debug_types
        section CUs.
        For DWARF5 all DIEs are in .debug_info[.dwo] */

    Dwarf_Half cc_unit_type; /* DWARF5
        Set from header as a DW_UT_ value.
        For DWARF 2,3,4 this is filled in initially
        from the CU header and refined by inspecting
        the CU DIE to detect the correct setting. */

};

/*  Consolidates section-specific data in one place.
    Section is an Elf specific term, intended as a general
    term (for non-Elf objects some code must synthesize the
    values somehow).  */
struct Dwarf_Section_s {
    Dwarf_Small *  dss_data;
    Dwarf_Unsigned dss_size;
    /*  Some Elf sections have a non-zero dss_entrysize which
        is the size in bytes of a table entry in the section.
        Relocations and symbols are both in tables, so have a
        non-zero entrysize.  Object formats which do not care
        about this should leave this field zero. */
    Dwarf_Unsigned dss_entrysize;
    /*  dss_index is the section index as things are numbered in
        an object file being read.   An Elf section number. */
    Dwarf_Unsigned     dss_index;
    /*  dss_addr is the 'section address' which is only
        non-zero for a GNU eh section.
        Purpose: to handle DW_EH_PE_pcrel encoding. Leaving
        it zero is fine for non-elf.  */
    Dwarf_Addr     dss_addr;

    Dwarf_Unsigned dss_computed_mmap_offset;
    Dwarf_Unsigned dss_computed_mmap_len;
    Dwarf_Small *  dss_mmap_realarea;
    /*  Value is Dwarf_Alloc_Malloc=1 or Dwarf_Alloc_Mmap=2 */
    enum Dwarf_Sec_Alloc_Pref  dss_load_preference;
    /*  Any valid Dwarf_Alloc_*, tells if free() or
        equivalent required for dss_data. */
    enum Dwarf_Sec_Alloc_Pref  dss_actual_load_type;

    /*  is_in_use set during initial object reading to
        detect duplicates. Ignored after setup done. */
    Dwarf_Small    dss_is_in_use;

    /*  When loading COMDAT they refer (sometimes) to
        base sections, so we need to have the BASE
        group sections filled in when the corresponding is
        not in the COMDAT group list.  .debug_abbrev is
        an example. */
    Dwarf_Unsigned     dss_group_number;

    /* These for reporting compression */
    Dwarf_Unsigned dss_uncompressed_length;
    Dwarf_Unsigned dss_compressed_length;

    /*  If this is zdebug, to start  data/size are the
        raw section bytes.
        Initially for all sections
            dss_was_alloc set FALSE
            dss_requires_decompress set FALSE.
        For zdebug set dss_zdebug_requires_decompress set TRUE
            In that case it is likely ZLIB compressed but
            we do not know that just scanning section headers.
        If not .zdebug but it is SHF_COMPRESSED
            then decompress is required.

        On translation (ie zlib use and malloc)
        Set dss_data dss_size to point to malloc space and
            malloc size.
        Set dss_did_decompress FALSE
        Set dss_was_alloc  TRUE */
    Dwarf_Small    dss_zdebug_requires_decompress;
    Dwarf_Small    dss_did_decompress;
    Dwarf_Small    dss_shf_compressed; /* section SHF_COMPRESS */
    Dwarf_Small    dss_was_alloc;

    /* Section compression starts with ZLIB chars*/
    Dwarf_Small dss_ZLIB_compressed;

    /*  For non-elf, leaving the following fields zero
        will mean they are ignored. */
    /*  dss_link should be zero unless a section has a link
        to another (sh_link).  Used to access relocation data for
        a section (and for symtab section, access its strtab). */
    Dwarf_Unsigned     dss_link;
    /*  The following is used when reading .rela sections
        (such sections appear in some .o files). */
    Dwarf_Unsigned     dss_reloc_index; /* Zero means ignore
        the reloc fields. */
    Dwarf_Small *  dss_reloc_data;
    Dwarf_Unsigned dss_reloc_size;
    Dwarf_Unsigned dss_reloc_entrysize;
    Dwarf_Addr     dss_reloc_addr;
    /*  dss_reloc_symtab is the sh_link of a .rela
        to its .symtab, leave
        it 0 if non-meaningful. */
    Dwarf_Addr     dss_reloc_symtab;
    /*  dss_reloc_link should be zero unless a reloc section
        has a link to another (sh_link).
        Used to access the symtab for relocating a section. */
    Dwarf_Unsigned     dss_reloc_link;
    /*  Pointer to the elf symtab, used for elf .rela. Leave it 0
        if not relevant. */
    struct Dwarf_Section_s *dss_symtab;
    /*  dss_name, dss_standard_name must never be freed,
        they are static strings in libdwarf. */
    const char * dss_name;
    const char * dss_standard_name;

    /* Object section number in object file. */
    Dwarf_Unsigned dss_number;

    /*  These are elf flags and non-elf object should
        just leave these fields zero.  */
    Dwarf_Unsigned  dss_flags;
    Dwarf_Unsigned  dss_addralign;

    /*  Set when loading .group section as those are special and
        neither compressed nor have relocations so never malloc
        space for libdwarf.  */
    Dwarf_Small     dss_ignore_reloc_group_sec;
    char dss_is_rela;
};

/*  Overview: if next_to_use== first, no error slots are used.
    If next_to_use+1 (mod maxcount) == first the slots are all used
*/
struct Dwarf_Harmless_s {
    unsigned dh_maxcount;
    unsigned dh_next_to_use;
    unsigned dh_first;
    unsigned dh_errs_count;
    char **  dh_errors;
};

/*  Data needed separately for debug_info and debug_types
    as we may be reading both interspersed.  So we always
    select the one we need. */
struct Dwarf_Debug_InfoTypes_s {
    /*  Context for the compilation_unit just read by a call to
        dwarf_next_cu_header. **Updated by dwarf_next_cu_header in
        dwarf_die_deliv.c */
    Dwarf_CU_Context de_cu_context;
    /*  Points to linked list of CU Contexts for the
        CU's already read.  These are only CU's read
        by dwarf_next_cu_header(). */
    Dwarf_CU_Context de_cu_context_list;
    /*  Points to the last CU Context added to the list by
        dwarf_next_cu_header(). */
    Dwarf_CU_Context de_cu_context_list_end;

    /*  Offset of last byte of last CU read.
        Actually one-past that last byte.  So
        use care and compare as offset >= de_last_offset
        to know if offset is too big. */
    Dwarf_Unsigned de_last_offset;
    /*  de_last_di_info_ptr and de_last_die are used with
        dwarf_siblingof, dwarf_child, and dwarf_validate_die_sibling.
        dwarf_validate_die_sibling will not give meaningful results
        if called inappropriately. */
    Dwarf_Byte_Ptr  de_last_di_ptr;
    Dwarf_Die  de_last_die;
};
typedef struct Dwarf_Debug_InfoTypes_s *Dwarf_Debug_InfoTypes;

/*  As the tasks performed on a debug related section is the same,
    in order to make the process of adding a new section
    (very unlikely) a little bit easy and to reduce the
    possibility of errors, a simple table
    build dynamically, will contain the relevant information.
*/

struct Dwarf_dbg_sect_s {
    /*  Debug section name must not be freed, is quoted string.
        This is the name from the object file itself. */
    const char    *ds_name;
    /* The section number in object section numbering. */
    Dwarf_Unsigned ds_number;
    /*   Debug section information, points to de_debug_*member
        (or the like) of the dbg struct.  */
    struct Dwarf_Section_s *ds_secdata;

    unsigned ds_groupnumber;
    int ds_duperr;            /* Error code for duplicated section */
    int ds_emptyerr;          /* Error code for empty section */
    int ds_have_dwarf;        /* Section contains DWARF */
    int ds_have_zdebug;       /* Section compressed: .zdebug name */
};

/*  As the number of debug sections does not change very often,
    in the case a
    new section is added in 'enter_section_in_array()'
    the 'MAX_DEBUG_SECTIONS' must
    be updated accordingly.
    This does not yet allow for section-groups in object files,
    for which many .debug_info (and other) sections may exist.
*/
#define DWARF_MAX_DEBUG_SECTIONS 50
#define DWARFSTRING_ALLOC_SIZE   200

/*  A 'magic number' to validate a Dwarf_Debug pointer is live.*/
#define DBG_IS_VALID 0xebfdebfd

/*  All the Dwarf_Debug tied-file info in one place.  */
struct Dwarf_Tied_Data_s {
    /*  Used to access executable from .dwo or .dwp object.
        Pointer to the tied_to Dwarf_Debug*/
    Dwarf_Debug td_tied_object;

    /*  Used for Type Unit signatures.
        Type Units are in .debug_types in DW4
        but in .debug_info in DW5.
        Some .debug_info point to them symbolically
        via DW_AT_signature attributes.
        If non-zero is a dwarf_tsearch 'tree'.
        Only non-zero if
        we had a reason to build the search tree..
        Type Units have a Dwarf_Sig8 signature
        in the header, and such is recorded here.

        Type Unit signatures can conflict with
        signatures in split-dwarf (dwo/dwp) sections.

        The Key for each record is a Dwarf_Sig8 (8 bytes).
        The data for each is a pointer to a Dwarf_CU_Context
        record in this dbg (cu_context in
        one of tied dbg's de_cu_context_list). */
    void *td_tied_search;

};

/*  dg_groupnum 0 does not exist.
    dg_groupnum 1 is base
    dg_groupnum 2 is dwo
    dg_groupnum 3 and higher are COMDAT groups (if any).

    We assume the number of groups will not exceed
    event the Windows 16 bit int maximum.
*/
struct Dwarf_Group_Data_s {
    /* For traditional DWARF the value is one, just one group. */
    unsigned gd_number_of_groups;

    /* Raw elf (elf-like) section count. */
    unsigned gd_number_of_sections;

    unsigned gd_map_entry_count;

    /* A map from section number to group number. */
    void *gd_map;
};

struct Dwarf_Debug_s {
    Dwarf_Unsigned de_magic;
    /*  All file access methods and support data
        are hidden in this structure.
        We get a pointer, callers control the lifetime of the
        structure and contents. */
    struct Dwarf_Obj_Access_Interface_a_s *de_obj_file;

    /*  See dwarf_generic_init.c comments on the
        use of the next four fields. And see
        DBG_IS_SECONDARY(p) DBG_IS_PRIMARY(p)
        DBG_HAS_SECONDARY(p) below and  also dwarf_util.c */
    struct Dwarf_Debug_s * de_dbg;
    struct Dwarf_Debug_s * de_primary_dbg;
    struct Dwarf_Debug_s * de_secondary_dbg;
    struct Dwarf_Debug_s * de_errors_dbg;

    Dwarf_Handler de_errhand;
    Dwarf_Ptr de_errarg;

    /*  Enabling us to close an fd if we own it,
        as in the case of dwarf_init_path().
        de_fd is only meaningful
        if de_owns_fd is set.  Each object
        file type has any necessary fd recorded
        under de_obj_file. */
    int  de_fd;
    char de_owns_fd;
    Dwarf_Small de_ftype; /* DW_FTYPE_PE, ... */
    char de_in_tdestroy; /* for de_alloc_tree  DW202309-001 */
    /* DW_PATHSOURCE_BASIC or MACOS or DEBUGLINK */
    Dwarf_Small de_path_source;
    Dwarf_Small de_preferred_load_type; /* DW_LOAD_PREF_MALLOC etc*/
    /*  de_path is only set automatically if dwarf_init_path()
        was used to initialize things.
        Used with the .gnu_debuglink section. */
    const char *de_path;

    const char ** de_gnu_global_paths;
    unsigned      de_gnu_global_path_count;

    struct Dwarf_Debug_InfoTypes_s de_info_reading;
    struct Dwarf_Debug_InfoTypes_s de_types_reading;

    /*  DW_GROUPNUMBER_ANY, DW_GROUPNUMBER_BASE, DW_GROUPNUMBER_DWO,
        or a comdat group number > 2
        Selected at init time of this dbg based on
        user request and on data in the object. */
    unsigned de_groupnumber;

    /* Supporting data for groupnumbers. */
    struct Dwarf_Group_Data_s de_groupnumbers;

    /*  Number of bytes in the length, and offset field in various
        .debu* sections.  It's not very meaningful, and is
        only used in one 'approximate' calculation.
        de_offset_size would be a more apropos name. */
    Dwarf_Small de_length_size;

    /*  Size of the object file in bytes. If Unknown
        leave this zero. */
    Dwarf_Unsigned de_filesize;

    /*  The value is what the object file encodes for
        the machine, In an  Elf Header, for example, its value
        comes from the e_machine field.
        MACOS provides a cputype field.
        PE provides IMAGE_FILE_HEADER.Machine.
        Inspect de_ftype using the value of
        de_obj_machine or the following de_obj_* fields. */
    Dwarf_Unsigned de_obj_machine;

    /*  For Elf is the obj header e_type field.
        Not yet specified: value for MachO or PE objects,
        so expect zero */
    Dwarf_Unsigned de_obj_type;

    /*  For DW_FTYPE_APPLEUNIVERSAL this is the
        offset of an executable object in the multi-executable
        file.  For all other de_ftype values this has
        value zero. */
    Dwarf_Unsigned de_obj_ub_offset;
    /*  The flags field from an Elf or Macos header
        or the Charactersics field from a PE header. */
    Dwarf_Unsigned de_obj_flags;

    /*  number of bytes in a pointer of the target in various .debug_
        sections. 4 in 32bit, 8 in MIPS 64, ia64.
        This is taken from object file headers. */
    Dwarf_Small de_pointer_size;

    /*  set at creation of a Dwarf_Debug to say if form_string
        should be checked for valid length at every call.
        0 means do the check.
        non-zero means do not do the check. */
    Dwarf_Small de_assume_string_in_bounds;

    /*  Keep track of allocations so a dwarf_finish call can clean up.
        Null till a tree is created */
    void * de_alloc_tree;

    /*  These fields are used to process debug_frame section.
        Updated
        by dwarf_get_fde_list in dwarf_frame.h */
    /*  Points to contiguous block of pointers to
        Dwarf_Cie_s structs. */
    Dwarf_Cie *de_cie_data;
    /*  Count of number of Dwarf_Cie_s structs. */
    Dwarf_Signed de_cie_count;
    /*  Keep eh (GNU) separate!. */
    Dwarf_Cie *de_cie_data_eh;
    Dwarf_Signed de_cie_count_eh;
    /*  Points to contiguous block of pointers to
        Dwarf_Fde_s structs. */
    Dwarf_Fde *de_fde_data;
    /*  Count of number of Dwarf_Fde_s structs. */
    Dwarf_Unsigned de_fde_count;
    /*  Keep eh (GNU) separate!. */
    Dwarf_Fde *de_fde_data_eh;
    Dwarf_Unsigned de_fde_count_eh;

    struct Dwarf_Section_s de_debug_info;
    struct Dwarf_Section_s de_debug_types;
    struct Dwarf_Section_s de_debug_abbrev;
    struct Dwarf_Section_s de_debug_line;
    struct Dwarf_Section_s de_debug_line_str; /* New in DWARF5 */
    struct Dwarf_Section_s de_debug_loc;
    struct Dwarf_Section_s de_debug_aranges;
    struct Dwarf_Section_s de_debug_macinfo;
    struct Dwarf_Section_s de_debug_macro;    /* New in DWARF5 */
    struct Dwarf_Section_s de_debug_names;    /* New in DWARF5 */
    struct Dwarf_Section_s de_debug_pubnames;
    struct Dwarf_Section_s de_debug_str;
    struct Dwarf_Section_s de_debug_sup;      /* New in DWARF5 */
    struct Dwarf_Section_s de_debug_loclists; /* New in DWARF5 */
    struct Dwarf_Section_s de_debug_rnglists; /* New in DWARF5 */
    struct Dwarf_Section_s de_debug_frame;
    struct Dwarf_Section_s de_gnu_debuglink;  /* New Sept. 2019 */
    struct Dwarf_Section_s de_note_gnu_buildid; /* New Sept. 2019 */

    /* gnu: the g++ eh_frame section */
    struct Dwarf_Section_s de_debug_frame_eh_gnu;

    /* DWARF3 .debug_pubtypes */
    struct Dwarf_Section_s de_debug_pubtypes;

    /*  Four SGI IRIX extensions essentially
        identical to DWARF3 .debug_pubtypes.
        Only on SGI IRIX. */
    struct Dwarf_Section_s de_debug_funcnames;
    struct Dwarf_Section_s de_debug_typenames;
    struct Dwarf_Section_s de_debug_varnames;
    struct Dwarf_Section_s de_debug_weaknames;

    struct Dwarf_Section_s de_debug_ranges;
    /*  Following two part of DebugFission and DWARF5 */
    struct Dwarf_Section_s de_debug_str_offsets;
    struct Dwarf_Section_s de_debug_addr;

    /*  For the .debug_rnglists[.dwo] section */
    Dwarf_Unsigned de_rnglists_context_count;
    /*  pointer to array of pointers to
        rnglists context instances */
    Dwarf_Rnglists_Context *  de_rnglists_context;

    /*  For the .debug_loclists[.dwo] section */
    Dwarf_Unsigned de_loclists_context_count;
    /*  pointer to array of pointers to
        loclists context instances */
    Dwarf_Loclists_Context *  de_loclists_context;

    /* Following for the .gdb_index section.  */
    struct Dwarf_Section_s de_debug_gdbindex;

    /*  Types in DWARF5 are in .debug_info
        and in DWARF4 are in .debug_types.
        These indexes first standardized in DWARF5,
        DWARF4 can have them as an extension.
        The next to refer to the DWP index sections and the
        tu and cu indexes sections are distinct in DWARF4 & 5. */
    struct Dwarf_Section_s de_debug_cu_index;
    struct Dwarf_Section_s de_debug_tu_index;
    struct Dwarf_Section_s de_debug_gnu_pubnames;
    struct Dwarf_Section_s de_debug_gnu_pubtypes;

    /*  For non-elf, simply leave the following two structs
        zeroed and they will be ignored. */
    struct Dwarf_Section_s de_elf_symtab;
    struct Dwarf_Section_s de_elf_strtab;

    /*  For a .dwp object file .
        For DWARF4, type units are in .debug_types
            (DWP is a GNU extension in DW4)..
        For DWARF5, type units are in .debug_info.
    */
    Dwarf_Xu_Index_Header  de_cu_hashindex_data;
    Dwarf_Xu_Index_Header  de_tu_hashindex_data;

    void (*de_copy_word) (void *dw_targ, const void *dw_src,
        unsigned long dw_len);
    unsigned char de_elf_must_close; /* If non-zero, then
        it was dwarf_init (not dwarf_elf_init)
        so must elf_end() */

    /* Default is DW_FRAME_INITIAL_VALUE from header. */
    Dwarf_Unsigned de_frame_rule_initial_value;

    /* Default is   DW_FRAME_LAST_REG_NUM. */
    Dwarf_Unsigned de_frame_reg_rules_entry_count;

    Dwarf_Unsigned de_frame_cfa_col_number;
    Dwarf_Unsigned de_frame_same_value_number;
    Dwarf_Unsigned de_frame_undefined_value_number;

    /*  If count > 0 means the DW_FTYPE_APPLEUNIVERSAL
        we initially read has this number of
        binaries in it, and de_universalbinary_index
        is the index of the current object inside
        the universal binary. */
    unsigned int   de_universalbinary_count;
    unsigned int   de_universalbinary_index;

    unsigned char de_big_endian_object; /* Non-zero if
        object being read is big-endian. */

    /*  Non-zero if dwarf_get_globals(), dwarf_get_funcs,
        dwarf_get_types,dwarf_get_pubtypes,
        dwarf_get_vars,dwarf_get_weaks should create
        and return a special zero-die-offset for the
        corresponding pubnames-style section CU header with
        zero pubnames-style named DIEs.  In that case the
        list returned will have an entry with a zero for
        the die-offset (which is an impossible debug_info
        die_offset). New March 2019.
        See dwarf_return_empty_pubnames() */
    unsigned char de_return_empty_pubnames;

    struct Dwarf_dbg_sect_s de_debug_sections[
        DWARF_MAX_DEBUG_SECTIONS];

    /* Number actually used. */
    unsigned de_debug_sections_total_entries;

    struct Dwarf_Harmless_s de_harmless_errors;

    struct Dwarf_Printf_Callback_Info_s  de_printf_callback;
    void *   de_printf_callback_null_device_handle;

    /*  Used in a tied dbg  to hold global info
        on the tied object (DW_AT_dwo_id).
        And for Type Unit signatures whether tied
        or not. It is not defined whether
        the main object is executable and
        the tied file is a dwo/dwp or the
        reverse. The focus of reporting
        is on the main file, but the tied
        file is sometimes needed
        and referenced.*/
    struct Dwarf_Tied_Data_s de_tied_data;

    /*  The following two are used to detect a DWARF4
        .debug_addr (a GNU extension) and attempt
        to print a raw .debug_addr section.  Simply
        assuming the first CU seen values for these
        work for everything in the GNU extenstion
        .debug_addr section. Only needed if the version
        here is 4 (DWARF4). */
    Dwarf_Half de_debug_addr_version;
    Dwarf_Half de_debug_addr_offset_size;
    Dwarf_Half de_debug_addr_address_size;
};

/* New style. takes advantage of dwarfstrings capability.
    This not a public function. */
void  _dwarf_printf(Dwarf_Debug dbg, const char * data);

typedef struct Dwarf_Chain_s *Dwarf_Chain;
struct Dwarf_Chain_s {
    void *ch_item;
    int  ch_itemtype; /* Needed to dealloc chain contents */
    Dwarf_Chain ch_next;
};

typedef struct Dwarf_Chain_o *Dwarf_Chain_2;
struct Dwarf_Chain_o {
    Dwarf_Off ch_item;
    Dwarf_Chain_2 ch_next;
};

    /* Size of cu header version stamp field. */
#define CU_VERSION_STAMP_SIZE   DWARF_HALF_SIZE

    /* Size of cu header address size field. */
#define CU_ADDRESS_SIZE_SIZE    sizeof(Dwarf_Small)

#define ORIGINAL_DWARF_OFFSET_SIZE  4
/*  The DISTINGUISHED VALUE is 4 byte value defined by DWARF
    since DWARF3. */
#define DISTINGUISHED_VALUE  0xffffffff
#define DISTINGUISHED_VALUE_OFFSET_SIZE 8
#define DISTINGUISHED_VALUE_ARRAY(x)  char (x)[4] = \
    { 0xff,0xff,0xff,0xff }

/*  We don't load the sections until they are needed.
    This function is used to load the section.  */
int _dwarf_load_section(Dwarf_Debug,
    struct Dwarf_Section_s *,
    Dwarf_Error *);

void _dwarf_dealloc_rnglists_context(Dwarf_Debug dbg);
void _dwarf_dealloc_loclists_context(Dwarf_Debug dbg);

int _dwarf_get_string_base_attr_value(Dwarf_Debug dbg,
    Dwarf_CU_Context context,
    Dwarf_Unsigned *sbase_out,
    Dwarf_Error *error);

int _dwarf_look_in_local_and_tied_by_index(
    Dwarf_Debug dbg,
    Dwarf_CU_Context context,
    Dwarf_Unsigned index,
    Dwarf_Addr *return_addr,
    Dwarf_Error *error);

Dwarf_Bool _dwarf_file_has_debug_fission_cu_index(Dwarf_Debug dbg);
Dwarf_Bool _dwarf_file_has_debug_fission_tu_index(Dwarf_Debug dbg);
Dwarf_Bool _dwarf_file_has_debug_fission_index(Dwarf_Debug dbg);

/* This should only be called on a CU. Never a TU. */
int _dwarf_get_debugfission_for_offset(Dwarf_Debug dbg,
    Dwarf_Off   offset_wanted,
    const char *keytype, /* "cu" or "tu" */
    Dwarf_Debug_Fission_Per_CU *  percu_out,
    Dwarf_Error *error);

/* whichone: must be a valid DW_SECT* macro value. */
Dwarf_Unsigned _dwarf_get_dwp_extra_offset(
    struct Dwarf_Debug_Fission_Per_CU_s* dwp,
    unsigned whichone, Dwarf_Unsigned * size);

/*  This will look into the tied Dwarf_Debug
    to which should have a skeleton CU DIE
    and an addr_base and also have the .debug_addr
    section. */

int _dwarf_get_addr_from_tied(Dwarf_Debug dbg,
    Dwarf_CU_Context context,
    Dwarf_Unsigned addrindex,
    Dwarf_Addr *addr_out,
    Dwarf_Error *error);

int _dwarf_get_fission_addition_die(Dwarf_Die die, int dw_sect_index,
    Dwarf_Unsigned* offset, Dwarf_Unsigned*size,
    Dwarf_Error *error);
int _dwarf_get_addr_index_itself(int theform,
    Dwarf_Small *info_ptr,
    Dwarf_Debug dbg,
    Dwarf_CU_Context cu_context,
    Dwarf_Unsigned *val_out,
    Dwarf_Error * error);
Dwarf_Bool _dwarf_addr_form_is_indexed(int form);

int _dwarf_load_die_containing_section(Dwarf_Debug dbg,
    Dwarf_Bool is_info,
    Dwarf_Error *error);

int _dwarf_create_a_new_cu_context_record_on_list(
    Dwarf_Debug dbg,
    Dwarf_Debug_InfoTypes dis,
    Dwarf_Bool is_info,
    Dwarf_Unsigned section_size,
    Dwarf_Unsigned new_cu_offset,
    Dwarf_CU_Context *context_out,
    Dwarf_Die        *cu_die_out,
    Dwarf_Error *error);
Dwarf_Unsigned _dwarf_calculate_next_cu_context_offset(
    Dwarf_CU_Context cu_context);

int _dwarf_search_for_signature(Dwarf_Debug dbg,
    Dwarf_Sig8 sig,
    Dwarf_CU_Context *context_out,
    Dwarf_Error *error);

int _dwarf_merge_all_base_attrs_of_cu_die(Dwarf_CU_Context context,
    Dwarf_Debug tieddbg,
    Dwarf_CU_Context *tiedcontext_out,
    Dwarf_Error *error);

void _dwarf_tied_destroy_free_node(void *node);
void _dwarf_destroy_group_map(Dwarf_Debug dbg);

int _dwarf_section_get_target_group(Dwarf_Debug dbg,
    unsigned   obj_section_index,
    unsigned * groupnumber,
    Dwarf_Error    * error);

int _dwarf_dwo_groupnumber_given_name(
    const char *name,
    unsigned *grpnum_out);

int _dwarf_section_get_target_group_from_map(Dwarf_Debug dbg,
    unsigned   obj_section_index,
    unsigned * groupnumber_out,
    Dwarf_Error    * error);

int _dwarf_insert_in_group_map(Dwarf_Debug dbg,
    unsigned groupnum,
    unsigned section_index,
    const char *name,
    Dwarf_Error * error);

/*  returns TRUE/FALSE: meaning this section name is in
    map for this groupnum  or not.*/
int _dwarf_section_in_group_by_name(Dwarf_Debug dbg,
    const char * scn_name,
    unsigned groupnum);

int
_dwarf_next_cu_header_internal(Dwarf_Debug dbg,
    Dwarf_Bool is_info,
    Dwarf_Die * cu_die_out,
    Dwarf_Unsigned * cu_header_length,
    Dwarf_Half * version_stamp,
    Dwarf_Unsigned * abbrev_offset,
    Dwarf_Half * address_size,
    Dwarf_Half * offset_size,
    Dwarf_Half * extension_size,
    Dwarf_Sig8 * signature,
    Dwarf_Bool * has_signature,
    Dwarf_Unsigned *typeoffset,
    Dwarf_Unsigned * next_cu_offset,
    Dwarf_Half     * header_cu_type,
    Dwarf_Error * error);

/* Relates to .debug_addr */
int _dwarf_look_in_local_and_tied(Dwarf_Half attr_form,
    Dwarf_CU_Context context,
    Dwarf_Small *info_ptr,
    Dwarf_Addr *return_addr,
    Dwarf_Error *error);

int _dwarf_get_ranges_base_attr_from_tied(Dwarf_Debug dbg,
    Dwarf_CU_Context context,
    Dwarf_Unsigned * ranges_base_out,
    Dwarf_Unsigned * addr_base_out,
    Dwarf_Error    * error);

int _dwarf_get_string_from_tied(Dwarf_Debug dbg,
    Dwarf_Unsigned offset,
    char **return_str, Dwarf_Error*error);

int _dwarf_valid_form_we_know(Dwarf_Unsigned at_form,
    Dwarf_Unsigned at_name);
int _dwarf_extract_local_debug_str_string_given_offset(
    Dwarf_Debug dbg,
    unsigned attrform,
    Dwarf_Unsigned offset,
    char ** return_str,
    Dwarf_Error *  error);

int _dwarf_file_name_is_full_path(Dwarf_Small  *fname);

/* This is libelf access to Elf object. */
extern int _dwarf_elf_setup(int fd,
    char *true_path_out_buffer,
    unsigned ftype,
    unsigned endian,
    unsigned offsetsize,
    size_t filesize,
    unsigned groupnumber,
    Dwarf_Handler errhand,
    Dwarf_Ptr errarg,
    Dwarf_Debug *dbg,Dwarf_Error *error);

/*  This is non-libelf Elf access */
extern int
_dwarf_elf_nlsetup(int fd,
    char *true_path,
    unsigned ftype,
    unsigned endian,
    unsigned offsetsize,
    size_t filesize,
    unsigned groupnumber,
    Dwarf_Handler errhand,
    Dwarf_Ptr errarg,
    Dwarf_Debug *dbg,Dwarf_Error *error);

extern int _dwarf_macho_setup(int fd,
    char *true_path,
    unsigned universalnumber,
    unsigned ftype,
    unsigned endian,
    unsigned offsetsize,
    Dwarf_Unsigned filesize,
    unsigned groupnumber,
    Dwarf_Handler errhand,
    Dwarf_Ptr errarg,
    Dwarf_Debug *dbg,Dwarf_Error *error);

extern int _dwarf_pe_setup(int fd,
    char *path,
    unsigned ftype,
    unsigned endian,
    unsigned offsetsize,
    size_t filesize,
    unsigned groupnumber,
    Dwarf_Handler errhand,
    Dwarf_Ptr errarg,
    Dwarf_Debug *dbg,Dwarf_Error *error);

void _dwarf_create_address_size_dwarf_error(Dwarf_Debug dbg,
    Dwarf_Error *error,
    Dwarf_Unsigned addrsize,
    int errcode,const char *errname);

extern Dwarf_Bool _dwarf_allow_formudata(unsigned form);
extern int _dwarf_formudata_internal(Dwarf_Debug dbg,
    Dwarf_Attribute attr,
    unsigned form,
    Dwarf_Byte_Ptr data,
    Dwarf_Byte_Ptr section_end,
    Dwarf_Unsigned *return_uval,
    Dwarf_Unsigned *bytes_read,
    Dwarf_Error *error);

Dwarf_Byte_Ptr _dwarf_calculate_info_section_start_ptr(
    Dwarf_CU_Context context,
    Dwarf_Unsigned *section_len_out);

Dwarf_Byte_Ptr _dwarf_calculate_info_section_end_ptr(
    Dwarf_CU_Context context);
Dwarf_Byte_Ptr _dwarf_calculate_abbrev_section_end_ptr(
    Dwarf_CU_Context context);

void _dwarf_closer(int fd);
int  _dwarf_readr(int fd, char *buf, Dwarf_Unsigned size,
    Dwarf_Unsigned *sizeread);
int  _dwarf_seekr(int fd, Dwarf_Unsigned loc, int seektype,
    Dwarf_Unsigned *out_loc);
int  _dwarf_openr(const char *name);

/*   This does free or munmap as appropriate. */
void _dwarf_malloc_section_free(struct Dwarf_Section_s * sec);

enum Dwarf_Sec_Alloc_Pref
    _dwarf_determine_section_allocation_type(void);

int _dwarf_formblock_internal(Dwarf_Debug dbg,
    Dwarf_Attribute attr,
    Dwarf_CU_Context cu_context,
    Dwarf_Block * return_block,
    Dwarf_Error * error);

int _dwarf_extract_data16(Dwarf_Debug dbg,
    Dwarf_Small *data,
    Dwarf_Small *section_start,
    Dwarf_Small *section_end,
    Dwarf_Form_Data16  * returned_val,
    Dwarf_Error *error);

int _dwarf_fill_in_attr_form_abtable(Dwarf_CU_Context context,
    Dwarf_Byte_Ptr abbrev_ptr,
    Dwarf_Byte_Ptr abbrev_end,
    Dwarf_Abbrev_List abbrev_list,
    Dwarf_Error *error);

int _dwarf_internal_find_die_given_sig8(Dwarf_Debug dbg,
    int context_level,
    Dwarf_Sig8 *ref,
    Dwarf_Die  *die_out,
    Dwarf_Bool *is_info,
    Dwarf_Error *error);
int
_dwarf_internal_global_formref_b(Dwarf_Attribute attr,
    int context_level,
    Dwarf_Off * ret_offset,
    Dwarf_Bool * offset_is_info,
    Dwarf_Error * error);

int
_dwarf_has_SECT_fission(Dwarf_CU_Context ctx,
    unsigned int      SECT_number, /* example: DW_SECT_RNGLISTS */
    Dwarf_Bool       *hasfissionoffset,
    Dwarf_Unsigned   *loclistsbase);

int _dwarf_skip_leb128(char * leb,
    Dwarf_Unsigned * leblen,
    char           * endptr);

/*  Used for DW_AT_ranges to get base address along with
    dwarf_lowpc() */
int
_dwarf_entrypc(Dwarf_Die die,
    Dwarf_Addr  *return_addr,
    Dwarf_Error *error);

int _dwarf_get_suppress_debuglink_crc(void);
void _dwarf_dumpsig(const char *msg, Dwarf_Sig8 *sig, int lineno);
