/*
  Copyright (C) 2000-2006 Silicon Graphics, Inc.  All Rights Reserved.
  Portions Copyright (C) 2007-2020 David Anderson. All Rights Reserved.
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

/*  This  implements _dwarf_get_fde_list_internal()
    and related helper functions for reading cie/fde data.  */

#include <config.h>

#include <stdlib.h> /* qsort() */
#include <stdio.h> /* printf() */
#include <string.h> /* memcpy() memset() strcmp()
    strncmp() strlen() */

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
#include "dwarf_alloc.h"
#include "dwarf_error.h"
#include "dwarf_util.h"
#include "dwarf_frame.h"
#include "dwarf_arange.h" /* using Arange as a way to build a list */
#include "dwarf_string.h"

/*  For a little information about .eh_frame see
    https://stackoverflow.com/questions/14091231/
    what-do-the-eh-frame-and-eh-frame-hdr-sections-store-exactly
    http://refspecs.linuxfoundation.org/LSB_3.0.0/
    LSB-Core-generic/LSB-Core-generic/ehframechpt.html
    The above give information about fields and sizes but
    very very little about content.

    .eh_frame_hdr contains data for C++ unwinding. Namely
    tables for fast access into .eh_frame.
*/

#if 0  /* dump_bytes FOR DEBUGGING */
/* For debugging only. */
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
}

#endif

static int _dwarf_find_existing_cie_ptr(Dwarf_Small * cie_ptr,
    Dwarf_Cie cur_cie_ptr,
    Dwarf_Cie * cie_ptr_to_use_out,
    Dwarf_Cie head_cie_ptr);
static void _dwarf_dealloc_fde_cie_list_internal(
    Dwarf_Fde head_fde_ptr,
    Dwarf_Cie head_cie_ptr);
static int _dwarf_create_cie_from_start(Dwarf_Debug dbg,
    Dwarf_Small * cie_ptr_val,
    Dwarf_Small * section_ptr,
    Dwarf_Unsigned section_index,
    Dwarf_Unsigned section_length,
    Dwarf_Small * section_ptr_end,
    Dwarf_Unsigned cie_id_value,
    Dwarf_Unsigned cie_count,
    int use_gnu_cie_calc,
    Dwarf_Cie * cie_ptr_to_use_out,
    Dwarf_Error * error);

static int _dwarf_get_gcc_eh_augmentation(Dwarf_Debug dbg,
    Dwarf_Small * frame_ptr,
    Dwarf_Unsigned *size_of_augmentation_data,
    enum Dwarf_augmentation_type augtype,
    Dwarf_Small * section_end_pointer,
    char *augmentation,
    Dwarf_Error *error);

static int
_dwarf_gnu_aug_encodings(Dwarf_Debug dbg, char *augmentation,
    Dwarf_Small * aug_data, Dwarf_Unsigned aug_data_len,
    Dwarf_Half address_size,
    unsigned char *pers_hand_enc_out,
    unsigned char *lsda_enc_out,
    unsigned char *fde_begin_enc_out,
    Dwarf_Addr * gnu_pers_addr_out,
    Dwarf_Error *error);

static int _dwarf_read_encoded_ptr(Dwarf_Debug dbg,
    Dwarf_Small * section_pointer,
    Dwarf_Small * input_field,
    int gnu_encoding,
    Dwarf_Small * section_ptr_end,
    Dwarf_Half address_size,
    Dwarf_Unsigned * addr,
    Dwarf_Small ** input_field_out,
    Dwarf_Error *error);

/*  Called by qsort to compare FDE entries.
    Consumer code expects the array of FDE pointers to be
    in address order.
*/
static int
qsort_compare(const void *elem1, const void *elem2)
{
    const Dwarf_Fde fde1 = *(const Dwarf_Fde *) elem1;
    const Dwarf_Fde fde2 = *(const Dwarf_Fde *) elem2;
    Dwarf_Addr addr1 = fde1->fd_initial_location;
    Dwarf_Addr addr2 = fde2->fd_initial_location;

    if (addr1 < addr2) {
        return -1;
    } else if (addr1 > addr2) {
        return 1;
    }
    return 0;
}

/*  Adds 'newone' to the end of the list starting at 'head'
    and makes the new one current. */
static void
chain_up_fde(Dwarf_Fde newone, Dwarf_Fde * head, Dwarf_Fde * cur)
{
    if (*head == NULL)
        *head = newone;
    else {
        (*cur)->fd_next = newone;
    }
    *cur = newone;

}

/*  Adds 'newone' to the end of the list starting at 'head'
    and makes the new one current. */
static void
chain_up_cie(Dwarf_Cie newone, Dwarf_Cie * head, Dwarf_Cie * cur)
{
    if (*head == NULL) {
        *head = newone;
    } else {
        (*cur)->ci_next = newone;
    }
    *cur = newone;
}

/*  The size of the length field plus the
    value of length must be an integral
    multiple of the address size.  Dwarf4 standard.

    A constant that gives the number of bytes of the CIE
    structure, not including the length field itself
    (where length mod <size of an address> == 0)
    (see Section 7.2.2). Dwarf3 standard.

    A uword constant that gives the number of bytes of
    the CIE structure, not including the
    length field, itself (length mod <addressing unit size> == 0).
    Dwarf2 standard.*/
static void
validate_length(Dwarf_Debug dbg,
    Dwarf_Cie cieptr, Dwarf_Unsigned length,
    Dwarf_Unsigned length_size,
    Dwarf_Unsigned extension_size,
    Dwarf_Small * section_ptr,
    Dwarf_Small * ciefde_start,
    const char * cieorfde)
{
    Dwarf_Unsigned address_size = 0;
    Dwarf_Unsigned length_field_summed = length_size + extension_size;
    Dwarf_Unsigned total_len = length + length_field_summed;
    Dwarf_Unsigned mod = 0;

    if (cieptr) {
        address_size = cieptr->ci_address_size;
    } else {
        address_size = dbg->de_pointer_size;
    }
    mod = total_len % address_size;
    if (mod != 0) {
        dwarfstring  harm;
        Dwarf_Unsigned sectionoffset = ciefde_start - section_ptr;

        dwarfstring_constructor(&harm);
        if (!cieorfde || (strlen(cieorfde) > 3)) {
            /*  Coding error or memory corruption? */
            cieorfde = "ERROR!";
        }
        dwarfstring_append_printf_u(&harm,
            "DW_DLE_DEBUG_FRAME_LENGTH_NOT_MULTIPLE"
            " len=0x%" DW_PR_XZEROS DW_PR_DUx,
            length);
        dwarfstring_append_printf_u(&harm,
            ", len size=0x%"  DW_PR_XZEROS DW_PR_DUx,
            length_size);
        dwarfstring_append_printf_u(&harm,
            ", extn size=0x%" DW_PR_XZEROS DW_PR_DUx,
            extension_size);
        dwarfstring_append_printf_u(&harm,
            ", totl length=0x%" DW_PR_XZEROS DW_PR_DUx,
            total_len);
        dwarfstring_append_printf_u(&harm,
            ", addr size=0x%" DW_PR_XZEROS DW_PR_DUx,
            address_size);
        dwarfstring_append_printf_u(&harm,
            ", mod=0x%" DW_PR_XZEROS DW_PR_DUx " must be zero",
            mod);
        dwarfstring_append_printf_s(&harm,
            " in %s",(char *)cieorfde);
        dwarfstring_append_printf_u(&harm,
            ", offset 0x%" DW_PR_XZEROS DW_PR_DUx ".",
            sectionoffset);
        dwarf_insert_harmless_error(dbg,
            dwarfstring_string(&harm));
        dwarfstring_destructor(&harm);
    }
    return;
}

#if 0 /* print_prefix() FOR DEBUGGING */
/* For debugging only. */
static void
print_prefix(struct cie_fde_prefix_s *prefix, int line)
{
    printf("prefix-print, prefix at 0x%lx, line %d\n",
        (unsigned long) prefix, line);
    printf("  start addr 0x%lx after prefix 0x%lx\n",
        (unsigned long) prefix->cf_start_addr,
        (unsigned long) prefix->cf_addr_after_prefix);
    printf("  length 0x%" DW_PR_DUx ", len size %d ext size %d\n",
        (Dwarf_Unsigned) prefix->cf_length,
        prefix->cf_local_length_size,
        prefix->cf_local_extension_size);
    printf("  cie_id 0x%" DW_PR_DUx " cie_id  cie_id_addr 0x%lx\n",
        (Dwarf_Unsigned) prefix->cf_cie_id,
        (long) prefix->cf_cie_id_addr);
    printf
        ("  sec ptr 0x%lx sec index %" DW_PR_DSd
        " sec len 0x%" DW_PR_DUx " sec past end 0x%lx\n",
        (unsigned long) prefix->cf_section_ptr,
        (Dwarf_Signed) prefix->cf_section_index,
        (Dwarf_Unsigned) prefix->cf_section_length,
        (unsigned long) prefix->cf_section_ptr +
        (unsigned long)prefix->cf_section_length);
}
#endif

/*  Make the 'cieptr' consistent across .debug_frame and .eh_frame.
    Calculate a pointer into section bytes given a cie_id in
    an FDE header.

    In .debug_frame, the CIE_pointer is an offset in .debug_frame.

    In .eh_frame, the CIE Pointer is, when
    cie_id_value subtracted from the
    cie_id_addr, the address in memory of
    a CIE length field.
    Since cie_id_addr is the address of an FDE CIE_Pointer
    field, cie_id_value for .eh_frame
    has to account for the length-prefix.
    so that the returned cieptr really points to
    a  CIE length field. Whew!
    Available documentation on this is just a bit
    ambiguous, but this calculation is correct.
*/

static int
get_cieptr_given_offset(Dwarf_Debug dbg,
    Dwarf_Unsigned cie_id_value,
    int use_gnu_cie_calc,
    Dwarf_Small * section_ptr,
    Dwarf_Unsigned section_length,
    Dwarf_Small * cie_id_addr,
    Dwarf_Small ** ret_cieptr,
    Dwarf_Error *error)
{
    if (cie_id_value >= section_length) {
        _dwarf_error_string(dbg, error,
            DW_DLE_DEBUG_FRAME_LENGTH_BAD,
            "DW_DLE_DEBUG_FRAME_LENGTH_BAD: in eh_frame "
            " cie_id value makes no sense. Corrupt DWARF");
        return DW_DLV_ERROR;
    }
    if (use_gnu_cie_calc) {
        /*  cie_id value is offset, in section, of the
            cie_id itself, to
            use vm ptr of the value,
            less the value, to get to the cie header.  */
        if ((Dwarf_Unsigned)(uintptr_t)cie_id_addr <= cie_id_value) {
            _dwarf_error_string(dbg, error,
                DW_DLE_DEBUG_FRAME_LENGTH_BAD,
                "DW_DLE_DEBUG_FRAME_LENGTH_BAD: in eh_frame "
                " cie_id makes no sense. Corrupt DWARF");
            return DW_DLV_ERROR;
        }
        *ret_cieptr = cie_id_addr - cie_id_value;
    } else {
        /*  Traditional dwarf section offset is in cie_id */
        *ret_cieptr = section_ptr + cie_id_value;
    }
    return DW_DLV_OK;
}

/*  Internal function called from various places to create
    lists of CIEs and FDEs.  Not directly called
    by consumer code */
int
_dwarf_get_fde_list_internal(Dwarf_Debug dbg, Dwarf_Cie ** cie_data,
    Dwarf_Signed * cie_element_count,
    Dwarf_Fde ** fde_data,
    Dwarf_Signed * fde_element_count,
    Dwarf_Small * section_ptr,
    Dwarf_Unsigned section_index,
    Dwarf_Unsigned section_length,
    Dwarf_Unsigned cie_id_value,
    int use_gnu_cie_calc, Dwarf_Error * error)
{
    /* Scans the debug_frame section. */
    Dwarf_Small *frame_ptr = section_ptr;
    Dwarf_Small *section_ptr_end = section_ptr + section_length;

    /*  New_cie points to the Cie being read, and head_cie_ptr and
        cur_cie_ptr are used for chaining them up in sequence.
        In case cie's are reused aggressively we need tail_cie_ptr
        to add to the chain.  If we re-use an early cie
        later on, that does not mean we chain a
        new cie to the early one,
        we always chain it to the tail.  */
    Dwarf_Cie head_cie_ptr = NULL;
    Dwarf_Cie cur_cie_ptr = NULL;
    Dwarf_Cie tail_cie_ptr = NULL;
    Dwarf_Unsigned cie_count = 0;

    /*  Points to a list of contiguous pointers to
        Dwarf_Cie structures.
    */
    Dwarf_Cie *cie_list_ptr = 0;

    /*  New_fde points to the Fde being created, and head_fde_ptr and
        cur_fde_ptr are used to chain them up. */
    Dwarf_Fde head_fde_ptr = NULL;
    Dwarf_Fde cur_fde_ptr = NULL;
    Dwarf_Unsigned fde_count = 0;

    /*  Points to a list of contiguous pointers to
        Dwarf_Fde structures.
    */
    Dwarf_Fde *fde_list_ptr = NULL;

    Dwarf_Unsigned i = 0;
    int res = DW_DLV_ERROR;

    if (frame_ptr == 0) {
        return DW_DLV_NO_ENTRY;
    }
    res = _dwarf_validate_register_numbers(dbg,error);
    if (res == DW_DLV_ERROR) {
        return res;
    }

    /*  We create the fde and cie arrays.
        Processing each CIE as we come
        to it or as an FDE refers to it.
        We cannot process 'late' CIEs
        late as GNU .eh_frame complexities
        mean we need the whole CIE
        before we can process the FDE correctly. */
    while (frame_ptr < section_ptr_end) {

        struct cie_fde_prefix_s prefix;

        /*  First read in the 'common prefix' to
            figure out what we are
            to do with this entry. */
        memset(&prefix, 0, sizeof(prefix));
        res = _dwarf_read_cie_fde_prefix(dbg,
            frame_ptr, section_ptr,
            section_index,
            section_length, &prefix, error);
        if (res == DW_DLV_ERROR) {
            _dwarf_dealloc_fde_cie_list_internal(head_fde_ptr,
                head_cie_ptr);
            return res;
        }
        if (res == DW_DLV_NO_ENTRY) {
            break;
        }
        frame_ptr = prefix.cf_addr_after_prefix;
        if (frame_ptr >= section_ptr_end) {
            _dwarf_dealloc_fde_cie_list_internal(head_fde_ptr,
                head_cie_ptr);
            _dwarf_error_string(dbg, error,
                DW_DLE_DEBUG_FRAME_LENGTH_BAD,
                "DW_DLE_DEBUG_FRAME_LENGTH_BAD: following "
                "a the start of a cie/fde we have run off"
                " the end of the section.  Corrupt Dwarf");

            return DW_DLV_ERROR;
        }

        if (prefix.cf_cie_id == cie_id_value) {
            /* This is a CIE.  */
            Dwarf_Cie cie_ptr_to_use = 0;
            int resc = 0;

            resc = _dwarf_find_existing_cie_ptr(prefix.cf_start_addr,
                cur_cie_ptr,
                &cie_ptr_to_use,
                head_cie_ptr);
            if (resc == DW_DLV_OK) {
                cur_cie_ptr = cie_ptr_to_use;
                /* Ok. Seen already. */
            } else if (resc == DW_DLV_NO_ENTRY) {
                /* CIE before its FDE in this case. */
                resc = _dwarf_create_cie_from_after_start(dbg,
                    &prefix,
                    section_ptr,
                    frame_ptr,
                    section_ptr_end,
                    cie_count,
                    use_gnu_cie_calc,
                    &cie_ptr_to_use,
                    error);
                if (resc != DW_DLV_OK) {
                    _dwarf_dealloc_fde_cie_list_internal(head_fde_ptr,
                        head_cie_ptr);
                    return resc;
                }
                cie_count++;
                chain_up_cie(cie_ptr_to_use, &head_cie_ptr,
                    &tail_cie_ptr);
                cur_cie_ptr = tail_cie_ptr;
            } else {            /* res == DW_DLV_ERROR */

                _dwarf_dealloc_fde_cie_list_internal(head_fde_ptr,
                    head_cie_ptr);
                return resc;
            }
            frame_ptr = cie_ptr_to_use->ci_cie_start +
                cie_ptr_to_use->ci_length +
                cie_ptr_to_use->ci_length_size +
                cie_ptr_to_use->ci_extension_size;
            continue;
        } else {
            /*  This is an FDE, Frame Description Entry, see the Dwarf
                Spec, (section 6.4.1 in DWARF2, DWARF3, DWARF4, ...)
                Or see the .eh_frame specification,
                from the Linux Foundation (or other source).  */
            int resf = DW_DLV_ERROR;
            Dwarf_Cie cie_ptr_to_use = 0;
            Dwarf_Fde fde_ptr_to_use = 0;
            Dwarf_Small *cieptr_val = 0;

            resf = get_cieptr_given_offset(dbg,
                prefix.cf_cie_id,
                use_gnu_cie_calc,
                section_ptr,
                section_length,
                prefix.cf_cie_id_addr,&cieptr_val,error);
            if (resf != DW_DLV_OK) {
                return resf;
            }
            resf = _dwarf_find_existing_cie_ptr(cieptr_val,
                cur_cie_ptr,
                &cie_ptr_to_use,
                head_cie_ptr);
            if (resf == DW_DLV_OK) {
                cur_cie_ptr = cie_ptr_to_use;
                /* Ok. Seen CIE already. */
            } else if (resf == DW_DLV_NO_ENTRY) {
                resf = _dwarf_create_cie_from_start(dbg,
                    cieptr_val,
                    section_ptr,
                    section_index,
                    section_length,
                    section_ptr_end,
                    cie_id_value,
                    cie_count,
                    use_gnu_cie_calc,
                    &cie_ptr_to_use,
                    error);
                if (resf == DW_DLV_ERROR) {
                    _dwarf_dealloc_fde_cie_list_internal(head_fde_ptr,
                        head_cie_ptr);
                    return resf;
                } else if (resf == DW_DLV_NO_ENTRY) {
                    return resf;
                }
                ++cie_count;
                chain_up_cie(cie_ptr_to_use, &head_cie_ptr,
                    &tail_cie_ptr);
                cur_cie_ptr = tail_cie_ptr;

            } else {
                /* DW_DLV_ERROR */
                return resf;
            }

            resf = _dwarf_create_fde_from_after_start(dbg,
                &prefix,
                section_ptr,
                section_length,
                frame_ptr,
                section_ptr_end,
                use_gnu_cie_calc,
                cie_ptr_to_use,
                cie_ptr_to_use->ci_address_size,
                &fde_ptr_to_use,
                error);
            if (resf == DW_DLV_ERROR) {
                _dwarf_dealloc_fde_cie_list_internal(head_fde_ptr,
                    head_cie_ptr);
                return resf;
            }
            if (resf == DW_DLV_NO_ENTRY) {
                /* impossible. */
                return resf;
            }
            chain_up_fde(fde_ptr_to_use, &head_fde_ptr, &cur_fde_ptr);
            fde_count++;
            /* ASSERT: DW_DLV_OK. */
            frame_ptr = cur_fde_ptr->fd_fde_start +
                cur_fde_ptr->fd_length +
                cur_fde_ptr->fd_length_size +
                cur_fde_ptr->fd_extension_size;
            if (frame_ptr  <  fde_ptr_to_use->fd_fde_instr_start) {
                /*  Sanity check. With a really short fde instruction
                    set and address_size we think is 8
                    as it is ELF64 (but is
                    really 4, as in DWARF{2,3} where we have
                    no FDE address_size) we emit an error.
                    This error means things will not go well. */
                _dwarf_dealloc_fde_cie_list_internal(head_fde_ptr,
                    head_cie_ptr);
                _dwarf_error(dbg,error,
                    DW_DLE_DEBUG_FRAME_POSSIBLE_ADDRESS_BOTCH);
                return DW_DLV_ERROR;
            }
            continue;
        }
    }
    /*  Now build list of CIEs from the list. If there are no CIEs
        there should be no FDEs. */
    if (cie_count > 0) {
        cie_list_ptr = (Dwarf_Cie *)
            _dwarf_get_alloc(dbg, DW_DLA_LIST, cie_count);
    } else {
        if (fde_count > 0) {
            _dwarf_dealloc_fde_cie_list_internal(head_fde_ptr,
                head_cie_ptr);
            _dwarf_error(dbg, error, DW_DLE_ORPHAN_FDE);
            return DW_DLV_ERROR;
        }
        _dwarf_dealloc_fde_cie_list_internal(head_fde_ptr,
            head_cie_ptr);
        return DW_DLV_NO_ENTRY;
    }
    if (cie_list_ptr == NULL) {
        _dwarf_dealloc_fde_cie_list_internal(head_fde_ptr,
            head_cie_ptr);
        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
        return DW_DLV_ERROR;
    }
    if (!head_cie_ptr) {
        /*  Should be impossible. */
        _dwarf_error_string(dbg, error,DW_DLE_DEBUGFRAME_ERROR,
            "DW_DLE_DEBUGFRAME_ERROR"
            "Impossible no head_cie_ptr");
        return DW_DLV_ERROR;
    }
    cur_cie_ptr = head_cie_ptr;
    for (i = 0; i < cie_count; i++) {
        *(cie_list_ptr + i) = cur_cie_ptr;
        cur_cie_ptr = cur_cie_ptr->ci_next;
    }

    /*  Now build array of FDEs from the list.
        With orphan CIEs (meaning no FDEs)
        lets not return DW_DLV_NO_ENTRY */
    if (fde_count > 0) {
        fde_list_ptr = (Dwarf_Fde *)
            _dwarf_get_alloc(dbg, DW_DLA_LIST, fde_count);
        if (!fde_list_ptr) {
            _dwarf_dealloc_fde_cie_list_internal(head_fde_ptr,
                head_cie_ptr);
            _dwarf_error_string(dbg, error,DW_DLE_ALLOC_FAIL,
                "DW_DLE_ALLOC_FAIL"
                "getting DW_DLA_LIST given fde_count");
            return DW_DLV_ERROR;
        }
    }

    /* It is ok if fde_list_ptr is NULL, we just have no fdes. */
    cur_fde_ptr = head_fde_ptr;
    for (i = 0; i < fde_count; i++) {
        *(fde_list_ptr + i) = cur_fde_ptr;
        cur_fde_ptr = cur_fde_ptr->fd_next;
    }

    /* Return arguments. */
    *cie_data = cie_list_ptr;
    *cie_element_count = cie_count;

    *fde_data = fde_list_ptr;
    *fde_element_count = fde_count;
    if (use_gnu_cie_calc) {
        dbg->de_fde_data_eh = fde_list_ptr;
        dbg->de_fde_count_eh = fde_count;
        dbg->de_cie_data_eh = cie_list_ptr;
        dbg->de_cie_count_eh = cie_count;
    } else {
        dbg->de_fde_data = fde_list_ptr;
        dbg->de_fde_count = fde_count;
        dbg->de_cie_data = cie_list_ptr;
        dbg->de_cie_count = cie_count;
    }

    /*  Sort the list by the address so that
        dwarf_get_fde_at_pc() can
        binary search this list.  */
    if (fde_count > 0) {
        qsort((void *) fde_list_ptr, fde_count, sizeof(Dwarf_Ptr),
            qsort_compare);
    }

    return DW_DLV_OK;
}

/*  Internal function, not called by consumer code.
    'prefix' has accumulated the info up thru the cie-id
    and now we consume the rest and build a Dwarf_Cie_s structure.
*/
int
_dwarf_create_cie_from_after_start(Dwarf_Debug dbg,
    struct cie_fde_prefix_s *prefix,
    Dwarf_Small * section_pointer,
    Dwarf_Small * frame_ptr,
    Dwarf_Small * section_ptr_end,
    Dwarf_Unsigned cie_count,
    int use_gnu_cie_calc,
    Dwarf_Cie * cie_ptr_out,
    Dwarf_Error * error)
{
    Dwarf_Cie new_cie = 0;

    /*  egcs-1.1.2 .eh_frame uses 0 as the distinguishing
        id. sgi uses
        -1 (in .debug_frame). .eh_frame not quite identical to
        .debug_frame */
    /*  We here default the address size as it is not present
        in DWARF2 or DWARF3 cie data, below we set it right if
        it is present. */
    Dwarf_Half address_size = dbg->de_pointer_size;
    Dwarf_Small *augmentation = 0;
    Dwarf_Half segment_size = 0;
    Dwarf_Signed data_alignment_factor = -1;
    Dwarf_Unsigned code_alignment_factor = 4;
    Dwarf_Unsigned return_address_register = 31;
    int local_length_size = 0;
    Dwarf_Unsigned leb128_length = 0;
    Dwarf_Unsigned cie_aug_data_len = 0;
    Dwarf_Small *cie_aug_data = 0;
    Dwarf_Addr gnu_personality_handler_addr = 0;
    unsigned char gnu_personality_handler_encoding = 0;
    unsigned char gnu_lsda_encoding = 0;
    unsigned char gnu_fde_begin_encoding = 0;
    int res = 0;
    Dwarf_Small version = 0;

    enum Dwarf_augmentation_type augt = aug_unknown;

    /*  This is a CIE, Common Information Entry: See the dwarf spec,
        section 6.4.1 */
    if (frame_ptr >= section_ptr_end) {
        _dwarf_error_string(dbg, error,
            DW_DLE_DEBUG_FRAME_LENGTH_BAD,
            "DW_DLE_DEBUG_FRAME_LENGTH_BAD: reading a cie"
            " version byte we have run off"
            " the end of the section.  Corrupt Dwarf");
        return DW_DLV_ERROR;
    }
    version = *(Dwarf_Small *) frame_ptr;

    if ((frame_ptr+2) >= section_ptr_end) {
        _dwarf_error_string(dbg, error, DW_DLE_DEBUG_FRAME_LENGTH_BAD,
            "DW_DLE_DEBUG_FRAME_LENGTH_BAD: reading an augmentation"
            " would run off"
            " the end of the section.  Corrupt Dwarf");
        return DW_DLV_ERROR;
    }
    if (version != DW_CIE_VERSION && version != DW_CIE_VERSION3 &&
        version != DW_CIE_VERSION4 && version != DW_CIE_VERSION5) {
        dwarfstring m;
        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            "DW_DLE_FRAME_VERSION_BAD: cie version %u unknown",
            version);
        _dwarf_error_string(dbg, error,
            DW_DLE_FRAME_VERSION_BAD,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    frame_ptr++;
    augmentation = frame_ptr;
    res = _dwarf_check_string_valid(dbg,section_pointer,
        frame_ptr,section_ptr_end,
        DW_DLE_AUGMENTATION_STRING_OFF_END,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    frame_ptr = frame_ptr + strlen((char *) frame_ptr) + 1;
    if (frame_ptr  >= section_ptr_end) {
        _dwarf_error_string(dbg, error,
            DW_DLE_DEBUG_FRAME_LENGTH_BAD,
            "DW_DLE_DEBUG_FRAME_LENGTH_BAD: following any "
            "augmentation field we have run off "
            "the end of the section "
            "with the CIE incomplete.  Corrupt Dwarf");
        return DW_DLV_ERROR;
    }
    augt = _dwarf_get_augmentation_type(dbg,
        augmentation, use_gnu_cie_calc);
    if (augt == aug_eh) {
        if ((frame_ptr+local_length_size)  >= section_ptr_end) {
            _dwarf_error_string(dbg, error,
                DW_DLE_DEBUG_FRAME_LENGTH_BAD,
                "DW_DLE_DEBUG_FRAME_LENGTH_BAD: following "
                "type field we have run off the end of the section "
                "with the CIE incomplete.  Corrupt Dwarf");
            return DW_DLV_ERROR;
        }
#if 0 /* obsolete sgi exception table reference. Ignore. */
        /* REFERENCED *//* Not used in this instance */
        Dwarf_Unsigned exception_table_addr = 0;
        /* this is per egcs-1.1.2 as on RH 6.0 */
        READ_UNALIGNED_CK(dbg, exception_table_addr,
            Dwarf_Unsigned, frame_ptr, local_length_size,
            error,section_ptr_end);
#endif
        frame_ptr += local_length_size;
    }
    {
        Dwarf_Unsigned lreg = 0;
        unsigned long size = 0;

        if (version == DW_CIE_VERSION4) {
            if ((frame_ptr+2)  >= section_ptr_end) {
                _dwarf_error_string(dbg, error,
                    DW_DLE_DEBUG_FRAME_LENGTH_BAD,
                    "DW_DLE_DEBUG_FRAME_LENGTH_BAD: "
                    "We would run off the end of the section "
                    "in a DWARF4 cie header.  Corrupt Dwarf");
                return DW_DLV_ERROR;
            }
            address_size = *((unsigned char *)frame_ptr);
            if (address_size  <  1) {
                _dwarf_error_string(dbg, error,
                    DW_DLE_ADDRESS_SIZE_ZERO,
                    "DW_DLE_ADDRESS_SIZE_ZERO: bad address size "
                    "for a DWARF4 cie header");
                return DW_DLV_ERROR;
            }
            if (address_size  > sizeof(Dwarf_Addr)) {
                _dwarf_create_address_size_dwarf_error(dbg,
                    error,address_size,
                    DW_DLE_ADDRESS_SIZE_ERROR,
                    "DW_DLE_ADDRESS_SIZE_ERROR..:");
                return DW_DLV_ERROR;
            }
            if ((frame_ptr+2)  >= section_ptr_end) {
                _dwarf_error_string(dbg, error,
                    DW_DLE_DEBUG_FRAME_LENGTH_BAD,
                    "DW_DLE_DEBUG_FRAME_LENGTH_BAD: "
                    "Running off the end "
                    " of a CIE header. Corrupt DWARF4");
                return DW_DLV_ERROR;
            }
            ++frame_ptr;
            segment_size = *((unsigned char *)frame_ptr);
            ++frame_ptr;
            if (segment_size  > sizeof(Dwarf_Addr)) {
                _dwarf_error(dbg, error, DW_DLE_SEGMENT_SIZE_BAD);
                return DW_DLV_ERROR;
            }
        }

        /* Not a great test. But the DECODE* do checking so ok.  */
        if ((frame_ptr+2)  >= section_ptr_end) {
            _dwarf_error_string(dbg, error,
                DW_DLE_DEBUG_FRAME_LENGTH_BAD,
                "DW_DLE_DEBUG_FRAME_LENGTH_BAD: Running off the end "
                " of a CIE header before the code alignment value "
                "read. Corrupt DWARF");
            return DW_DLV_ERROR;
        }
        DECODE_LEB128_UWORD_CK(frame_ptr, lreg,dbg,error,
            section_ptr_end);
        code_alignment_factor = (Dwarf_Unsigned) lreg;
        res = dwarf_decode_signed_leb128(
            (char *)frame_ptr,
            &leb128_length,&data_alignment_factor,
            (char *)section_ptr_end);
        if (res != DW_DLV_OK) {
            return res;
        }
        frame_ptr = frame_ptr + leb128_length;
        if ((frame_ptr+1)  >= section_ptr_end) {
            _dwarf_error_string(dbg, error,
                DW_DLE_DEBUG_FRAME_LENGTH_BAD,
                "DW_DLE_DEBUG_FRAME_LENGTH_BAD: Running off the end "
                "of a CIE header before the return address register "
                "number read. Corrupt DWARF");

            return DW_DLV_ERROR;
        }
        res = _dwarf_get_return_address_reg(frame_ptr, version,
            dbg,section_ptr_end, &size,
            &return_address_register,error);
        if (res != DW_DLV_OK) {
            return res;
        }
        if (return_address_register >
            dbg->de_frame_reg_rules_entry_count) {
            _dwarf_error(dbg, error, DW_DLE_CIE_RET_ADDR_REG_ERROR);
            return DW_DLV_ERROR;
        }
        frame_ptr += size;
        if ((frame_ptr)  > section_ptr_end) {
            _dwarf_error_string(dbg, error,
                DW_DLE_DEBUG_FRAME_LENGTH_BAD,
                "DW_DLE_DEBUG_FRAME_LENGTH_BAD: Past the end "
                "of a CIE header before reading "
                "the augmentation string."
                " Corrupt DWARF");
            return DW_DLV_ERROR;
        }
    }
    switch (augt) {
    case aug_empty_string:
        break;
    case aug_irix_mti_v1:
        break;
    case aug_irix_exception_table:{
        Dwarf_Unsigned lreg = 0;
        Dwarf_Unsigned length_of_augmented_fields;

        /* Decode the length of augmented fields. */
        DECODE_LEB128_UWORD_CK(frame_ptr, lreg,
            dbg,error,section_ptr_end);
        length_of_augmented_fields = (Dwarf_Unsigned) lreg;
        if (length_of_augmented_fields >= dbg->de_filesize) {
            _dwarf_error_string(dbg,error,
                DW_DLE_DEBUG_FRAME_LENGTH_BAD,
                "DW_DLE_DEBUG_FRAME_LENGTH_BAD: "
                "The irix exception table length is too large "
                "to be real");
            return DW_DLV_ERROR;
        }
        /* set the frame_ptr to point at the instruction start. */
        frame_ptr += length_of_augmented_fields;
        }
        break;

    case aug_eh:{
        int err = 0;
        Dwarf_Unsigned increment = 0;

        if (!use_gnu_cie_calc) {
            /* This should be impossible. */
            _dwarf_error(dbg, error,
                DW_DLE_FRAME_AUGMENTATION_UNKNOWN);
            return DW_DLV_ERROR;
        }

        err = _dwarf_get_gcc_eh_augmentation(dbg, frame_ptr,
            &increment,
            augt,
            section_ptr_end,
            (char *) augmentation,error);
        if (err == DW_DLV_ERROR) {
            _dwarf_error(dbg, error,
                DW_DLE_FRAME_AUGMENTATION_UNKNOWN);
            return DW_DLV_ERROR;
        }
        frame_ptr += increment;
        }
        break;
    case aug_gcc_eh_z:{
        /*  Here we have Augmentation Data Length (uleb128) followed
            by Augmentation Data bytes (not a string). */
        int resz = DW_DLV_ERROR;
        Dwarf_Unsigned adlen = 0;

        if ((frame_ptr+1)  > section_ptr_end) {
            _dwarf_error_string(dbg, error,
                DW_DLE_DEBUG_FRAME_LENGTH_BAD,
                "DW_DLE_AUG_DATA_LENGTH_BAD: The "
                "gcc .eh_frame augmentation data "
                "cannot be read. Out of room in the section."
                " Corrupt DWARF.");
            return DW_DLV_ERROR;
        }
        DECODE_LEB128_UWORD_CK(frame_ptr, adlen,
            dbg,error,section_ptr_end);
        cie_aug_data_len = adlen;
        cie_aug_data = frame_ptr;
        if (adlen) {
            Dwarf_Small *cie_aug_data_end = cie_aug_data+adlen;
            if (cie_aug_data_end < cie_aug_data ||
                cie_aug_data_end > section_ptr_end) {
                dwarfstring m;

                dwarfstring_constructor(&m);
                dwarfstring_append_printf_u(&m,
                    "DW_DLE_AUG_DATA_LENGTH_BAD: The "
                    "gcc .eh_frame augmentation data "
                    "length of %" DW_PR_DUu " is too long to"
                    " fit in the section.",adlen);
                _dwarf_error_string(dbg, error,
                    DW_DLE_AUG_DATA_LENGTH_BAD,
                    dwarfstring_string(&m));
                dwarfstring_destructor(&m);
                return DW_DLV_ERROR;
            }
        }
        resz = _dwarf_gnu_aug_encodings(dbg,
            (char *) augmentation,
            cie_aug_data,
            cie_aug_data_len,
            address_size,
            &gnu_personality_handler_encoding,
            &gnu_lsda_encoding,
            &gnu_fde_begin_encoding,
            &gnu_personality_handler_addr,
            error);
        if (resz != DW_DLV_OK) {
            if (resz == DW_DLV_ERROR) {
                _dwarf_error_string(dbg, error,
                    DW_DLE_FRAME_AUGMENTATION_UNKNOWN,
                    "DW_DLE_FRAME_AUGMENTATION_UNKNOWN "
                    " Reading gnu aug encodings failed");
            } /* DW_DLV_NO_ENTRY seems impossible. */
            return resz;
        }
        frame_ptr += adlen;
        }
        break;
    case aug_armcc:
        break;
    default:{
        /*  We do not understand the augmentation string. No
            assumption can be made about any fields other than what
            we have already read. */
        frame_ptr = prefix->cf_start_addr +
            prefix->cf_length + prefix->cf_local_length_size
            + prefix->cf_local_extension_size;
        /*  FIX -- What are the values of data_alignment_factor,
            code_alignment_factor, return_address_register and
            instruction start? They were clearly uninitialized in the
            previous version and I am leaving them the same way. */
        }
        if ((frame_ptr)  > section_ptr_end) {
            _dwarf_error_string(dbg, error,
                DW_DLE_DEBUG_FRAME_LENGTH_BAD,
                "DW_DLE_DEBUG_FRAME_LENGTH_BAD: "
                "Reading an unknown type of augmentation string "
                "run off the end of the section. Corrupt DWARF.");
            return DW_DLV_ERROR;
        }
        break;
    }   /* End switch on augmentation type. */

    new_cie = (Dwarf_Cie) _dwarf_get_alloc(dbg, DW_DLA_CIE, 1);
    if (new_cie == NULL) {
        _dwarf_error_string(dbg, error,
            DW_DLE_ALLOC_FAIL,
            "DW_DLE_ALLOC_FAIL "
            "attempting to allocate a Dwarf_Cie");
        return DW_DLV_ERROR;
    }

    new_cie->ci_cie_version_number = version;
    new_cie->ci_initial_table = NULL;
    new_cie->ci_length = (Dwarf_Unsigned) prefix->cf_length;
    new_cie->ci_length_size =
        (Dwarf_Small)prefix->cf_local_length_size;
    new_cie->ci_extension_size =
        (Dwarf_Small)prefix->cf_local_extension_size;
    new_cie->ci_augmentation = (char *) augmentation;

    new_cie->ci_data_alignment_factor =
        (Dwarf_Sbyte) data_alignment_factor;
    new_cie->ci_code_alignment_factor =
        (Dwarf_Small) code_alignment_factor;
    new_cie->ci_return_address_register = return_address_register;
    new_cie->ci_cie_start = prefix->cf_start_addr;

    if ( frame_ptr > section_ptr_end) {
        _dwarf_error(dbg, error, DW_DLE_DF_FRAME_DECODING_ERROR);
        return DW_DLV_ERROR;
    }
    new_cie->ci_cie_instr_start = frame_ptr;
    new_cie->ci_dbg = dbg;
    new_cie->ci_augmentation_type = augt;
    new_cie->ci_gnu_eh_augmentation_len = cie_aug_data_len;
    new_cie->ci_gnu_eh_augmentation_bytes = cie_aug_data;
    new_cie->ci_gnu_personality_handler_encoding =
        gnu_personality_handler_encoding;
    new_cie->ci_gnu_personality_handler_addr =
        gnu_personality_handler_addr;
    new_cie->ci_gnu_lsda_encoding = gnu_lsda_encoding;
    new_cie->ci_gnu_fde_begin_encoding = gnu_fde_begin_encoding;

    new_cie->ci_index = cie_count;
    new_cie->ci_section_ptr = prefix->cf_section_ptr;
    new_cie->ci_section_end = section_ptr_end;
    new_cie->ci_cie_end = new_cie->ci_cie_start + new_cie->ci_length +
        new_cie->ci_length_size+ new_cie->ci_extension_size;
    if ( new_cie->ci_cie_end > section_ptr_end) {
        dwarf_dealloc(dbg,new_cie,DW_DLA_CIE);
        _dwarf_error(dbg, error, DW_DLE_DF_FRAME_DECODING_ERROR);
        return DW_DLV_ERROR;
    }

    /* The Following new in DWARF4 */
    new_cie->ci_address_size = address_size;
    new_cie->ci_segment_size = segment_size;
    validate_length(dbg,new_cie,new_cie->ci_length,
        new_cie->ci_length_size, new_cie->ci_extension_size,
        new_cie->ci_section_ptr,
        new_cie->ci_cie_start,"cie");
    *cie_ptr_out = new_cie;
    return DW_DLV_OK;
}

/*  Internal function, not called by consumer code.
    'prefix' has accumulated the info up thru the cie-id
    and now we consume the rest and build a Dwarf_Fde_s structure.
    Can be called with cie_ptr_in NULL from dwarf_frame.c  */

int
_dwarf_create_fde_from_after_start(Dwarf_Debug dbg,
    struct cie_fde_prefix_s *prefix,
    Dwarf_Small *section_pointer,
    Dwarf_Unsigned section_length,
    Dwarf_Small *frame_ptr,
    Dwarf_Small *section_ptr_end,
    int          use_gnu_cie_calc,
    Dwarf_Cie    cie_ptr_in,
    Dwarf_Half   address_size,
    Dwarf_Fde   *fde_ptr_out,
    Dwarf_Error *error)
{
    Dwarf_Fde new_fde = 0;
    Dwarf_Cie cieptr = 0;
    Dwarf_Small *saved_frame_ptr = 0;

    Dwarf_Small *initloc = frame_ptr;
    Dwarf_Signed offset_into_exception_tables
        = (Dwarf_Signed) DW_DLX_NO_EH_OFFSET;
    Dwarf_Small *fde_aug_data = 0;
    Dwarf_Unsigned fde_aug_data_len = 0;
    Dwarf_Addr cie_base_offset = prefix->cf_cie_id;
    Dwarf_Addr initial_location = 0;    /* must be min de_pointer_size
        bytes in size */
    Dwarf_Addr address_range = 0;       /* must be min de_pointer_size
        bytes in size */
    Dwarf_Unsigned eh_table_value = 0;
    Dwarf_Bool eh_table_value_set = FALSE;
    /* Temporary assumption.  */
    enum Dwarf_augmentation_type augt = aug_empty_string;

    if (cie_ptr_in) {
        cieptr = cie_ptr_in;
        augt = cieptr->ci_augmentation_type;
    }
    if (augt == aug_gcc_eh_z) {
        /*  If z augmentation this is eh_frame,
            and initial_location and
            address_range in the FDE are read according to the CIE
            augmentation string instructions.  */

        if (cieptr) {
            Dwarf_Small *fp_updated = 0;
            int res = _dwarf_read_encoded_ptr(dbg,
                section_pointer,
                frame_ptr,
                cieptr-> ci_gnu_fde_begin_encoding,
                section_ptr_end,
                address_size,
                &initial_location,
                &fp_updated,error);
            if (res != DW_DLV_OK) {
                return res;
            }
            frame_ptr = fp_updated;
            /*  For the address-range it makes no sense to be
                pc-relative, so we turn it off
                with a section_pointer of
                NULL. Masking off DW_EH_PE_pcrel from the
                ci_gnu_fde_begin_encoding in this
                call would also work
                to turn off DW_EH_PE_pcrel. */
            res = _dwarf_read_encoded_ptr(dbg, (Dwarf_Small *) NULL,
                frame_ptr,
                cieptr->ci_gnu_fde_begin_encoding,
                section_ptr_end,
                address_size,
                &address_range, &fp_updated,error);
            if (res != DW_DLV_OK) {
                return res;
            }
            frame_ptr = fp_updated;
        } /*  We know cieptr was set as was augt, no else needed
            converity scan CID 323429 */
        {
            Dwarf_Unsigned adlen = 0;

            DECODE_LEB128_UWORD_CK(frame_ptr, adlen,
                dbg,error,section_ptr_end);
            fde_aug_data_len = adlen;
            fde_aug_data = frame_ptr;
            if (frame_ptr < section_ptr_end) {
                Dwarf_Unsigned remaininglen = 0;
                remaininglen = (Dwarf_Unsigned)
                    (section_ptr_end - frame_ptr);
                if (remaininglen <= adlen) {
                    _dwarf_error_string(dbg, error,
                        DW_DLE_AUG_DATA_LENGTH_BAD,
                        "DW_DLE_AUG_DATA_LENGTH_BAD: The "
                        "augmentation length is too large for "
                        "the frame section, corrupt DWARF");
                    return DW_DLV_ERROR;
                }
            } else {
                _dwarf_error_string(dbg, error,
                    DW_DLE_AUG_DATA_LENGTH_BAD,
                    "DW_DLE_AUG_DATA_LENGTH_BAD: The "
                    "frame pointer has stepped off the end "
                    "of the frame section on reading augmentation "
                    "length. Corrupt DWARF");
                return DW_DLV_ERROR;
            }
            if ( adlen >= section_length) {
                dwarfstring m;

                dwarfstring_constructor(&m);
                dwarfstring_append_printf_u(&m,
                    "DW_DLE_AUG_DATA_LENGTH_BAD: The "
                    "gcc .eh_frame augmentation data "
                    "length of %" DW_PR_DUu " is too long to"
                    " fit in the section.",adlen);
                _dwarf_error_string(dbg, error,
                    DW_DLE_AUG_DATA_LENGTH_BAD,
                    dwarfstring_string(&m));
                dwarfstring_destructor(&m);
                return DW_DLV_ERROR;
            }
            frame_ptr += adlen;
            if (adlen) {
                if (frame_ptr < fde_aug_data ||
                    frame_ptr >= section_ptr_end ) {
                    dwarfstring m;

                    dwarfstring_constructor(&m);
                    dwarfstring_append_printf_u(&m,
                        "DW_DLE_AUG_DATA_LENGTH_BAD: The "
                        "gcc .eh_frame augmentation data "
                        "length of %" DW_PR_DUu " is too long to"
                        " fit in the section.",adlen);
                    _dwarf_error_string(dbg, error,
                        DW_DLE_AUG_DATA_LENGTH_BAD,
                        dwarfstring_string(&m));
                    dwarfstring_destructor(&m);
                    return DW_DLV_ERROR;
                }
            }
        }
    } else {
        if ((frame_ptr + 2*address_size) > section_ptr_end) {
            _dwarf_error(dbg,error,DW_DLE_DEBUG_FRAME_LENGTH_BAD);
            return DW_DLV_ERROR;
        }
        READ_UNALIGNED_CK(dbg, initial_location, Dwarf_Addr,
            frame_ptr, address_size,
            error,section_ptr_end);
        frame_ptr += address_size;
        READ_UNALIGNED_CK(dbg, address_range, Dwarf_Addr,
            frame_ptr, address_size,
            error,section_ptr_end);
        frame_ptr += address_size;
    }
    switch (augt) {
    case aug_irix_mti_v1:
    case aug_empty_string:
        break;
    case aug_irix_exception_table:{
        Dwarf_Unsigned lreg = 0;
        Dwarf_Unsigned length_of_augmented_fields = 0;

        DECODE_LEB128_UWORD_CK(frame_ptr, lreg,
            dbg,error,section_ptr_end);
        length_of_augmented_fields = (Dwarf_Unsigned) lreg;

        if (length_of_augmented_fields >= dbg->de_filesize) {
            _dwarf_error_string(dbg, error,
                DW_DLE_DEBUG_FRAME_LENGTH_BAD,
                "DW_DLE_DEBUG_FRAME_LENGTH_BAD "
                "in irix exception table length of augmented "
                "fields is too large to be real");
            return DW_DLV_ERROR;
        }
        saved_frame_ptr = frame_ptr;
        /*  The first word is an offset into exception tables.
            Defined as a 32bit offset even for CC -64. */
        if ((frame_ptr + DWARF_32BIT_SIZE) > section_ptr_end) {
            _dwarf_error_string(dbg,error,
                DW_DLE_DEBUG_FRAME_LENGTH_BAD,
                "DW_DLE_DEBUG_FRAME_LENGTH_BAD "
                "irix:frame does not fit in the DWARF section");
            return DW_DLV_ERROR;
        }
        READ_UNALIGNED_CK(dbg, offset_into_exception_tables,
            Dwarf_Addr, frame_ptr, DWARF_32BIT_SIZE,
            error,section_ptr_end);
        SIGN_EXTEND(offset_into_exception_tables,
            DWARF_32BIT_SIZE);
        if (offset_into_exception_tables > 0) {
            if ((Dwarf_Unsigned)offset_into_exception_tables >=
                dbg->de_filesize) {
                _dwarf_error_string(dbg,error,
                    DW_DLE_DEBUG_FRAME_LENGTH_BAD,
                    "DW_DLE_DEBUG_FRAME_LENGTH_BAD "
                    "Irix offset into exception tables");
                return DW_DLV_ERROR;
            }
        } /* nobody uses irix anyway now */
        frame_ptr = saved_frame_ptr + length_of_augmented_fields;
        }
        break;
    case aug_eh:{

        if (!use_gnu_cie_calc) {
            /* This should be impossible. */
            _dwarf_error(dbg, error,
                DW_DLE_FRAME_AUGMENTATION_UNKNOWN);
            return DW_DLV_ERROR;
        }

        /* gnu eh fde case. we do not need to do anything */
        /*REFERENCED*/ /* Not used in this instance of the macro */
        if ((frame_ptr + address_size) > section_ptr_end) {
            _dwarf_error(dbg,error,DW_DLE_DEBUG_FRAME_LENGTH_BAD);
            return DW_DLV_ERROR;
        }
        READ_UNALIGNED_CK(dbg, eh_table_value,
            Dwarf_Unsigned, frame_ptr,
            address_size,
            error,section_ptr_end);
        eh_table_value_set = TRUE;
        frame_ptr += address_size;
        }
        break;

    case aug_gcc_eh_z:{
        /*  The Augmentation Data Length is here, followed by the
            Augmentation Data bytes themselves. */
        }
        break;
    case aug_armcc:
        break;
    case aug_past_last:
        break;

    case aug_metaware: /* No special fields. See dwarf_util.h */
        break;

    case aug_unknown:
        _dwarf_error(dbg, error, DW_DLE_FRAME_AUGMENTATION_UNKNOWN);
        return DW_DLV_ERROR;
    default: break;
    }                           /* End switch on augmentation type */
    if ( frame_ptr > section_ptr_end) {
        _dwarf_error(dbg, error, DW_DLE_DF_FRAME_DECODING_ERROR);
        return DW_DLV_ERROR;
    }
    if ( frame_ptr < initloc) {
        _dwarf_error_string(dbg, error,
            DW_DLE_DF_FRAME_DECODING_ERROR,
            "DW_DLE_DF_FRAME_DECODING_ERROR "
            "frame pointer decreased.Impossible. "
            "arithmetic overflow");
        return DW_DLV_ERROR;
    }

    new_fde = (Dwarf_Fde) _dwarf_get_alloc(dbg, DW_DLA_FDE, 1);
    if (new_fde == NULL) {
        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
        return DW_DLV_ERROR;
    }

    new_fde->fd_length = prefix->cf_length;
    new_fde->fd_length_size =
        (Dwarf_Small)prefix->cf_local_length_size;
    new_fde->fd_extension_size =
        (Dwarf_Small)prefix->cf_local_extension_size;
    new_fde->fd_is_eh = (Dwarf_Small)use_gnu_cie_calc;
    new_fde->fd_cie_offset = cie_base_offset;
    if (cieptr) {
        new_fde->fd_cie_index = cieptr->ci_index;
    }
    new_fde->fd_cie = cieptr;
    new_fde->fd_initial_location = initial_location;
    new_fde->fd_initial_loc_pos = initloc;
    new_fde->fd_address_range = address_range;
    new_fde->fd_fde_start = prefix->cf_start_addr;

    new_fde->fd_fde_instr_start = frame_ptr;
    new_fde->fd_fde_end = prefix->cf_start_addr +
        prefix->cf_length +  prefix->cf_local_length_size  +
        prefix->cf_local_extension_size;
    if ( new_fde->fd_fde_end > section_ptr_end) {
        _dwarf_error(dbg, error, DW_DLE_DF_FRAME_DECODING_ERROR);
        dwarf_dealloc(dbg,new_fde,DW_DLA_FDE);
        return DW_DLV_ERROR;
    }

    new_fde->fd_dbg = dbg;
    new_fde->fd_offset_into_exception_tables =
        offset_into_exception_tables;
    new_fde->fd_eh_table_value = eh_table_value;
    new_fde->fd_eh_table_value_set = eh_table_value_set;

    new_fde->fd_section_ptr = prefix->cf_section_ptr;
    new_fde->fd_section_index = prefix->cf_section_index;
    new_fde->fd_section_length = prefix->cf_section_length;
    new_fde->fd_section_end = section_ptr_end;

    if (augt == aug_gcc_eh_z) {
        new_fde->fd_gnu_eh_aug_present = TRUE;
    }
    new_fde->fd_gnu_eh_augmentation_bytes = fde_aug_data;
    new_fde->fd_gnu_eh_augmentation_len = fde_aug_data_len;
    validate_length(dbg,cieptr,new_fde->fd_length,
        new_fde->fd_length_size, new_fde->fd_extension_size,
        new_fde->fd_section_ptr,new_fde->fd_fde_start,"fde");
    *fde_ptr_out = new_fde;
    return DW_DLV_OK;
}

/*  Read in the common cie/fde prefix, including reading
    the cie-value which shows which this is: cie or fde.  */
int
_dwarf_read_cie_fde_prefix(Dwarf_Debug dbg,
    Dwarf_Small * frame_ptr_in,
    Dwarf_Small * section_ptr_in,
    Dwarf_Unsigned section_index_in,
    Dwarf_Unsigned section_length_in,
    struct cie_fde_prefix_s *data_out,
    Dwarf_Error * error)
{
    Dwarf_Unsigned length = 0;
    int local_length_size = 0;
    int local_extension_size = 0;
    Dwarf_Small *frame_ptr = frame_ptr_in;
    Dwarf_Small *cie_ptr_addr = 0;
    Dwarf_Unsigned cie_id = 0;
    Dwarf_Small *section_end = section_ptr_in + section_length_in;

    if (frame_ptr_in < section_ptr_in ||
        frame_ptr_in >= section_end) {
        _dwarf_error_string(dbg,error,DW_DLE_DEBUG_FRAME_LENGTH_BAD,
            "DW_DLE_DEBUG_FRAME_LENGTH_BAD: "
            "The frame point given _dwarf_read_cie_fde_prefix() "
            "is invalid");
        return DW_DLV_ERROR;
    }
    if (section_end < (frame_ptr +4)) {
        dwarfstring m;
        Dwarf_Unsigned u =
            (Dwarf_Unsigned)(uintptr_t)(frame_ptr+4) -
            (Dwarf_Unsigned)(uintptr_t)section_end;

        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            "DW_DLE_DEBUG_FRAME_LENGTH_BAD: "
            "Reading the cie/fde prefix would "
            "put us %u bytes past the end of the "
            "frame section.  Corrupt Dwarf.",u);
        _dwarf_error_string(dbg,error,DW_DLE_DEBUG_FRAME_LENGTH_BAD,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    /* READ_AREA_LENGTH updates frame_ptr for consumed bytes */
    READ_AREA_LENGTH_CK(dbg, length, Dwarf_Unsigned,
        frame_ptr, local_length_size,
        local_extension_size,error,
        section_length_in,section_end);
    if (length == 0) {
        /*  nul bytes at end of section, seen at end of egcs eh_frame
            sections (in a.out). Take this as meaning no more CIE/FDE
            data. We should be very close to end of section. */
        return DW_DLV_NO_ENTRY;
    }
    if (length >= dbg->de_filesize) {
        _dwarf_error(dbg,error,DW_DLE_DEBUG_FRAME_LENGTH_BAD);
        return DW_DLV_ERROR;
    }
    if (length > section_length_in ||
        (length +local_length_size + local_extension_size) >
        section_length_in) {
        _dwarf_error(dbg,error,DW_DLE_DEBUG_FRAME_LENGTH_BAD);
        return DW_DLV_ERROR;
    }
    if ((frame_ptr + local_length_size) >= section_end) {
        _dwarf_error(dbg,error,DW_DLE_DEBUG_FRAME_LENGTH_BAD);
        return DW_DLV_ERROR;
    }

    cie_ptr_addr = frame_ptr;
    READ_UNALIGNED_CK(dbg, cie_id, Dwarf_Unsigned,
        frame_ptr, local_length_size,error,section_end);
    SIGN_EXTEND(cie_id, local_length_size);
    frame_ptr += local_length_size;

    data_out->cf_start_addr = frame_ptr_in;
    data_out->cf_addr_after_prefix = frame_ptr;

    data_out->cf_length = length;
    if (length > section_length_in) {
        _dwarf_error(dbg,error,DW_DLE_DEBUG_FRAME_LENGTH_BAD);
        return DW_DLV_ERROR;
    }
    if (cie_ptr_addr+length > section_end) {
        _dwarf_error(dbg,error,DW_DLE_DEBUG_FRAME_LENGTH_BAD);
        return DW_DLV_ERROR;
    }
    data_out->cf_local_length_size = local_length_size;
    data_out->cf_local_extension_size = local_extension_size;

    /*  We do not know if it is a CIE or FDE id yet.
        How we check and what it means
        depends whether it is .debug_frame
        or .eh_frame. */
    data_out->cf_cie_id = cie_id;

    /*  The address of the CIE_id  or FDE_id value in memory.  */
    data_out->cf_cie_id_addr = cie_ptr_addr;

    data_out->cf_section_ptr = section_ptr_in;
    data_out->cf_section_index = section_index_in;
    data_out->cf_section_length = section_length_in;
    return DW_DLV_OK;
}

/*  On various errors previously-allocated CIEs and FDEs
    must be cleaned up.
    This helps avoid leaks in case of errors.
*/
static void
_dwarf_dealloc_fde_cie_list_internal(Dwarf_Fde head_fde_ptr,
    Dwarf_Cie head_cie_ptr)
{
    Dwarf_Fde curfde = 0;
    Dwarf_Cie curcie = 0;
    Dwarf_Fde nextfde = 0;
    Dwarf_Cie nextcie = 0;

    for (curfde = head_fde_ptr; curfde; curfde = nextfde) {
        nextfde = curfde->fd_next;
        dwarf_dealloc(curfde->fd_dbg, curfde, DW_DLA_FDE);
    }
    for (curcie = head_cie_ptr; curcie; curcie = nextcie) {
        Dwarf_Frame frame = curcie->ci_initial_table;

        nextcie = curcie->ci_next;
        if (frame)
            dwarf_dealloc(curcie->ci_dbg, frame, DW_DLA_FRAME);
        dwarf_dealloc(curcie->ci_dbg, curcie, DW_DLA_CIE);
    }
}

/*  Find the cie whose id value is given: the id
    value is, per DWARF2/3, an offset in the section.
    For .debug_frame, zero is a legal offset. For
    GNU .eh_frame it is not a legal offset.
    'cie_ptr' is a pointer into our section, not an offset. */
static int
_dwarf_find_existing_cie_ptr(Dwarf_Small * cie_ptr,
    Dwarf_Cie cur_cie_ptr,
    Dwarf_Cie * cie_ptr_to_use_out,
    Dwarf_Cie head_cie_ptr)
{
    Dwarf_Cie next = 0;

    if (cur_cie_ptr && cie_ptr == cur_cie_ptr->ci_cie_start) {
        /* Usually, we use the same cie again and again. */
        *cie_ptr_to_use_out = cur_cie_ptr;
        return DW_DLV_OK;
    }
    for (next = head_cie_ptr; next; next = next->ci_next) {
        if (cie_ptr == next->ci_cie_start) {
            *cie_ptr_to_use_out = next;
            return DW_DLV_OK;
        }
    }
    return DW_DLV_NO_ENTRY;
}

/*  We have a valid cie_ptr_val that has not been
    turned into an internal Cie yet. Do so now.
    Returns DW_DLV_OK or DW_DLV_ERROR, never
    DW_DLV_NO_ENTRY.

    'section_ptr'    - Points to first byte of section data.
    'section_length' - Length of the section, in bytes.
    'section_ptr_end'  - Points 1-past last byte of section data.  */
static int
_dwarf_create_cie_from_start(Dwarf_Debug dbg,
    Dwarf_Small * cie_ptr_val,
    Dwarf_Small * section_ptr,
    Dwarf_Unsigned section_index,
    Dwarf_Unsigned section_length,
    Dwarf_Small * section_ptr_end,
    Dwarf_Unsigned cie_id_value,
    Dwarf_Unsigned cie_count,
    int use_gnu_cie_calc,
    Dwarf_Cie * cie_ptr_to_use_out,
    Dwarf_Error * error)
{
    struct cie_fde_prefix_s prefix;
    int res = DW_DLV_ERROR;
    Dwarf_Small *frame_ptr = cie_ptr_val;

    if (frame_ptr < section_ptr || frame_ptr >= section_ptr_end) {
        _dwarf_error(dbg, error, DW_DLE_DEBUG_FRAME_LENGTH_BAD);
        return DW_DLV_ERROR;
    }
    /*  First read in the 'common prefix' to figure out
        what * we are to
        do with this entry. If it is not a cie *
        we are in big trouble. */
    memset(&prefix, 0, sizeof(prefix));
    res = _dwarf_read_cie_fde_prefix(dbg, frame_ptr, section_ptr,
        section_index, section_length,
        &prefix, error);
    if (res == DW_DLV_ERROR) {
        return res;
    }
    if (res == DW_DLV_NO_ENTRY) {
        /* error. */
        _dwarf_error(dbg, error, DW_DLE_FRAME_CIE_DECODE_ERROR);
        return DW_DLV_ERROR;

    }

    if (prefix.cf_cie_id != cie_id_value) {
        _dwarf_error(dbg, error, DW_DLE_FRAME_CIE_DECODE_ERROR);
        return DW_DLV_ERROR;
    }
    frame_ptr = prefix.cf_addr_after_prefix;
    res = _dwarf_create_cie_from_after_start(dbg,
        &prefix,
        section_ptr,
        frame_ptr,
        section_ptr_end,
        cie_count,
        use_gnu_cie_calc,
        cie_ptr_to_use_out, error);
    return res;

}

/*  This is for gnu eh frames, the 'z' case.
    We find the letter involved
    Return the augmentation character and, if applicable,
    the personality routine address.

    personality_routine_out -
        if 'P' is augchar, is personality handler addr.
        Otherwise is not set.
    aug_data  - if 'P' points  to data space of the
    aug_data_len - length of areas aug_data points to.
*/

/*  It is not clear if this is entirely correct. */
static int
_dwarf_gnu_aug_encodings(Dwarf_Debug dbg, char *augmentation,
    Dwarf_Small * aug_data, Dwarf_Unsigned aug_data_len,
    Dwarf_Half address_size,
    unsigned char *pers_hand_enc_out,
    unsigned char *lsda_enc_out,
    unsigned char *fde_begin_enc_out,
    Dwarf_Addr * gnu_pers_addr_out,
    Dwarf_Error * error)
{
    char *nc = 0;
    Dwarf_Small *cur_aug_p = aug_data;
    Dwarf_Small *end_aug_p = aug_data + aug_data_len;

    for (nc = augmentation; *nc; ++nc) {
        char c = *nc;

        switch (c) {
        case 'z':
            /* Means that the augmentation data is present. */
            continue;

        case 'S':
            /*  Indicates this is a signal stack frame.
                Debuggers have to do
                special handling.  We don't need to do more than
                print this flag at the right time, though
                (see dwarfdump where it prints the augmentation
                string).
                A signal stack frame (in some OS's) can only be
                unwound (backtraced) by knowing it is a signal
                stack frame (perhaps by noticing the name of the
                function for the stack frame if the name can be
                found somehow) and figuring
                out (or knowing) how the kernel and libc
                pushed a structure
                onto the stack and loading registers from
                that structure.
                Totally different from normal stack unwinding.
                This flag gives an unwinder a big leg up by
                decoupling the 'hint: this is a stack frame'
                from knowledge like
                the function name (the name might be
                unavailable at unwind time).
            */
            break;

        case 'L':
            if (cur_aug_p >= end_aug_p) {
                _dwarf_error_string(dbg, error,
                    DW_DLE_FRAME_AUGMENTATION_UNKNOWN,
                    "DW_DLE_FRAME_AUGMENTATION_UNKNOWN: "
                    " Augmentation L runs off the end"
                    " of augmentation bytes");
                return DW_DLV_ERROR;
            }
            *lsda_enc_out = *(unsigned char *) cur_aug_p;
            ++cur_aug_p;
            break;
        case 'R':
            /*  Followed by a one byte argument giving the
                pointer encoding for the address
                pointers in the fde. */
            if (cur_aug_p >= end_aug_p) {
                _dwarf_error(dbg, error,
                    DW_DLE_FRAME_AUGMENTATION_UNKNOWN);
                return DW_DLV_ERROR;
            }
            *fde_begin_enc_out = *(unsigned char *) cur_aug_p;
            ++cur_aug_p;
            break;
        case 'P':{
            int res = DW_DLV_ERROR;
            Dwarf_Small *updated_aug_p = 0;
            unsigned char encoding = 0;

            if (cur_aug_p >= end_aug_p) {
                _dwarf_error(dbg, error,
                    DW_DLE_FRAME_AUGMENTATION_UNKNOWN);
                return DW_DLV_ERROR;
            }
            encoding = *(unsigned char *) cur_aug_p;
            *pers_hand_enc_out = encoding;
            ++cur_aug_p;
            if (cur_aug_p > end_aug_p) {
                _dwarf_error(dbg, error,
                    DW_DLE_FRAME_AUGMENTATION_UNKNOWN);
                return DW_DLV_ERROR;
            }
            /*  DW_EH_PE_pcrel makes no sense here, so we turn it
                off via a section pointer of NULL. */
            res = _dwarf_read_encoded_ptr(dbg,
                (Dwarf_Small *) NULL,
                cur_aug_p,
                encoding,
                end_aug_p,
                address_size,
                gnu_pers_addr_out,
                &updated_aug_p,
                error);
            if (res != DW_DLV_OK) {
                return res;
            }
            cur_aug_p = updated_aug_p;
            if (cur_aug_p > end_aug_p) {
                _dwarf_error(dbg, error,
                    DW_DLE_FRAME_AUGMENTATION_UNKNOWN);
                return DW_DLV_ERROR;
            }
            }
            break;
        default:
            _dwarf_error(dbg, error,
                DW_DLE_FRAME_AUGMENTATION_UNKNOWN);
            return DW_DLV_ERROR;

        }
    }
    return DW_DLV_OK;
}

/*  Given augmentation character (the encoding) giving the
    address format, read the address from input_field
    and return an incremented value 1 past the input bytes of the
    address.
    Push the address read back thru the *addr pointer.
    See LSB (Linux Standard Base)  exception handling documents.  */
static int
_dwarf_read_encoded_ptr(Dwarf_Debug dbg,
    Dwarf_Small * section_pointer,
    Dwarf_Small * input_field,
    int gnu_encoding,
    Dwarf_Small * section_end,
    Dwarf_Half address_size,
    Dwarf_Unsigned * addr,
    Dwarf_Small ** input_field_updated,
    Dwarf_Error *error)
{
    int value_type = gnu_encoding & 0xf;
    Dwarf_Small *input_field_original = input_field;

    if (gnu_encoding == 0xff) {
        /* There is no data here. */

        *addr = 0;
        *input_field_updated = input_field;
        /* Should we return DW_DLV_NO_ENTRY? */
        return DW_DLV_OK;
    }
    switch (value_type) {
    case DW_EH_PE_absptr:{
        /* value_type is zero. Treat as pointer size of the object.
        */
        Dwarf_Unsigned ret_value = 0;

        READ_UNALIGNED_CK(dbg, ret_value, Dwarf_Unsigned,
            input_field, address_size,error,section_end);
        *addr = ret_value;
        *input_field_updated = input_field + address_size;
        }
        break;
    case DW_EH_PE_uleb128:{
        Dwarf_Unsigned val = 0;

        DECODE_LEB128_UWORD_CK(input_field,val,dbg,error,section_end);
        *addr = val;
        *input_field_updated = input_field;
        }
        break;
    case DW_EH_PE_udata2:{
        Dwarf_Unsigned ret_value = 0;

        READ_UNALIGNED_CK(dbg, ret_value, Dwarf_Unsigned,
            input_field, 2,error,section_end);
        *addr = ret_value;
        *input_field_updated = input_field + 2;
        }
        break;

    case DW_EH_PE_udata4:{
        Dwarf_Unsigned ret_value = 0;

        READ_UNALIGNED_CK(dbg, ret_value, Dwarf_Unsigned,
            input_field, DWARF_32BIT_SIZE,error,section_end);
        *addr = ret_value;
        *input_field_updated = input_field + DWARF_32BIT_SIZE;
        }
        break;

    case DW_EH_PE_udata8:{
        Dwarf_Unsigned ret_value = 0;

        /* ASSERT: sizeof(Dwarf_Unsigned) == 8 */
        READ_UNALIGNED_CK(dbg, ret_value, Dwarf_Unsigned,
            input_field, DWARF_64BIT_SIZE,error,section_end);
        *addr = ret_value;
        *input_field_updated = input_field +  DWARF_64BIT_SIZE;
        }
        break;

    case DW_EH_PE_sleb128:{
        Dwarf_Signed val = 0;

        DECODE_LEB128_SWORD_CK(input_field,val,dbg,error,section_end);
        *addr = (Dwarf_Unsigned) val;
        *input_field_updated = input_field;
        }
        break;
    case DW_EH_PE_sdata2:{
        Dwarf_Unsigned val = 0;

        READ_UNALIGNED_CK(dbg, val, Dwarf_Unsigned, input_field, 2,
            error,section_end);
        SIGN_EXTEND(val, 2);
        *addr = (Dwarf_Unsigned) val;
        *input_field_updated = input_field + 2;
        }
        break;

    case DW_EH_PE_sdata4:{
        Dwarf_Unsigned val = 0;

        READ_UNALIGNED_CK(dbg, val,
            Dwarf_Unsigned, input_field,
            DWARF_32BIT_SIZE,error,section_end);
        SIGN_EXTEND(val, DWARF_32BIT_SIZE);
        *addr = (Dwarf_Unsigned) val;
        *input_field_updated = input_field + DWARF_32BIT_SIZE;
        }
        break;
    case DW_EH_PE_sdata8:{
        Dwarf_Unsigned val = 0;

        /* ASSERT: sizeof(Dwarf_Unsigned) == 8 */
        READ_UNALIGNED_CK(dbg, val,
            Dwarf_Unsigned, input_field,
            DWARF_64BIT_SIZE,error,section_end);
        *addr = (Dwarf_Unsigned) val;
        *input_field_updated = input_field + DWARF_64BIT_SIZE;
        }
        break;
    default:
        _dwarf_error(dbg, error, DW_DLE_FRAME_AUGMENTATION_UNKNOWN);
        return DW_DLV_ERROR;

    };
    /*  The ELF ABI for gnu does not document the meaning of
        DW_EH_PE_pcrel, which is awkward.
        It apparently means the value
        we got above is pc-relative (meaning section-relative),
        so we adjust the value. Section_pointer may be null
        if it is known DW_EH_PE_pcrel cannot apply,
        such as for .debug_frame or for an
        address-range value. */
    if (section_pointer && ((gnu_encoding & 0x70) ==
        DW_EH_PE_pcrel)) {
        /*  Address (*addr) above is pc relative with respect to a
            section. Add to the offset the base address (from elf) of
            section and the distance of the field we are reading from
            the section-beginning to get the actual address. */
        /*  ASSERT: input_field_original >= section_pointer */
        Dwarf_Unsigned distance =
            input_field_original - section_pointer;
        *addr += dbg->de_debug_frame_eh_gnu.dss_addr + distance;
    }
    return DW_DLV_OK;
}

/*  All augmentation string checking done here now.

    For .eh_frame, gcc from 3.3 uses the z style, earlier used
    only "eh" as augmentation.  We don't yet handle
    decoding .eh_frame with the z style extensions like L P.
    _dwarf_gnu_aug_encodings() does handle L P.

    These are nasty heuristics, but then that's life
    as augmentations are implementation specific.  */
/* ARGSUSED */
enum Dwarf_augmentation_type
_dwarf_get_augmentation_type(Dwarf_Debug dbg,
    Dwarf_Small * augmentation_string,
    int is_gcc_eh_frame)
{
    enum Dwarf_augmentation_type t = aug_unknown;
    char *ag_string = (char *) augmentation_string;

    (void)dbg;
    if (!ag_string[0]) {
        /*  Empty string. We'll just guess that we know
            what this means:
            standard dwarf2/3 with no
            implementation-defined fields.  */
        t = aug_empty_string;
    } else if (!strcmp(ag_string, DW_DEBUG_FRAME_AUGMENTER_STRING)) {
        /*  The string is "mti v1". Used internally at SGI, probably
            never shipped. Replaced by "z". Treat like 'nothing
            special'.  */
        t = aug_irix_mti_v1;
    } else if (ag_string[0] == 'z') {
        /*  If it's IRIX cc, z means aug_irix_exception_table. z1 z2
            were designed as for IRIX CC, but never implemented */
        /*  If it's gcc, z may be any of several things. "z" or z
            followed optionally followed by one or more of L R P,
            each of which means a value may be present.
            Should be in eh_frame
            only, I think. */
        if (is_gcc_eh_frame) {
            t = aug_gcc_eh_z;
        } else if (!ag_string[1]) {
            /*  This is the normal IRIX C++ case, where there is an
                offset into a table in each fde. The table being for
                IRIX CC exception handling.  */
            /*  DW_CIE_AUGMENTER_STRING_V0 "z" */
            t = aug_irix_exception_table;
        }                       /* Else unknown. */
    } else if (!strncmp(ag_string, "eh", 2)) {
        /*  gcc .eh_frame augmentation for egcs and gcc 2.x, at least
            for x86. */
        t = aug_eh;
    } else if (!strcmp(ag_string, "armcc+")) {
        /*  Arm  uses this string to mean a bug in
            in Arm compilers was fixed, changing to the standard
            calculation of the CFA.  See
            http://sourceware.org/ml/gdb-patches/
            2006-12/msg00249.html
            for details. */
        t = aug_armcc;
    } else if (!strcmp(ag_string, "HC")) {
        t = aug_metaware;
    } else {
    }
    return t;
}

/*  Using augmentation, and version
    read in the augmentation data for GNU eh.

    Return DW_DLV_OK if we succeeded,
    DW_DLV_ERR if we fail.

    On success, update  'size_of_augmentation_data' with
    the length of the fields that are part of augmentation (so the
    caller can increment frame_ptr appropriately).

    'frame_ptr' points within section.
    'section_end' points to end of section area of interest.

*/
/* ARGSUSED */
static int
_dwarf_get_gcc_eh_augmentation(Dwarf_Debug dbg,
    Dwarf_Small * frame_ptr,
    Dwarf_Unsigned *size_of_augmentation_data,
    enum Dwarf_augmentation_type augtype,
    Dwarf_Small * section_ptr_end, char *augmentation,
    Dwarf_Error *error)
{
    char *suffix = 0;
    Dwarf_Unsigned augdata_size = 0;

    if (augtype == aug_gcc_eh_z) {
        /* Has leading 'z'. */
        Dwarf_Unsigned leb128_length = 0;

        /* Dwarf_Unsigned eh_value = */
        SKIP_LEB128_LEN_CK(frame_ptr,leb128_length,
            dbg,error,section_ptr_end);
        augdata_size += leb128_length;
        suffix = augmentation + 1;
    } else {
        /*  Prefix is 'eh'.  As in gcc 3.2. No suffix present
            apparently. */
        suffix = augmentation + 2;
    }
    if (*suffix) {
        /*  We have no idea what this is as yet.
            Some extensions beyond
            dwarf exist which we do not yet handle. */
        _dwarf_error(dbg, error, DW_DLE_FRAME_AUGMENTATION_UNKNOWN);
        return DW_DLV_ERROR;

    }

    *size_of_augmentation_data = augdata_size;
    return DW_DLV_OK;
}

/* To properly release all spaced used.
   Earlier approaches (before July 15, 2005)
   letting client do the dealloc directly left
   some data allocated.
   This is directly called by consumer code.
*/
void
dwarf_dealloc_fde_cie_list(Dwarf_Debug dbg,
    Dwarf_Cie * cie_data,
    Dwarf_Signed cie_element_count,
    Dwarf_Fde * fde_data,
    Dwarf_Signed fde_element_count)
{
    Dwarf_Signed i = 0;

    for (i = 0; i < cie_element_count; ++i) {
        Dwarf_Frame frame = cie_data[i]->ci_initial_table;

        if (frame) {
            dwarf_dealloc(dbg, frame, DW_DLA_FRAME);
        }
        dwarf_dealloc(dbg, cie_data[i], DW_DLA_CIE);
    }
    for (i = 0; i < fde_element_count; ++i) {
        dwarf_dealloc(dbg, fde_data[i], DW_DLA_FDE);
    }
    if (cie_data) {
        dwarf_dealloc(dbg, cie_data, DW_DLA_LIST);
    }
    if (fde_data) {
        dwarf_dealloc(dbg, fde_data, DW_DLA_LIST);
    }
}
