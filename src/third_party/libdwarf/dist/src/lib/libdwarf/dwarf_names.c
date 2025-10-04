/* Generated routines, do not edit. */
/* Generated for source version 2.1.0 */

/* BEGIN FILE */

#include "dwarf.h"

#include "libdwarf.h"

/* ARGSUSED */
int
dwarf_get_TAG_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_TAG_array_type:
        *s_out = "DW_TAG_array_type";
        return DW_DLV_OK;
    case DW_TAG_class_type:
        *s_out = "DW_TAG_class_type";
        return DW_DLV_OK;
    case DW_TAG_entry_point:
        *s_out = "DW_TAG_entry_point";
        return DW_DLV_OK;
    case DW_TAG_enumeration_type:
        *s_out = "DW_TAG_enumeration_type";
        return DW_DLV_OK;
    case DW_TAG_formal_parameter:
        *s_out = "DW_TAG_formal_parameter";
        return DW_DLV_OK;
    case DW_TAG_imported_declaration:
        *s_out = "DW_TAG_imported_declaration";
        return DW_DLV_OK;
    case DW_TAG_label:
        *s_out = "DW_TAG_label";
        return DW_DLV_OK;
    case DW_TAG_lexical_block:
        *s_out = "DW_TAG_lexical_block";
        return DW_DLV_OK;
    case DW_TAG_member:
        *s_out = "DW_TAG_member";
        return DW_DLV_OK;
    case DW_TAG_pointer_type:
        *s_out = "DW_TAG_pointer_type";
        return DW_DLV_OK;
    case DW_TAG_reference_type:
        *s_out = "DW_TAG_reference_type";
        return DW_DLV_OK;
    case DW_TAG_compile_unit:
        *s_out = "DW_TAG_compile_unit";
        return DW_DLV_OK;
    case DW_TAG_string_type:
        *s_out = "DW_TAG_string_type";
        return DW_DLV_OK;
    case DW_TAG_structure_type:
        *s_out = "DW_TAG_structure_type";
        return DW_DLV_OK;
    case DW_TAG_subroutine_type:
        *s_out = "DW_TAG_subroutine_type";
        return DW_DLV_OK;
    case DW_TAG_typedef:
        *s_out = "DW_TAG_typedef";
        return DW_DLV_OK;
    case DW_TAG_union_type:
        *s_out = "DW_TAG_union_type";
        return DW_DLV_OK;
    case DW_TAG_unspecified_parameters:
        *s_out = "DW_TAG_unspecified_parameters";
        return DW_DLV_OK;
    case DW_TAG_variant:
        *s_out = "DW_TAG_variant";
        return DW_DLV_OK;
    case DW_TAG_common_block:
        *s_out = "DW_TAG_common_block";
        return DW_DLV_OK;
    case DW_TAG_common_inclusion:
        *s_out = "DW_TAG_common_inclusion";
        return DW_DLV_OK;
    case DW_TAG_inheritance:
        *s_out = "DW_TAG_inheritance";
        return DW_DLV_OK;
    case DW_TAG_inlined_subroutine:
        *s_out = "DW_TAG_inlined_subroutine";
        return DW_DLV_OK;
    case DW_TAG_module:
        *s_out = "DW_TAG_module";
        return DW_DLV_OK;
    case DW_TAG_ptr_to_member_type:
        *s_out = "DW_TAG_ptr_to_member_type";
        return DW_DLV_OK;
    case DW_TAG_set_type:
        *s_out = "DW_TAG_set_type";
        return DW_DLV_OK;
    case DW_TAG_subrange_type:
        *s_out = "DW_TAG_subrange_type";
        return DW_DLV_OK;
    case DW_TAG_with_stmt:
        *s_out = "DW_TAG_with_stmt";
        return DW_DLV_OK;
    case DW_TAG_access_declaration:
        *s_out = "DW_TAG_access_declaration";
        return DW_DLV_OK;
    case DW_TAG_base_type:
        *s_out = "DW_TAG_base_type";
        return DW_DLV_OK;
    case DW_TAG_catch_block:
        *s_out = "DW_TAG_catch_block";
        return DW_DLV_OK;
    case DW_TAG_const_type:
        *s_out = "DW_TAG_const_type";
        return DW_DLV_OK;
    case DW_TAG_constant:
        *s_out = "DW_TAG_constant";
        return DW_DLV_OK;
    case DW_TAG_enumerator:
        *s_out = "DW_TAG_enumerator";
        return DW_DLV_OK;
    case DW_TAG_file_type:
        *s_out = "DW_TAG_file_type";
        return DW_DLV_OK;
    case DW_TAG_friend:
        *s_out = "DW_TAG_friend";
        return DW_DLV_OK;
    case DW_TAG_namelist:
        *s_out = "DW_TAG_namelist";
        return DW_DLV_OK;
    case DW_TAG_namelist_item:
        *s_out = "DW_TAG_namelist_item";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x2c. DW_TAG_namelist_items */
    case DW_TAG_packed_type:
        *s_out = "DW_TAG_packed_type";
        return DW_DLV_OK;
    case DW_TAG_subprogram:
        *s_out = "DW_TAG_subprogram";
        return DW_DLV_OK;
    case DW_TAG_template_type_parameter:
        *s_out = "DW_TAG_template_type_parameter";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x2f. DW_TAG_template_type_param */
    case DW_TAG_template_value_parameter:
        *s_out = "DW_TAG_template_value_parameter";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x30. DW_TAG_template_value_param */
    case DW_TAG_thrown_type:
        *s_out = "DW_TAG_thrown_type";
        return DW_DLV_OK;
    case DW_TAG_try_block:
        *s_out = "DW_TAG_try_block";
        return DW_DLV_OK;
    case DW_TAG_variant_part:
        *s_out = "DW_TAG_variant_part";
        return DW_DLV_OK;
    case DW_TAG_variable:
        *s_out = "DW_TAG_variable";
        return DW_DLV_OK;
    case DW_TAG_volatile_type:
        *s_out = "DW_TAG_volatile_type";
        return DW_DLV_OK;
    case DW_TAG_dwarf_procedure:
        *s_out = "DW_TAG_dwarf_procedure";
        return DW_DLV_OK;
    case DW_TAG_restrict_type:
        *s_out = "DW_TAG_restrict_type";
        return DW_DLV_OK;
    case DW_TAG_interface_type:
        *s_out = "DW_TAG_interface_type";
        return DW_DLV_OK;
    case DW_TAG_namespace:
        *s_out = "DW_TAG_namespace";
        return DW_DLV_OK;
    case DW_TAG_imported_module:
        *s_out = "DW_TAG_imported_module";
        return DW_DLV_OK;
    case DW_TAG_unspecified_type:
        *s_out = "DW_TAG_unspecified_type";
        return DW_DLV_OK;
    case DW_TAG_partial_unit:
        *s_out = "DW_TAG_partial_unit";
        return DW_DLV_OK;
    case DW_TAG_imported_unit:
        *s_out = "DW_TAG_imported_unit";
        return DW_DLV_OK;
    case DW_TAG_mutable_type:
        *s_out = "DW_TAG_mutable_type";
        return DW_DLV_OK;
    case DW_TAG_condition:
        *s_out = "DW_TAG_condition";
        return DW_DLV_OK;
    case DW_TAG_shared_type:
        *s_out = "DW_TAG_shared_type";
        return DW_DLV_OK;
    case DW_TAG_type_unit:
        *s_out = "DW_TAG_type_unit";
        return DW_DLV_OK;
    case DW_TAG_rvalue_reference_type:
        *s_out = "DW_TAG_rvalue_reference_type";
        return DW_DLV_OK;
    case DW_TAG_template_alias:
        *s_out = "DW_TAG_template_alias";
        return DW_DLV_OK;
    case DW_TAG_coarray_type:
        *s_out = "DW_TAG_coarray_type";
        return DW_DLV_OK;
    case DW_TAG_generic_subrange:
        *s_out = "DW_TAG_generic_subrange";
        return DW_DLV_OK;
    case DW_TAG_dynamic_type:
        *s_out = "DW_TAG_dynamic_type";
        return DW_DLV_OK;
    case DW_TAG_atomic_type:
        *s_out = "DW_TAG_atomic_type";
        return DW_DLV_OK;
    case DW_TAG_call_site:
        *s_out = "DW_TAG_call_site";
        return DW_DLV_OK;
    case DW_TAG_call_site_parameter:
        *s_out = "DW_TAG_call_site_parameter";
        return DW_DLV_OK;
    case DW_TAG_skeleton_unit:
        *s_out = "DW_TAG_skeleton_unit";
        return DW_DLV_OK;
    case DW_TAG_immutable_type:
        *s_out = "DW_TAG_immutable_type";
        return DW_DLV_OK;
    case DW_TAG_TI_far_type:
        *s_out = "DW_TAG_TI_far_type";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x4080. DW_TAG_lo_user */
    case DW_TAG_MIPS_loop:
        *s_out = "DW_TAG_MIPS_loop";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x4081. DW_TAG_TI_near_type */
    case DW_TAG_TI_assign_register:
        *s_out = "DW_TAG_TI_assign_register";
        return DW_DLV_OK;
    case DW_TAG_TI_ioport_type:
        *s_out = "DW_TAG_TI_ioport_type";
        return DW_DLV_OK;
    case DW_TAG_TI_restrict_type:
        *s_out = "DW_TAG_TI_restrict_type";
        return DW_DLV_OK;
    case DW_TAG_TI_onchip_type:
        *s_out = "DW_TAG_TI_onchip_type";
        return DW_DLV_OK;
    case DW_TAG_HP_array_descriptor:
        *s_out = "DW_TAG_HP_array_descriptor";
        return DW_DLV_OK;
    case DW_TAG_format_label:
        *s_out = "DW_TAG_format_label";
        return DW_DLV_OK;
    case DW_TAG_function_template:
        *s_out = "DW_TAG_function_template";
        return DW_DLV_OK;
    case DW_TAG_class_template:
        *s_out = "DW_TAG_class_template";
        return DW_DLV_OK;
    case DW_TAG_GNU_BINCL:
        *s_out = "DW_TAG_GNU_BINCL";
        return DW_DLV_OK;
    case DW_TAG_GNU_EINCL:
        *s_out = "DW_TAG_GNU_EINCL";
        return DW_DLV_OK;
    case DW_TAG_GNU_template_template_parameter:
        *s_out = "DW_TAG_GNU_template_template_parameter";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x4106. DW_TAG_GNU_template_template_param */
    case DW_TAG_GNU_template_parameter_pack:
        *s_out = "DW_TAG_GNU_template_parameter_pack";
        return DW_DLV_OK;
    case DW_TAG_GNU_formal_parameter_pack:
        *s_out = "DW_TAG_GNU_formal_parameter_pack";
        return DW_DLV_OK;
    case DW_TAG_GNU_call_site:
        *s_out = "DW_TAG_GNU_call_site";
        return DW_DLV_OK;
    case DW_TAG_GNU_call_site_parameter:
        *s_out = "DW_TAG_GNU_call_site_parameter";
        return DW_DLV_OK;
    case DW_TAG_SUN_function_template:
        *s_out = "DW_TAG_SUN_function_template";
        return DW_DLV_OK;
    case DW_TAG_SUN_class_template:
        *s_out = "DW_TAG_SUN_class_template";
        return DW_DLV_OK;
    case DW_TAG_SUN_struct_template:
        *s_out = "DW_TAG_SUN_struct_template";
        return DW_DLV_OK;
    case DW_TAG_SUN_union_template:
        *s_out = "DW_TAG_SUN_union_template";
        return DW_DLV_OK;
    case DW_TAG_SUN_indirect_inheritance:
        *s_out = "DW_TAG_SUN_indirect_inheritance";
        return DW_DLV_OK;
    case DW_TAG_SUN_codeflags:
        *s_out = "DW_TAG_SUN_codeflags";
        return DW_DLV_OK;
    case DW_TAG_SUN_memop_info:
        *s_out = "DW_TAG_SUN_memop_info";
        return DW_DLV_OK;
    case DW_TAG_SUN_omp_child_func:
        *s_out = "DW_TAG_SUN_omp_child_func";
        return DW_DLV_OK;
    case DW_TAG_SUN_rtti_descriptor:
        *s_out = "DW_TAG_SUN_rtti_descriptor";
        return DW_DLV_OK;
    case DW_TAG_SUN_dtor_info:
        *s_out = "DW_TAG_SUN_dtor_info";
        return DW_DLV_OK;
    case DW_TAG_SUN_dtor:
        *s_out = "DW_TAG_SUN_dtor";
        return DW_DLV_OK;
    case DW_TAG_SUN_f90_interface:
        *s_out = "DW_TAG_SUN_f90_interface";
        return DW_DLV_OK;
    case DW_TAG_SUN_fortran_vax_structure:
        *s_out = "DW_TAG_SUN_fortran_vax_structure";
        return DW_DLV_OK;
    case DW_TAG_SUN_hi:
        *s_out = "DW_TAG_SUN_hi";
        return DW_DLV_OK;
    case DW_TAG_ALTIUM_circ_type:
        *s_out = "DW_TAG_ALTIUM_circ_type";
        return DW_DLV_OK;
    case DW_TAG_ALTIUM_mwa_circ_type:
        *s_out = "DW_TAG_ALTIUM_mwa_circ_type";
        return DW_DLV_OK;
    case DW_TAG_ALTIUM_rev_carry_type:
        *s_out = "DW_TAG_ALTIUM_rev_carry_type";
        return DW_DLV_OK;
    case DW_TAG_ALTIUM_rom:
        *s_out = "DW_TAG_ALTIUM_rom";
        return DW_DLV_OK;
    case DW_TAG_LLVM_annotation:
        *s_out = "DW_TAG_LLVM_annotation";
        return DW_DLV_OK;
    case DW_TAG_ghs_namespace:
        *s_out = "DW_TAG_ghs_namespace";
        return DW_DLV_OK;
    case DW_TAG_ghs_using_namespace:
        *s_out = "DW_TAG_ghs_using_namespace";
        return DW_DLV_OK;
    case DW_TAG_ghs_using_declaration:
        *s_out = "DW_TAG_ghs_using_declaration";
        return DW_DLV_OK;
    case DW_TAG_ghs_template_templ_param:
        *s_out = "DW_TAG_ghs_template_templ_param";
        return DW_DLV_OK;
    case DW_TAG_upc_shared_type:
        *s_out = "DW_TAG_upc_shared_type";
        return DW_DLV_OK;
    case DW_TAG_upc_strict_type:
        *s_out = "DW_TAG_upc_strict_type";
        return DW_DLV_OK;
    case DW_TAG_upc_relaxed_type:
        *s_out = "DW_TAG_upc_relaxed_type";
        return DW_DLV_OK;
    case DW_TAG_PGI_kanji_type:
        *s_out = "DW_TAG_PGI_kanji_type";
        return DW_DLV_OK;
    case DW_TAG_PGI_interface_block:
        *s_out = "DW_TAG_PGI_interface_block";
        return DW_DLV_OK;
    case DW_TAG_BORLAND_property:
        *s_out = "DW_TAG_BORLAND_property";
        return DW_DLV_OK;
    case DW_TAG_BORLAND_Delphi_string:
        *s_out = "DW_TAG_BORLAND_Delphi_string";
        return DW_DLV_OK;
    case DW_TAG_BORLAND_Delphi_dynamic_array:
        *s_out = "DW_TAG_BORLAND_Delphi_dynamic_array";
        return DW_DLV_OK;
    case DW_TAG_BORLAND_Delphi_set:
        *s_out = "DW_TAG_BORLAND_Delphi_set";
        return DW_DLV_OK;
    case DW_TAG_BORLAND_Delphi_variant:
        *s_out = "DW_TAG_BORLAND_Delphi_variant";
        return DW_DLV_OK;
    case DW_TAG_hi_user:
        *s_out = "DW_TAG_hi_user";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_children_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_children_no:
        *s_out = "DW_children_no";
        return DW_DLV_OK;
    case DW_children_yes:
        *s_out = "DW_children_yes";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_FORM_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_FORM_addr:
        *s_out = "DW_FORM_addr";
        return DW_DLV_OK;
    case DW_FORM_block2:
        *s_out = "DW_FORM_block2";
        return DW_DLV_OK;
    case DW_FORM_block4:
        *s_out = "DW_FORM_block4";
        return DW_DLV_OK;
    case DW_FORM_data2:
        *s_out = "DW_FORM_data2";
        return DW_DLV_OK;
    case DW_FORM_data4:
        *s_out = "DW_FORM_data4";
        return DW_DLV_OK;
    case DW_FORM_data8:
        *s_out = "DW_FORM_data8";
        return DW_DLV_OK;
    case DW_FORM_string:
        *s_out = "DW_FORM_string";
        return DW_DLV_OK;
    case DW_FORM_block:
        *s_out = "DW_FORM_block";
        return DW_DLV_OK;
    case DW_FORM_block1:
        *s_out = "DW_FORM_block1";
        return DW_DLV_OK;
    case DW_FORM_data1:
        *s_out = "DW_FORM_data1";
        return DW_DLV_OK;
    case DW_FORM_flag:
        *s_out = "DW_FORM_flag";
        return DW_DLV_OK;
    case DW_FORM_sdata:
        *s_out = "DW_FORM_sdata";
        return DW_DLV_OK;
    case DW_FORM_strp:
        *s_out = "DW_FORM_strp";
        return DW_DLV_OK;
    case DW_FORM_udata:
        *s_out = "DW_FORM_udata";
        return DW_DLV_OK;
    case DW_FORM_ref_addr:
        *s_out = "DW_FORM_ref_addr";
        return DW_DLV_OK;
    case DW_FORM_ref1:
        *s_out = "DW_FORM_ref1";
        return DW_DLV_OK;
    case DW_FORM_ref2:
        *s_out = "DW_FORM_ref2";
        return DW_DLV_OK;
    case DW_FORM_ref4:
        *s_out = "DW_FORM_ref4";
        return DW_DLV_OK;
    case DW_FORM_ref8:
        *s_out = "DW_FORM_ref8";
        return DW_DLV_OK;
    case DW_FORM_ref_udata:
        *s_out = "DW_FORM_ref_udata";
        return DW_DLV_OK;
    case DW_FORM_indirect:
        *s_out = "DW_FORM_indirect";
        return DW_DLV_OK;
    case DW_FORM_sec_offset:
        *s_out = "DW_FORM_sec_offset";
        return DW_DLV_OK;
    case DW_FORM_exprloc:
        *s_out = "DW_FORM_exprloc";
        return DW_DLV_OK;
    case DW_FORM_flag_present:
        *s_out = "DW_FORM_flag_present";
        return DW_DLV_OK;
    case DW_FORM_strx:
        *s_out = "DW_FORM_strx";
        return DW_DLV_OK;
    case DW_FORM_addrx:
        *s_out = "DW_FORM_addrx";
        return DW_DLV_OK;
    case DW_FORM_ref_sup4:
        *s_out = "DW_FORM_ref_sup4";
        return DW_DLV_OK;
    case DW_FORM_strp_sup:
        *s_out = "DW_FORM_strp_sup";
        return DW_DLV_OK;
    case DW_FORM_data16:
        *s_out = "DW_FORM_data16";
        return DW_DLV_OK;
    case DW_FORM_line_strp:
        *s_out = "DW_FORM_line_strp";
        return DW_DLV_OK;
    case DW_FORM_ref_sig8:
        *s_out = "DW_FORM_ref_sig8";
        return DW_DLV_OK;
    case DW_FORM_implicit_const:
        *s_out = "DW_FORM_implicit_const";
        return DW_DLV_OK;
    case DW_FORM_loclistx:
        *s_out = "DW_FORM_loclistx";
        return DW_DLV_OK;
    case DW_FORM_rnglistx:
        *s_out = "DW_FORM_rnglistx";
        return DW_DLV_OK;
    case DW_FORM_ref_sup8:
        *s_out = "DW_FORM_ref_sup8";
        return DW_DLV_OK;
    case DW_FORM_strx1:
        *s_out = "DW_FORM_strx1";
        return DW_DLV_OK;
    case DW_FORM_strx2:
        *s_out = "DW_FORM_strx2";
        return DW_DLV_OK;
    case DW_FORM_strx3:
        *s_out = "DW_FORM_strx3";
        return DW_DLV_OK;
    case DW_FORM_strx4:
        *s_out = "DW_FORM_strx4";
        return DW_DLV_OK;
    case DW_FORM_addrx1:
        *s_out = "DW_FORM_addrx1";
        return DW_DLV_OK;
    case DW_FORM_addrx2:
        *s_out = "DW_FORM_addrx2";
        return DW_DLV_OK;
    case DW_FORM_addrx3:
        *s_out = "DW_FORM_addrx3";
        return DW_DLV_OK;
    case DW_FORM_addrx4:
        *s_out = "DW_FORM_addrx4";
        return DW_DLV_OK;
    case DW_FORM_GNU_addr_index:
        *s_out = "DW_FORM_GNU_addr_index";
        return DW_DLV_OK;
    case DW_FORM_GNU_str_index:
        *s_out = "DW_FORM_GNU_str_index";
        return DW_DLV_OK;
    case DW_FORM_GNU_ref_alt:
        *s_out = "DW_FORM_GNU_ref_alt";
        return DW_DLV_OK;
    case DW_FORM_GNU_strp_alt:
        *s_out = "DW_FORM_GNU_strp_alt";
        return DW_DLV_OK;
    case DW_FORM_LLVM_addrx_offset:
        *s_out = "DW_FORM_LLVM_addrx_offset";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_AT_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_AT_sibling:
        *s_out = "DW_AT_sibling";
        return DW_DLV_OK;
    case DW_AT_location:
        *s_out = "DW_AT_location";
        return DW_DLV_OK;
    case DW_AT_name:
        *s_out = "DW_AT_name";
        return DW_DLV_OK;
    case DW_AT_ordering:
        *s_out = "DW_AT_ordering";
        return DW_DLV_OK;
    case DW_AT_subscr_data:
        *s_out = "DW_AT_subscr_data";
        return DW_DLV_OK;
    case DW_AT_byte_size:
        *s_out = "DW_AT_byte_size";
        return DW_DLV_OK;
    case DW_AT_bit_offset:
        *s_out = "DW_AT_bit_offset";
        return DW_DLV_OK;
    case DW_AT_bit_size:
        *s_out = "DW_AT_bit_size";
        return DW_DLV_OK;
    case DW_AT_element_list:
        *s_out = "DW_AT_element_list";
        return DW_DLV_OK;
    case DW_AT_stmt_list:
        *s_out = "DW_AT_stmt_list";
        return DW_DLV_OK;
    case DW_AT_low_pc:
        *s_out = "DW_AT_low_pc";
        return DW_DLV_OK;
    case DW_AT_high_pc:
        *s_out = "DW_AT_high_pc";
        return DW_DLV_OK;
    case DW_AT_language:
        *s_out = "DW_AT_language";
        return DW_DLV_OK;
    case DW_AT_member:
        *s_out = "DW_AT_member";
        return DW_DLV_OK;
    case DW_AT_discr:
        *s_out = "DW_AT_discr";
        return DW_DLV_OK;
    case DW_AT_discr_value:
        *s_out = "DW_AT_discr_value";
        return DW_DLV_OK;
    case DW_AT_visibility:
        *s_out = "DW_AT_visibility";
        return DW_DLV_OK;
    case DW_AT_import:
        *s_out = "DW_AT_import";
        return DW_DLV_OK;
    case DW_AT_string_length:
        *s_out = "DW_AT_string_length";
        return DW_DLV_OK;
    case DW_AT_common_reference:
        *s_out = "DW_AT_common_reference";
        return DW_DLV_OK;
    case DW_AT_comp_dir:
        *s_out = "DW_AT_comp_dir";
        return DW_DLV_OK;
    case DW_AT_const_value:
        *s_out = "DW_AT_const_value";
        return DW_DLV_OK;
    case DW_AT_containing_type:
        *s_out = "DW_AT_containing_type";
        return DW_DLV_OK;
    case DW_AT_default_value:
        *s_out = "DW_AT_default_value";
        return DW_DLV_OK;
    case DW_AT_inline:
        *s_out = "DW_AT_inline";
        return DW_DLV_OK;
    case DW_AT_is_optional:
        *s_out = "DW_AT_is_optional";
        return DW_DLV_OK;
    case DW_AT_lower_bound:
        *s_out = "DW_AT_lower_bound";
        return DW_DLV_OK;
    case DW_AT_producer:
        *s_out = "DW_AT_producer";
        return DW_DLV_OK;
    case DW_AT_prototyped:
        *s_out = "DW_AT_prototyped";
        return DW_DLV_OK;
    case DW_AT_return_addr:
        *s_out = "DW_AT_return_addr";
        return DW_DLV_OK;
    case DW_AT_start_scope:
        *s_out = "DW_AT_start_scope";
        return DW_DLV_OK;
    case DW_AT_bit_stride:
        *s_out = "DW_AT_bit_stride";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x2e. DW_AT_stride_size */
    case DW_AT_upper_bound:
        *s_out = "DW_AT_upper_bound";
        return DW_DLV_OK;
    case DW_AT_abstract_origin:
        *s_out = "DW_AT_abstract_origin";
        return DW_DLV_OK;
    case DW_AT_accessibility:
        *s_out = "DW_AT_accessibility";
        return DW_DLV_OK;
    case DW_AT_address_class:
        *s_out = "DW_AT_address_class";
        return DW_DLV_OK;
    case DW_AT_artificial:
        *s_out = "DW_AT_artificial";
        return DW_DLV_OK;
    case DW_AT_base_types:
        *s_out = "DW_AT_base_types";
        return DW_DLV_OK;
    case DW_AT_calling_convention:
        *s_out = "DW_AT_calling_convention";
        return DW_DLV_OK;
    case DW_AT_count:
        *s_out = "DW_AT_count";
        return DW_DLV_OK;
    case DW_AT_data_member_location:
        *s_out = "DW_AT_data_member_location";
        return DW_DLV_OK;
    case DW_AT_decl_column:
        *s_out = "DW_AT_decl_column";
        return DW_DLV_OK;
    case DW_AT_decl_file:
        *s_out = "DW_AT_decl_file";
        return DW_DLV_OK;
    case DW_AT_decl_line:
        *s_out = "DW_AT_decl_line";
        return DW_DLV_OK;
    case DW_AT_declaration:
        *s_out = "DW_AT_declaration";
        return DW_DLV_OK;
    case DW_AT_discr_list:
        *s_out = "DW_AT_discr_list";
        return DW_DLV_OK;
    case DW_AT_encoding:
        *s_out = "DW_AT_encoding";
        return DW_DLV_OK;
    case DW_AT_external:
        *s_out = "DW_AT_external";
        return DW_DLV_OK;
    case DW_AT_frame_base:
        *s_out = "DW_AT_frame_base";
        return DW_DLV_OK;
    case DW_AT_friend:
        *s_out = "DW_AT_friend";
        return DW_DLV_OK;
    case DW_AT_identifier_case:
        *s_out = "DW_AT_identifier_case";
        return DW_DLV_OK;
    case DW_AT_macro_info:
        *s_out = "DW_AT_macro_info";
        return DW_DLV_OK;
    case DW_AT_namelist_item:
        *s_out = "DW_AT_namelist_item";
        return DW_DLV_OK;
    case DW_AT_priority:
        *s_out = "DW_AT_priority";
        return DW_DLV_OK;
    case DW_AT_segment:
        *s_out = "DW_AT_segment";
        return DW_DLV_OK;
    case DW_AT_specification:
        *s_out = "DW_AT_specification";
        return DW_DLV_OK;
    case DW_AT_static_link:
        *s_out = "DW_AT_static_link";
        return DW_DLV_OK;
    case DW_AT_type:
        *s_out = "DW_AT_type";
        return DW_DLV_OK;
    case DW_AT_use_location:
        *s_out = "DW_AT_use_location";
        return DW_DLV_OK;
    case DW_AT_variable_parameter:
        *s_out = "DW_AT_variable_parameter";
        return DW_DLV_OK;
    case DW_AT_virtuality:
        *s_out = "DW_AT_virtuality";
        return DW_DLV_OK;
    case DW_AT_vtable_elem_location:
        *s_out = "DW_AT_vtable_elem_location";
        return DW_DLV_OK;
    case DW_AT_allocated:
        *s_out = "DW_AT_allocated";
        return DW_DLV_OK;
    case DW_AT_associated:
        *s_out = "DW_AT_associated";
        return DW_DLV_OK;
    case DW_AT_data_location:
        *s_out = "DW_AT_data_location";
        return DW_DLV_OK;
    case DW_AT_byte_stride:
        *s_out = "DW_AT_byte_stride";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x51. DW_AT_stride */
    case DW_AT_entry_pc:
        *s_out = "DW_AT_entry_pc";
        return DW_DLV_OK;
    case DW_AT_use_UTF8:
        *s_out = "DW_AT_use_UTF8";
        return DW_DLV_OK;
    case DW_AT_extension:
        *s_out = "DW_AT_extension";
        return DW_DLV_OK;
    case DW_AT_ranges:
        *s_out = "DW_AT_ranges";
        return DW_DLV_OK;
    case DW_AT_trampoline:
        *s_out = "DW_AT_trampoline";
        return DW_DLV_OK;
    case DW_AT_call_column:
        *s_out = "DW_AT_call_column";
        return DW_DLV_OK;
    case DW_AT_call_file:
        *s_out = "DW_AT_call_file";
        return DW_DLV_OK;
    case DW_AT_call_line:
        *s_out = "DW_AT_call_line";
        return DW_DLV_OK;
    case DW_AT_description:
        *s_out = "DW_AT_description";
        return DW_DLV_OK;
    case DW_AT_binary_scale:
        *s_out = "DW_AT_binary_scale";
        return DW_DLV_OK;
    case DW_AT_decimal_scale:
        *s_out = "DW_AT_decimal_scale";
        return DW_DLV_OK;
    case DW_AT_small:
        *s_out = "DW_AT_small";
        return DW_DLV_OK;
    case DW_AT_decimal_sign:
        *s_out = "DW_AT_decimal_sign";
        return DW_DLV_OK;
    case DW_AT_digit_count:
        *s_out = "DW_AT_digit_count";
        return DW_DLV_OK;
    case DW_AT_picture_string:
        *s_out = "DW_AT_picture_string";
        return DW_DLV_OK;
    case DW_AT_mutable:
        *s_out = "DW_AT_mutable";
        return DW_DLV_OK;
    case DW_AT_threads_scaled:
        *s_out = "DW_AT_threads_scaled";
        return DW_DLV_OK;
    case DW_AT_explicit:
        *s_out = "DW_AT_explicit";
        return DW_DLV_OK;
    case DW_AT_object_pointer:
        *s_out = "DW_AT_object_pointer";
        return DW_DLV_OK;
    case DW_AT_endianity:
        *s_out = "DW_AT_endianity";
        return DW_DLV_OK;
    case DW_AT_elemental:
        *s_out = "DW_AT_elemental";
        return DW_DLV_OK;
    case DW_AT_pure:
        *s_out = "DW_AT_pure";
        return DW_DLV_OK;
    case DW_AT_recursive:
        *s_out = "DW_AT_recursive";
        return DW_DLV_OK;
    case DW_AT_signature:
        *s_out = "DW_AT_signature";
        return DW_DLV_OK;
    case DW_AT_main_subprogram:
        *s_out = "DW_AT_main_subprogram";
        return DW_DLV_OK;
    case DW_AT_data_bit_offset:
        *s_out = "DW_AT_data_bit_offset";
        return DW_DLV_OK;
    case DW_AT_const_expr:
        *s_out = "DW_AT_const_expr";
        return DW_DLV_OK;
    case DW_AT_enum_class:
        *s_out = "DW_AT_enum_class";
        return DW_DLV_OK;
    case DW_AT_linkage_name:
        *s_out = "DW_AT_linkage_name";
        return DW_DLV_OK;
    case DW_AT_string_length_bit_size:
        *s_out = "DW_AT_string_length_bit_size";
        return DW_DLV_OK;
    case DW_AT_string_length_byte_size:
        *s_out = "DW_AT_string_length_byte_size";
        return DW_DLV_OK;
    case DW_AT_rank:
        *s_out = "DW_AT_rank";
        return DW_DLV_OK;
    case DW_AT_str_offsets_base:
        *s_out = "DW_AT_str_offsets_base";
        return DW_DLV_OK;
    case DW_AT_addr_base:
        *s_out = "DW_AT_addr_base";
        return DW_DLV_OK;
    case DW_AT_rnglists_base:
        *s_out = "DW_AT_rnglists_base";
        return DW_DLV_OK;
    case DW_AT_dwo_id:
        *s_out = "DW_AT_dwo_id";
        return DW_DLV_OK;
    case DW_AT_dwo_name:
        *s_out = "DW_AT_dwo_name";
        return DW_DLV_OK;
    case DW_AT_reference:
        *s_out = "DW_AT_reference";
        return DW_DLV_OK;
    case DW_AT_rvalue_reference:
        *s_out = "DW_AT_rvalue_reference";
        return DW_DLV_OK;
    case DW_AT_macros:
        *s_out = "DW_AT_macros";
        return DW_DLV_OK;
    case DW_AT_call_all_calls:
        *s_out = "DW_AT_call_all_calls";
        return DW_DLV_OK;
    case DW_AT_call_all_source_calls:
        *s_out = "DW_AT_call_all_source_calls";
        return DW_DLV_OK;
    case DW_AT_call_all_tail_calls:
        *s_out = "DW_AT_call_all_tail_calls";
        return DW_DLV_OK;
    case DW_AT_call_return_pc:
        *s_out = "DW_AT_call_return_pc";
        return DW_DLV_OK;
    case DW_AT_call_value:
        *s_out = "DW_AT_call_value";
        return DW_DLV_OK;
    case DW_AT_call_origin:
        *s_out = "DW_AT_call_origin";
        return DW_DLV_OK;
    case DW_AT_call_parameter:
        *s_out = "DW_AT_call_parameter";
        return DW_DLV_OK;
    case DW_AT_call_pc:
        *s_out = "DW_AT_call_pc";
        return DW_DLV_OK;
    case DW_AT_call_tail_call:
        *s_out = "DW_AT_call_tail_call";
        return DW_DLV_OK;
    case DW_AT_call_target:
        *s_out = "DW_AT_call_target";
        return DW_DLV_OK;
    case DW_AT_call_target_clobbered:
        *s_out = "DW_AT_call_target_clobbered";
        return DW_DLV_OK;
    case DW_AT_call_data_location:
        *s_out = "DW_AT_call_data_location";
        return DW_DLV_OK;
    case DW_AT_call_data_value:
        *s_out = "DW_AT_call_data_value";
        return DW_DLV_OK;
    case DW_AT_noreturn:
        *s_out = "DW_AT_noreturn";
        return DW_DLV_OK;
    case DW_AT_alignment:
        *s_out = "DW_AT_alignment";
        return DW_DLV_OK;
    case DW_AT_export_symbols:
        *s_out = "DW_AT_export_symbols";
        return DW_DLV_OK;
    case DW_AT_deleted:
        *s_out = "DW_AT_deleted";
        return DW_DLV_OK;
    case DW_AT_defaulted:
        *s_out = "DW_AT_defaulted";
        return DW_DLV_OK;
    case DW_AT_loclists_base:
        *s_out = "DW_AT_loclists_base";
        return DW_DLV_OK;
    case DW_AT_language_name:
        *s_out = "DW_AT_language_name";
        return DW_DLV_OK;
    case DW_AT_language_version:
        *s_out = "DW_AT_language_version";
        return DW_DLV_OK;
    case DW_AT_ghs_namespace_alias:
        *s_out = "DW_AT_ghs_namespace_alias";
        return DW_DLV_OK;
    case DW_AT_ghs_using_namespace:
        *s_out = "DW_AT_ghs_using_namespace";
        return DW_DLV_OK;
    case DW_AT_ghs_using_declaration:
        *s_out = "DW_AT_ghs_using_declaration";
        return DW_DLV_OK;
    case DW_AT_HP_block_index:
        *s_out = "DW_AT_HP_block_index";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x2000. DW_AT_lo_user */
    /*  Skipping alternate spelling of value
        0x2000. DW_AT_TI_veneer */
    case DW_AT_MIPS_fde:
        *s_out = "DW_AT_MIPS_fde";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x2001. DW_AT_TI_symbol_name */
    /*  Skipping alternate spelling of value
        0x2001. DW_AT_HP_unmodifiable */
    /*  Skipping alternate spelling of value
        0x2001. DW_AT_CPQ_discontig_ranges */
    case DW_AT_MIPS_loop_begin:
        *s_out = "DW_AT_MIPS_loop_begin";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x2002. DW_AT_CPQ_semantic_events */
    case DW_AT_MIPS_tail_loop_begin:
        *s_out = "DW_AT_MIPS_tail_loop_begin";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x2003. DW_AT_CPQ_split_lifetimes_var */
    case DW_AT_MIPS_epilog_begin:
        *s_out = "DW_AT_MIPS_epilog_begin";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x2004. DW_AT_CPQ_split_lifetimes_rtn */
    case DW_AT_MIPS_loop_unroll_factor:
        *s_out = "DW_AT_MIPS_loop_unroll_factor";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x2005. DW_AT_HP_prologue */
    /*  Skipping alternate spelling of value
        0x2005. DW_AT_CPQ_prologue_length */
    case DW_AT_MIPS_software_pipeline_depth:
        *s_out = "DW_AT_MIPS_software_pipeline_depth";
        return DW_DLV_OK;
    case DW_AT_MIPS_linkage_name:
        *s_out = "DW_AT_MIPS_linkage_name";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x2007. DW_AT_ghs_mangled */
    case DW_AT_MIPS_stride:
        *s_out = "DW_AT_MIPS_stride";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x2008. DW_AT_HP_epilogue */
    case DW_AT_MIPS_abstract_name:
        *s_out = "DW_AT_MIPS_abstract_name";
        return DW_DLV_OK;
    case DW_AT_MIPS_clone_origin:
        *s_out = "DW_AT_MIPS_clone_origin";
        return DW_DLV_OK;
    case DW_AT_MIPS_has_inlines:
        *s_out = "DW_AT_MIPS_has_inlines";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x200b. DW_AT_TI_version */
    case DW_AT_MIPS_stride_byte:
        *s_out = "DW_AT_MIPS_stride_byte";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x200c. DW_AT_TI_asm */
    case DW_AT_MIPS_stride_elem:
        *s_out = "DW_AT_MIPS_stride_elem";
        return DW_DLV_OK;
    case DW_AT_MIPS_ptr_dopetype:
        *s_out = "DW_AT_MIPS_ptr_dopetype";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x200e. DW_AT_TI_skeletal */
    case DW_AT_MIPS_allocatable_dopetype:
        *s_out = "DW_AT_MIPS_allocatable_dopetype";
        return DW_DLV_OK;
    case DW_AT_MIPS_assumed_shape_dopetype:
        *s_out = "DW_AT_MIPS_assumed_shape_dopetype";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x2010. DW_AT_HP_actuals_stmt_list */
    case DW_AT_MIPS_assumed_size:
        *s_out = "DW_AT_MIPS_assumed_size";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x2011. DW_AT_TI_interrupt */
    /*  Skipping alternate spelling of value
        0x2011. DW_AT_HP_proc_per_section */
    case DW_AT_HP_raw_data_ptr:
        *s_out = "DW_AT_HP_raw_data_ptr";
        return DW_DLV_OK;
    case DW_AT_HP_pass_by_reference:
        *s_out = "DW_AT_HP_pass_by_reference";
        return DW_DLV_OK;
    case DW_AT_HP_opt_level:
        *s_out = "DW_AT_HP_opt_level";
        return DW_DLV_OK;
    case DW_AT_HP_prof_version_id:
        *s_out = "DW_AT_HP_prof_version_id";
        return DW_DLV_OK;
    case DW_AT_HP_opt_flags:
        *s_out = "DW_AT_HP_opt_flags";
        return DW_DLV_OK;
    case DW_AT_HP_cold_region_low_pc:
        *s_out = "DW_AT_HP_cold_region_low_pc";
        return DW_DLV_OK;
    case DW_AT_HP_cold_region_high_pc:
        *s_out = "DW_AT_HP_cold_region_high_pc";
        return DW_DLV_OK;
    case DW_AT_HP_all_variables_modifiable:
        *s_out = "DW_AT_HP_all_variables_modifiable";
        return DW_DLV_OK;
    case DW_AT_HP_linkage_name:
        *s_out = "DW_AT_HP_linkage_name";
        return DW_DLV_OK;
    case DW_AT_HP_prof_flags:
        *s_out = "DW_AT_HP_prof_flags";
        return DW_DLV_OK;
    case DW_AT_HP_unit_name:
        *s_out = "DW_AT_HP_unit_name";
        return DW_DLV_OK;
    case DW_AT_HP_unit_size:
        *s_out = "DW_AT_HP_unit_size";
        return DW_DLV_OK;
    case DW_AT_HP_widened_byte_size:
        *s_out = "DW_AT_HP_widened_byte_size";
        return DW_DLV_OK;
    case DW_AT_HP_definition_points:
        *s_out = "DW_AT_HP_definition_points";
        return DW_DLV_OK;
    case DW_AT_HP_default_location:
        *s_out = "DW_AT_HP_default_location";
        return DW_DLV_OK;
    case DW_AT_INTEL_other_endian:
        *s_out = "DW_AT_INTEL_other_endian";
        return DW_DLV_OK;
    case DW_AT_HP_is_result_param:
        *s_out = "DW_AT_HP_is_result_param";
        return DW_DLV_OK;
    case DW_AT_ghs_rsm:
        *s_out = "DW_AT_ghs_rsm";
        return DW_DLV_OK;
    case DW_AT_ghs_frsm:
        *s_out = "DW_AT_ghs_frsm";
        return DW_DLV_OK;
    case DW_AT_ghs_frames:
        *s_out = "DW_AT_ghs_frames";
        return DW_DLV_OK;
    case DW_AT_ghs_rso:
        *s_out = "DW_AT_ghs_rso";
        return DW_DLV_OK;
    case DW_AT_ghs_subcpu:
        *s_out = "DW_AT_ghs_subcpu";
        return DW_DLV_OK;
    case DW_AT_ghs_lbrace_line:
        *s_out = "DW_AT_ghs_lbrace_line";
        return DW_DLV_OK;
    case DW_AT_sf_names:
        *s_out = "DW_AT_sf_names";
        return DW_DLV_OK;
    case DW_AT_src_info:
        *s_out = "DW_AT_src_info";
        return DW_DLV_OK;
    case DW_AT_mac_info:
        *s_out = "DW_AT_mac_info";
        return DW_DLV_OK;
    case DW_AT_src_coords:
        *s_out = "DW_AT_src_coords";
        return DW_DLV_OK;
    case DW_AT_body_begin:
        *s_out = "DW_AT_body_begin";
        return DW_DLV_OK;
    case DW_AT_body_end:
        *s_out = "DW_AT_body_end";
        return DW_DLV_OK;
    case DW_AT_GNU_vector:
        *s_out = "DW_AT_GNU_vector";
        return DW_DLV_OK;
    case DW_AT_GNU_guarded_by:
        *s_out = "DW_AT_GNU_guarded_by";
        return DW_DLV_OK;
    case DW_AT_GNU_pt_guarded_by:
        *s_out = "DW_AT_GNU_pt_guarded_by";
        return DW_DLV_OK;
    case DW_AT_GNU_guarded:
        *s_out = "DW_AT_GNU_guarded";
        return DW_DLV_OK;
    case DW_AT_GNU_pt_guarded:
        *s_out = "DW_AT_GNU_pt_guarded";
        return DW_DLV_OK;
    case DW_AT_GNU_locks_excluded:
        *s_out = "DW_AT_GNU_locks_excluded";
        return DW_DLV_OK;
    case DW_AT_GNU_exclusive_locks_required:
        *s_out = "DW_AT_GNU_exclusive_locks_required";
        return DW_DLV_OK;
    case DW_AT_GNU_shared_locks_required:
        *s_out = "DW_AT_GNU_shared_locks_required";
        return DW_DLV_OK;
    case DW_AT_GNU_odr_signature:
        *s_out = "DW_AT_GNU_odr_signature";
        return DW_DLV_OK;
    case DW_AT_GNU_template_name:
        *s_out = "DW_AT_GNU_template_name";
        return DW_DLV_OK;
    case DW_AT_GNU_call_site_value:
        *s_out = "DW_AT_GNU_call_site_value";
        return DW_DLV_OK;
    case DW_AT_GNU_call_site_data_value:
        *s_out = "DW_AT_GNU_call_site_data_value";
        return DW_DLV_OK;
    case DW_AT_GNU_call_site_target:
        *s_out = "DW_AT_GNU_call_site_target";
        return DW_DLV_OK;
    case DW_AT_GNU_call_site_target_clobbered:
        *s_out = "DW_AT_GNU_call_site_target_clobbered";
        return DW_DLV_OK;
    case DW_AT_GNU_tail_call:
        *s_out = "DW_AT_GNU_tail_call";
        return DW_DLV_OK;
    case DW_AT_GNU_all_tail_call_sites:
        *s_out = "DW_AT_GNU_all_tail_call_sites";
        return DW_DLV_OK;
    case DW_AT_GNU_all_call_sites:
        *s_out = "DW_AT_GNU_all_call_sites";
        return DW_DLV_OK;
    case DW_AT_GNU_all_source_call_sites:
        *s_out = "DW_AT_GNU_all_source_call_sites";
        return DW_DLV_OK;
    case DW_AT_GNU_macros:
        *s_out = "DW_AT_GNU_macros";
        return DW_DLV_OK;
    case DW_AT_GNU_deleted:
        *s_out = "DW_AT_GNU_deleted";
        return DW_DLV_OK;
    case DW_AT_GNU_dwo_name:
        *s_out = "DW_AT_GNU_dwo_name";
        return DW_DLV_OK;
    case DW_AT_GNU_dwo_id:
        *s_out = "DW_AT_GNU_dwo_id";
        return DW_DLV_OK;
    case DW_AT_GNU_ranges_base:
        *s_out = "DW_AT_GNU_ranges_base";
        return DW_DLV_OK;
    case DW_AT_GNU_addr_base:
        *s_out = "DW_AT_GNU_addr_base";
        return DW_DLV_OK;
    case DW_AT_GNU_pubnames:
        *s_out = "DW_AT_GNU_pubnames";
        return DW_DLV_OK;
    case DW_AT_GNU_pubtypes:
        *s_out = "DW_AT_GNU_pubtypes";
        return DW_DLV_OK;
    case DW_AT_GNU_discriminator:
        *s_out = "DW_AT_GNU_discriminator";
        return DW_DLV_OK;
    case DW_AT_GNU_locviews:
        *s_out = "DW_AT_GNU_locviews";
        return DW_DLV_OK;
    case DW_AT_GNU_entry_view:
        *s_out = "DW_AT_GNU_entry_view";
        return DW_DLV_OK;
    case DW_AT_SUN_template:
        *s_out = "DW_AT_SUN_template";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x2201. DW_AT_VMS_rtnbeg_pd_address */
    case DW_AT_SUN_alignment:
        *s_out = "DW_AT_SUN_alignment";
        return DW_DLV_OK;
    case DW_AT_SUN_vtable:
        *s_out = "DW_AT_SUN_vtable";
        return DW_DLV_OK;
    case DW_AT_SUN_count_guarantee:
        *s_out = "DW_AT_SUN_count_guarantee";
        return DW_DLV_OK;
    case DW_AT_SUN_command_line:
        *s_out = "DW_AT_SUN_command_line";
        return DW_DLV_OK;
    case DW_AT_SUN_vbase:
        *s_out = "DW_AT_SUN_vbase";
        return DW_DLV_OK;
    case DW_AT_SUN_compile_options:
        *s_out = "DW_AT_SUN_compile_options";
        return DW_DLV_OK;
    case DW_AT_SUN_language:
        *s_out = "DW_AT_SUN_language";
        return DW_DLV_OK;
    case DW_AT_SUN_browser_file:
        *s_out = "DW_AT_SUN_browser_file";
        return DW_DLV_OK;
    case DW_AT_SUN_vtable_abi:
        *s_out = "DW_AT_SUN_vtable_abi";
        return DW_DLV_OK;
    case DW_AT_SUN_func_offsets:
        *s_out = "DW_AT_SUN_func_offsets";
        return DW_DLV_OK;
    case DW_AT_SUN_cf_kind:
        *s_out = "DW_AT_SUN_cf_kind";
        return DW_DLV_OK;
    case DW_AT_SUN_vtable_index:
        *s_out = "DW_AT_SUN_vtable_index";
        return DW_DLV_OK;
    case DW_AT_SUN_omp_tpriv_addr:
        *s_out = "DW_AT_SUN_omp_tpriv_addr";
        return DW_DLV_OK;
    case DW_AT_SUN_omp_child_func:
        *s_out = "DW_AT_SUN_omp_child_func";
        return DW_DLV_OK;
    case DW_AT_SUN_func_offset:
        *s_out = "DW_AT_SUN_func_offset";
        return DW_DLV_OK;
    case DW_AT_SUN_memop_type_ref:
        *s_out = "DW_AT_SUN_memop_type_ref";
        return DW_DLV_OK;
    case DW_AT_SUN_profile_id:
        *s_out = "DW_AT_SUN_profile_id";
        return DW_DLV_OK;
    case DW_AT_SUN_memop_signature:
        *s_out = "DW_AT_SUN_memop_signature";
        return DW_DLV_OK;
    case DW_AT_SUN_obj_dir:
        *s_out = "DW_AT_SUN_obj_dir";
        return DW_DLV_OK;
    case DW_AT_SUN_obj_file:
        *s_out = "DW_AT_SUN_obj_file";
        return DW_DLV_OK;
    case DW_AT_SUN_original_name:
        *s_out = "DW_AT_SUN_original_name";
        return DW_DLV_OK;
    case DW_AT_SUN_hwcprof_signature:
        *s_out = "DW_AT_SUN_hwcprof_signature";
        return DW_DLV_OK;
    case DW_AT_SUN_amd64_parmdump:
        *s_out = "DW_AT_SUN_amd64_parmdump";
        return DW_DLV_OK;
    case DW_AT_SUN_part_link_name:
        *s_out = "DW_AT_SUN_part_link_name";
        return DW_DLV_OK;
    case DW_AT_SUN_link_name:
        *s_out = "DW_AT_SUN_link_name";
        return DW_DLV_OK;
    case DW_AT_SUN_pass_with_const:
        *s_out = "DW_AT_SUN_pass_with_const";
        return DW_DLV_OK;
    case DW_AT_SUN_return_with_const:
        *s_out = "DW_AT_SUN_return_with_const";
        return DW_DLV_OK;
    case DW_AT_SUN_import_by_name:
        *s_out = "DW_AT_SUN_import_by_name";
        return DW_DLV_OK;
    case DW_AT_SUN_f90_pointer:
        *s_out = "DW_AT_SUN_f90_pointer";
        return DW_DLV_OK;
    case DW_AT_SUN_pass_by_ref:
        *s_out = "DW_AT_SUN_pass_by_ref";
        return DW_DLV_OK;
    case DW_AT_SUN_f90_allocatable:
        *s_out = "DW_AT_SUN_f90_allocatable";
        return DW_DLV_OK;
    case DW_AT_SUN_f90_assumed_shape_array:
        *s_out = "DW_AT_SUN_f90_assumed_shape_array";
        return DW_DLV_OK;
    case DW_AT_SUN_c_vla:
        *s_out = "DW_AT_SUN_c_vla";
        return DW_DLV_OK;
    case DW_AT_SUN_return_value_ptr:
        *s_out = "DW_AT_SUN_return_value_ptr";
        return DW_DLV_OK;
    case DW_AT_SUN_dtor_start:
        *s_out = "DW_AT_SUN_dtor_start";
        return DW_DLV_OK;
    case DW_AT_SUN_dtor_length:
        *s_out = "DW_AT_SUN_dtor_length";
        return DW_DLV_OK;
    case DW_AT_SUN_dtor_state_initial:
        *s_out = "DW_AT_SUN_dtor_state_initial";
        return DW_DLV_OK;
    case DW_AT_SUN_dtor_state_final:
        *s_out = "DW_AT_SUN_dtor_state_final";
        return DW_DLV_OK;
    case DW_AT_SUN_dtor_state_deltas:
        *s_out = "DW_AT_SUN_dtor_state_deltas";
        return DW_DLV_OK;
    case DW_AT_SUN_import_by_lname:
        *s_out = "DW_AT_SUN_import_by_lname";
        return DW_DLV_OK;
    case DW_AT_SUN_f90_use_only:
        *s_out = "DW_AT_SUN_f90_use_only";
        return DW_DLV_OK;
    case DW_AT_SUN_namelist_spec:
        *s_out = "DW_AT_SUN_namelist_spec";
        return DW_DLV_OK;
    case DW_AT_SUN_is_omp_child_func:
        *s_out = "DW_AT_SUN_is_omp_child_func";
        return DW_DLV_OK;
    case DW_AT_SUN_fortran_main_alias:
        *s_out = "DW_AT_SUN_fortran_main_alias";
        return DW_DLV_OK;
    case DW_AT_SUN_fortran_based:
        *s_out = "DW_AT_SUN_fortran_based";
        return DW_DLV_OK;
    case DW_AT_ALTIUM_loclist:
        *s_out = "DW_AT_ALTIUM_loclist";
        return DW_DLV_OK;
    case DW_AT_use_GNAT_descriptive_type:
        *s_out = "DW_AT_use_GNAT_descriptive_type";
        return DW_DLV_OK;
    case DW_AT_GNAT_descriptive_type:
        *s_out = "DW_AT_GNAT_descriptive_type";
        return DW_DLV_OK;
    case DW_AT_GNU_numerator:
        *s_out = "DW_AT_GNU_numerator";
        return DW_DLV_OK;
    case DW_AT_GNU_denominator:
        *s_out = "DW_AT_GNU_denominator";
        return DW_DLV_OK;
    case DW_AT_GNU_bias:
        *s_out = "DW_AT_GNU_bias";
        return DW_DLV_OK;
    case DW_AT_go_kind:
        *s_out = "DW_AT_go_kind";
        return DW_DLV_OK;
    case DW_AT_go_key:
        *s_out = "DW_AT_go_key";
        return DW_DLV_OK;
    case DW_AT_go_elem:
        *s_out = "DW_AT_go_elem";
        return DW_DLV_OK;
    case DW_AT_go_embedded_field:
        *s_out = "DW_AT_go_embedded_field";
        return DW_DLV_OK;
    case DW_AT_go_runtime_type:
        *s_out = "DW_AT_go_runtime_type";
        return DW_DLV_OK;
    case DW_AT_upc_threads_scaled:
        *s_out = "DW_AT_upc_threads_scaled";
        return DW_DLV_OK;
    case DW_AT_IBM_wsa_addr:
        *s_out = "DW_AT_IBM_wsa_addr";
        return DW_DLV_OK;
    case DW_AT_IBM_home_location:
        *s_out = "DW_AT_IBM_home_location";
        return DW_DLV_OK;
    case DW_AT_IBM_alt_srcview:
        *s_out = "DW_AT_IBM_alt_srcview";
        return DW_DLV_OK;
    case DW_AT_PGI_lbase:
        *s_out = "DW_AT_PGI_lbase";
        return DW_DLV_OK;
    case DW_AT_PGI_soffset:
        *s_out = "DW_AT_PGI_soffset";
        return DW_DLV_OK;
    case DW_AT_PGI_lstride:
        *s_out = "DW_AT_PGI_lstride";
        return DW_DLV_OK;
    case DW_AT_BORLAND_property_read:
        *s_out = "DW_AT_BORLAND_property_read";
        return DW_DLV_OK;
    case DW_AT_BORLAND_property_write:
        *s_out = "DW_AT_BORLAND_property_write";
        return DW_DLV_OK;
    case DW_AT_BORLAND_property_implements:
        *s_out = "DW_AT_BORLAND_property_implements";
        return DW_DLV_OK;
    case DW_AT_BORLAND_property_index:
        *s_out = "DW_AT_BORLAND_property_index";
        return DW_DLV_OK;
    case DW_AT_BORLAND_property_default:
        *s_out = "DW_AT_BORLAND_property_default";
        return DW_DLV_OK;
    case DW_AT_BORLAND_Delphi_unit:
        *s_out = "DW_AT_BORLAND_Delphi_unit";
        return DW_DLV_OK;
    case DW_AT_BORLAND_Delphi_class:
        *s_out = "DW_AT_BORLAND_Delphi_class";
        return DW_DLV_OK;
    case DW_AT_BORLAND_Delphi_record:
        *s_out = "DW_AT_BORLAND_Delphi_record";
        return DW_DLV_OK;
    case DW_AT_BORLAND_Delphi_metaclass:
        *s_out = "DW_AT_BORLAND_Delphi_metaclass";
        return DW_DLV_OK;
    case DW_AT_BORLAND_Delphi_constructor:
        *s_out = "DW_AT_BORLAND_Delphi_constructor";
        return DW_DLV_OK;
    case DW_AT_BORLAND_Delphi_destructor:
        *s_out = "DW_AT_BORLAND_Delphi_destructor";
        return DW_DLV_OK;
    case DW_AT_BORLAND_Delphi_anonymous_method:
        *s_out = "DW_AT_BORLAND_Delphi_anonymous_method";
        return DW_DLV_OK;
    case DW_AT_BORLAND_Delphi_interface:
        *s_out = "DW_AT_BORLAND_Delphi_interface";
        return DW_DLV_OK;
    case DW_AT_BORLAND_Delphi_ABI:
        *s_out = "DW_AT_BORLAND_Delphi_ABI";
        return DW_DLV_OK;
    case DW_AT_BORLAND_Delphi_frameptr:
        *s_out = "DW_AT_BORLAND_Delphi_frameptr";
        return DW_DLV_OK;
    case DW_AT_BORLAND_closure:
        *s_out = "DW_AT_BORLAND_closure";
        return DW_DLV_OK;
    case DW_AT_LLVM_include_path:
        *s_out = "DW_AT_LLVM_include_path";
        return DW_DLV_OK;
    case DW_AT_LLVM_config_macros:
        *s_out = "DW_AT_LLVM_config_macros";
        return DW_DLV_OK;
    case DW_AT_LLVM_sysroot:
        *s_out = "DW_AT_LLVM_sysroot";
        return DW_DLV_OK;
    case DW_AT_LLVM_tag_offset:
        *s_out = "DW_AT_LLVM_tag_offset";
        return DW_DLV_OK;
    case DW_AT_LLVM_apinotes:
        *s_out = "DW_AT_LLVM_apinotes";
        return DW_DLV_OK;
    case DW_AT_LLVM_active_lane:
        *s_out = "DW_AT_LLVM_active_lane";
        return DW_DLV_OK;
    case DW_AT_LLVM_augmentation:
        *s_out = "DW_AT_LLVM_augmentation";
        return DW_DLV_OK;
    case DW_AT_LLVM_lanes:
        *s_out = "DW_AT_LLVM_lanes";
        return DW_DLV_OK;
    case DW_AT_LLVM_lane_pc:
        *s_out = "DW_AT_LLVM_lane_pc";
        return DW_DLV_OK;
    case DW_AT_LLVM_vector_size:
        *s_out = "DW_AT_LLVM_vector_size";
        return DW_DLV_OK;
    case DW_AT_APPLE_optimized:
        *s_out = "DW_AT_APPLE_optimized";
        return DW_DLV_OK;
    case DW_AT_APPLE_flags:
        *s_out = "DW_AT_APPLE_flags";
        return DW_DLV_OK;
    case DW_AT_APPLE_isa:
        *s_out = "DW_AT_APPLE_isa";
        return DW_DLV_OK;
    case DW_AT_APPLE_block:
        *s_out = "DW_AT_APPLE_block";
        return DW_DLV_OK;
    case DW_AT_APPLE_major_runtime_vers:
        *s_out = "DW_AT_APPLE_major_runtime_vers";
        return DW_DLV_OK;
    case DW_AT_APPLE_runtime_class:
        *s_out = "DW_AT_APPLE_runtime_class";
        return DW_DLV_OK;
    case DW_AT_APPLE_omit_frame_ptr:
        *s_out = "DW_AT_APPLE_omit_frame_ptr";
        return DW_DLV_OK;
    case DW_AT_APPLE_property_name:
        *s_out = "DW_AT_APPLE_property_name";
        return DW_DLV_OK;
    case DW_AT_APPLE_property_getter:
        *s_out = "DW_AT_APPLE_property_getter";
        return DW_DLV_OK;
    case DW_AT_APPLE_property_setter:
        *s_out = "DW_AT_APPLE_property_setter";
        return DW_DLV_OK;
    case DW_AT_APPLE_property_attribute:
        *s_out = "DW_AT_APPLE_property_attribute";
        return DW_DLV_OK;
    case DW_AT_APPLE_objc_complete_type:
        *s_out = "DW_AT_APPLE_objc_complete_type";
        return DW_DLV_OK;
    case DW_AT_APPLE_property:
        *s_out = "DW_AT_APPLE_property";
        return DW_DLV_OK;
    case DW_AT_APPLE_objc_direct:
        *s_out = "DW_AT_APPLE_objc_direct";
        return DW_DLV_OK;
    case DW_AT_APPLE_sdk:
        *s_out = "DW_AT_APPLE_sdk";
        return DW_DLV_OK;
    case DW_AT_APPLE_origin:
        *s_out = "DW_AT_APPLE_origin";
        return DW_DLV_OK;
    case DW_AT_hi_user:
        *s_out = "DW_AT_hi_user";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_OP_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_OP_addr:
        *s_out = "DW_OP_addr";
        return DW_DLV_OK;
    case DW_OP_deref:
        *s_out = "DW_OP_deref";
        return DW_DLV_OK;
    case DW_OP_const1u:
        *s_out = "DW_OP_const1u";
        return DW_DLV_OK;
    case DW_OP_const1s:
        *s_out = "DW_OP_const1s";
        return DW_DLV_OK;
    case DW_OP_const2u:
        *s_out = "DW_OP_const2u";
        return DW_DLV_OK;
    case DW_OP_const2s:
        *s_out = "DW_OP_const2s";
        return DW_DLV_OK;
    case DW_OP_const4u:
        *s_out = "DW_OP_const4u";
        return DW_DLV_OK;
    case DW_OP_const4s:
        *s_out = "DW_OP_const4s";
        return DW_DLV_OK;
    case DW_OP_const8u:
        *s_out = "DW_OP_const8u";
        return DW_DLV_OK;
    case DW_OP_const8s:
        *s_out = "DW_OP_const8s";
        return DW_DLV_OK;
    case DW_OP_constu:
        *s_out = "DW_OP_constu";
        return DW_DLV_OK;
    case DW_OP_consts:
        *s_out = "DW_OP_consts";
        return DW_DLV_OK;
    case DW_OP_dup:
        *s_out = "DW_OP_dup";
        return DW_DLV_OK;
    case DW_OP_drop:
        *s_out = "DW_OP_drop";
        return DW_DLV_OK;
    case DW_OP_over:
        *s_out = "DW_OP_over";
        return DW_DLV_OK;
    case DW_OP_pick:
        *s_out = "DW_OP_pick";
        return DW_DLV_OK;
    case DW_OP_swap:
        *s_out = "DW_OP_swap";
        return DW_DLV_OK;
    case DW_OP_rot:
        *s_out = "DW_OP_rot";
        return DW_DLV_OK;
    case DW_OP_xderef:
        *s_out = "DW_OP_xderef";
        return DW_DLV_OK;
    case DW_OP_abs:
        *s_out = "DW_OP_abs";
        return DW_DLV_OK;
    case DW_OP_and:
        *s_out = "DW_OP_and";
        return DW_DLV_OK;
    case DW_OP_div:
        *s_out = "DW_OP_div";
        return DW_DLV_OK;
    case DW_OP_minus:
        *s_out = "DW_OP_minus";
        return DW_DLV_OK;
    case DW_OP_mod:
        *s_out = "DW_OP_mod";
        return DW_DLV_OK;
    case DW_OP_mul:
        *s_out = "DW_OP_mul";
        return DW_DLV_OK;
    case DW_OP_neg:
        *s_out = "DW_OP_neg";
        return DW_DLV_OK;
    case DW_OP_not:
        *s_out = "DW_OP_not";
        return DW_DLV_OK;
    case DW_OP_or:
        *s_out = "DW_OP_or";
        return DW_DLV_OK;
    case DW_OP_plus:
        *s_out = "DW_OP_plus";
        return DW_DLV_OK;
    case DW_OP_plus_uconst:
        *s_out = "DW_OP_plus_uconst";
        return DW_DLV_OK;
    case DW_OP_shl:
        *s_out = "DW_OP_shl";
        return DW_DLV_OK;
    case DW_OP_shr:
        *s_out = "DW_OP_shr";
        return DW_DLV_OK;
    case DW_OP_shra:
        *s_out = "DW_OP_shra";
        return DW_DLV_OK;
    case DW_OP_xor:
        *s_out = "DW_OP_xor";
        return DW_DLV_OK;
    case DW_OP_bra:
        *s_out = "DW_OP_bra";
        return DW_DLV_OK;
    case DW_OP_eq:
        *s_out = "DW_OP_eq";
        return DW_DLV_OK;
    case DW_OP_ge:
        *s_out = "DW_OP_ge";
        return DW_DLV_OK;
    case DW_OP_gt:
        *s_out = "DW_OP_gt";
        return DW_DLV_OK;
    case DW_OP_le:
        *s_out = "DW_OP_le";
        return DW_DLV_OK;
    case DW_OP_lt:
        *s_out = "DW_OP_lt";
        return DW_DLV_OK;
    case DW_OP_ne:
        *s_out = "DW_OP_ne";
        return DW_DLV_OK;
    case DW_OP_skip:
        *s_out = "DW_OP_skip";
        return DW_DLV_OK;
    case DW_OP_lit0:
        *s_out = "DW_OP_lit0";
        return DW_DLV_OK;
    case DW_OP_lit1:
        *s_out = "DW_OP_lit1";
        return DW_DLV_OK;
    case DW_OP_lit2:
        *s_out = "DW_OP_lit2";
        return DW_DLV_OK;
    case DW_OP_lit3:
        *s_out = "DW_OP_lit3";
        return DW_DLV_OK;
    case DW_OP_lit4:
        *s_out = "DW_OP_lit4";
        return DW_DLV_OK;
    case DW_OP_lit5:
        *s_out = "DW_OP_lit5";
        return DW_DLV_OK;
    case DW_OP_lit6:
        *s_out = "DW_OP_lit6";
        return DW_DLV_OK;
    case DW_OP_lit7:
        *s_out = "DW_OP_lit7";
        return DW_DLV_OK;
    case DW_OP_lit8:
        *s_out = "DW_OP_lit8";
        return DW_DLV_OK;
    case DW_OP_lit9:
        *s_out = "DW_OP_lit9";
        return DW_DLV_OK;
    case DW_OP_lit10:
        *s_out = "DW_OP_lit10";
        return DW_DLV_OK;
    case DW_OP_lit11:
        *s_out = "DW_OP_lit11";
        return DW_DLV_OK;
    case DW_OP_lit12:
        *s_out = "DW_OP_lit12";
        return DW_DLV_OK;
    case DW_OP_lit13:
        *s_out = "DW_OP_lit13";
        return DW_DLV_OK;
    case DW_OP_lit14:
        *s_out = "DW_OP_lit14";
        return DW_DLV_OK;
    case DW_OP_lit15:
        *s_out = "DW_OP_lit15";
        return DW_DLV_OK;
    case DW_OP_lit16:
        *s_out = "DW_OP_lit16";
        return DW_DLV_OK;
    case DW_OP_lit17:
        *s_out = "DW_OP_lit17";
        return DW_DLV_OK;
    case DW_OP_lit18:
        *s_out = "DW_OP_lit18";
        return DW_DLV_OK;
    case DW_OP_lit19:
        *s_out = "DW_OP_lit19";
        return DW_DLV_OK;
    case DW_OP_lit20:
        *s_out = "DW_OP_lit20";
        return DW_DLV_OK;
    case DW_OP_lit21:
        *s_out = "DW_OP_lit21";
        return DW_DLV_OK;
    case DW_OP_lit22:
        *s_out = "DW_OP_lit22";
        return DW_DLV_OK;
    case DW_OP_lit23:
        *s_out = "DW_OP_lit23";
        return DW_DLV_OK;
    case DW_OP_lit24:
        *s_out = "DW_OP_lit24";
        return DW_DLV_OK;
    case DW_OP_lit25:
        *s_out = "DW_OP_lit25";
        return DW_DLV_OK;
    case DW_OP_lit26:
        *s_out = "DW_OP_lit26";
        return DW_DLV_OK;
    case DW_OP_lit27:
        *s_out = "DW_OP_lit27";
        return DW_DLV_OK;
    case DW_OP_lit28:
        *s_out = "DW_OP_lit28";
        return DW_DLV_OK;
    case DW_OP_lit29:
        *s_out = "DW_OP_lit29";
        return DW_DLV_OK;
    case DW_OP_lit30:
        *s_out = "DW_OP_lit30";
        return DW_DLV_OK;
    case DW_OP_lit31:
        *s_out = "DW_OP_lit31";
        return DW_DLV_OK;
    case DW_OP_reg0:
        *s_out = "DW_OP_reg0";
        return DW_DLV_OK;
    case DW_OP_reg1:
        *s_out = "DW_OP_reg1";
        return DW_DLV_OK;
    case DW_OP_reg2:
        *s_out = "DW_OP_reg2";
        return DW_DLV_OK;
    case DW_OP_reg3:
        *s_out = "DW_OP_reg3";
        return DW_DLV_OK;
    case DW_OP_reg4:
        *s_out = "DW_OP_reg4";
        return DW_DLV_OK;
    case DW_OP_reg5:
        *s_out = "DW_OP_reg5";
        return DW_DLV_OK;
    case DW_OP_reg6:
        *s_out = "DW_OP_reg6";
        return DW_DLV_OK;
    case DW_OP_reg7:
        *s_out = "DW_OP_reg7";
        return DW_DLV_OK;
    case DW_OP_reg8:
        *s_out = "DW_OP_reg8";
        return DW_DLV_OK;
    case DW_OP_reg9:
        *s_out = "DW_OP_reg9";
        return DW_DLV_OK;
    case DW_OP_reg10:
        *s_out = "DW_OP_reg10";
        return DW_DLV_OK;
    case DW_OP_reg11:
        *s_out = "DW_OP_reg11";
        return DW_DLV_OK;
    case DW_OP_reg12:
        *s_out = "DW_OP_reg12";
        return DW_DLV_OK;
    case DW_OP_reg13:
        *s_out = "DW_OP_reg13";
        return DW_DLV_OK;
    case DW_OP_reg14:
        *s_out = "DW_OP_reg14";
        return DW_DLV_OK;
    case DW_OP_reg15:
        *s_out = "DW_OP_reg15";
        return DW_DLV_OK;
    case DW_OP_reg16:
        *s_out = "DW_OP_reg16";
        return DW_DLV_OK;
    case DW_OP_reg17:
        *s_out = "DW_OP_reg17";
        return DW_DLV_OK;
    case DW_OP_reg18:
        *s_out = "DW_OP_reg18";
        return DW_DLV_OK;
    case DW_OP_reg19:
        *s_out = "DW_OP_reg19";
        return DW_DLV_OK;
    case DW_OP_reg20:
        *s_out = "DW_OP_reg20";
        return DW_DLV_OK;
    case DW_OP_reg21:
        *s_out = "DW_OP_reg21";
        return DW_DLV_OK;
    case DW_OP_reg22:
        *s_out = "DW_OP_reg22";
        return DW_DLV_OK;
    case DW_OP_reg23:
        *s_out = "DW_OP_reg23";
        return DW_DLV_OK;
    case DW_OP_reg24:
        *s_out = "DW_OP_reg24";
        return DW_DLV_OK;
    case DW_OP_reg25:
        *s_out = "DW_OP_reg25";
        return DW_DLV_OK;
    case DW_OP_reg26:
        *s_out = "DW_OP_reg26";
        return DW_DLV_OK;
    case DW_OP_reg27:
        *s_out = "DW_OP_reg27";
        return DW_DLV_OK;
    case DW_OP_reg28:
        *s_out = "DW_OP_reg28";
        return DW_DLV_OK;
    case DW_OP_reg29:
        *s_out = "DW_OP_reg29";
        return DW_DLV_OK;
    case DW_OP_reg30:
        *s_out = "DW_OP_reg30";
        return DW_DLV_OK;
    case DW_OP_reg31:
        *s_out = "DW_OP_reg31";
        return DW_DLV_OK;
    case DW_OP_breg0:
        *s_out = "DW_OP_breg0";
        return DW_DLV_OK;
    case DW_OP_breg1:
        *s_out = "DW_OP_breg1";
        return DW_DLV_OK;
    case DW_OP_breg2:
        *s_out = "DW_OP_breg2";
        return DW_DLV_OK;
    case DW_OP_breg3:
        *s_out = "DW_OP_breg3";
        return DW_DLV_OK;
    case DW_OP_breg4:
        *s_out = "DW_OP_breg4";
        return DW_DLV_OK;
    case DW_OP_breg5:
        *s_out = "DW_OP_breg5";
        return DW_DLV_OK;
    case DW_OP_breg6:
        *s_out = "DW_OP_breg6";
        return DW_DLV_OK;
    case DW_OP_breg7:
        *s_out = "DW_OP_breg7";
        return DW_DLV_OK;
    case DW_OP_breg8:
        *s_out = "DW_OP_breg8";
        return DW_DLV_OK;
    case DW_OP_breg9:
        *s_out = "DW_OP_breg9";
        return DW_DLV_OK;
    case DW_OP_breg10:
        *s_out = "DW_OP_breg10";
        return DW_DLV_OK;
    case DW_OP_breg11:
        *s_out = "DW_OP_breg11";
        return DW_DLV_OK;
    case DW_OP_breg12:
        *s_out = "DW_OP_breg12";
        return DW_DLV_OK;
    case DW_OP_breg13:
        *s_out = "DW_OP_breg13";
        return DW_DLV_OK;
    case DW_OP_breg14:
        *s_out = "DW_OP_breg14";
        return DW_DLV_OK;
    case DW_OP_breg15:
        *s_out = "DW_OP_breg15";
        return DW_DLV_OK;
    case DW_OP_breg16:
        *s_out = "DW_OP_breg16";
        return DW_DLV_OK;
    case DW_OP_breg17:
        *s_out = "DW_OP_breg17";
        return DW_DLV_OK;
    case DW_OP_breg18:
        *s_out = "DW_OP_breg18";
        return DW_DLV_OK;
    case DW_OP_breg19:
        *s_out = "DW_OP_breg19";
        return DW_DLV_OK;
    case DW_OP_breg20:
        *s_out = "DW_OP_breg20";
        return DW_DLV_OK;
    case DW_OP_breg21:
        *s_out = "DW_OP_breg21";
        return DW_DLV_OK;
    case DW_OP_breg22:
        *s_out = "DW_OP_breg22";
        return DW_DLV_OK;
    case DW_OP_breg23:
        *s_out = "DW_OP_breg23";
        return DW_DLV_OK;
    case DW_OP_breg24:
        *s_out = "DW_OP_breg24";
        return DW_DLV_OK;
    case DW_OP_breg25:
        *s_out = "DW_OP_breg25";
        return DW_DLV_OK;
    case DW_OP_breg26:
        *s_out = "DW_OP_breg26";
        return DW_DLV_OK;
    case DW_OP_breg27:
        *s_out = "DW_OP_breg27";
        return DW_DLV_OK;
    case DW_OP_breg28:
        *s_out = "DW_OP_breg28";
        return DW_DLV_OK;
    case DW_OP_breg29:
        *s_out = "DW_OP_breg29";
        return DW_DLV_OK;
    case DW_OP_breg30:
        *s_out = "DW_OP_breg30";
        return DW_DLV_OK;
    case DW_OP_breg31:
        *s_out = "DW_OP_breg31";
        return DW_DLV_OK;
    case DW_OP_regx:
        *s_out = "DW_OP_regx";
        return DW_DLV_OK;
    case DW_OP_fbreg:
        *s_out = "DW_OP_fbreg";
        return DW_DLV_OK;
    case DW_OP_bregx:
        *s_out = "DW_OP_bregx";
        return DW_DLV_OK;
    case DW_OP_piece:
        *s_out = "DW_OP_piece";
        return DW_DLV_OK;
    case DW_OP_deref_size:
        *s_out = "DW_OP_deref_size";
        return DW_DLV_OK;
    case DW_OP_xderef_size:
        *s_out = "DW_OP_xderef_size";
        return DW_DLV_OK;
    case DW_OP_nop:
        *s_out = "DW_OP_nop";
        return DW_DLV_OK;
    case DW_OP_push_object_address:
        *s_out = "DW_OP_push_object_address";
        return DW_DLV_OK;
    case DW_OP_call2:
        *s_out = "DW_OP_call2";
        return DW_DLV_OK;
    case DW_OP_call4:
        *s_out = "DW_OP_call4";
        return DW_DLV_OK;
    case DW_OP_call_ref:
        *s_out = "DW_OP_call_ref";
        return DW_DLV_OK;
    case DW_OP_form_tls_address:
        *s_out = "DW_OP_form_tls_address";
        return DW_DLV_OK;
    case DW_OP_call_frame_cfa:
        *s_out = "DW_OP_call_frame_cfa";
        return DW_DLV_OK;
    case DW_OP_bit_piece:
        *s_out = "DW_OP_bit_piece";
        return DW_DLV_OK;
    case DW_OP_implicit_value:
        *s_out = "DW_OP_implicit_value";
        return DW_DLV_OK;
    case DW_OP_stack_value:
        *s_out = "DW_OP_stack_value";
        return DW_DLV_OK;
    case DW_OP_implicit_pointer:
        *s_out = "DW_OP_implicit_pointer";
        return DW_DLV_OK;
    case DW_OP_addrx:
        *s_out = "DW_OP_addrx";
        return DW_DLV_OK;
    case DW_OP_constx:
        *s_out = "DW_OP_constx";
        return DW_DLV_OK;
    case DW_OP_entry_value:
        *s_out = "DW_OP_entry_value";
        return DW_DLV_OK;
    case DW_OP_const_type:
        *s_out = "DW_OP_const_type";
        return DW_DLV_OK;
    case DW_OP_regval_type:
        *s_out = "DW_OP_regval_type";
        return DW_DLV_OK;
    case DW_OP_deref_type:
        *s_out = "DW_OP_deref_type";
        return DW_DLV_OK;
    case DW_OP_xderef_type:
        *s_out = "DW_OP_xderef_type";
        return DW_DLV_OK;
    case DW_OP_convert:
        *s_out = "DW_OP_convert";
        return DW_DLV_OK;
    case DW_OP_reinterpret:
        *s_out = "DW_OP_reinterpret";
        return DW_DLV_OK;
    case DW_OP_GNU_push_tls_address:
        *s_out = "DW_OP_GNU_push_tls_address";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0xe0. DW_OP_lo_user */
    /*  Skipping alternate spelling of value
        0xe0. DW_OP_HP_unknown */
    case DW_OP_LLVM_form_aspace_address:
        *s_out = "DW_OP_LLVM_form_aspace_address";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0xe1. DW_OP_HP_is_value */
    case DW_OP_LLVM_push_lane:
        *s_out = "DW_OP_LLVM_push_lane";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0xe2. DW_OP_HP_fltconst4 */
    case DW_OP_LLVM_offset:
        *s_out = "DW_OP_LLVM_offset";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0xe3. DW_OP_HP_fltconst8 */
    case DW_OP_LLVM_offset_uconst:
        *s_out = "DW_OP_LLVM_offset_uconst";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0xe4. DW_OP_HP_mod_range */
    case DW_OP_LLVM_bit_offset:
        *s_out = "DW_OP_LLVM_bit_offset";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0xe5. DW_OP_HP_unmod_range */
    case DW_OP_LLVM_call_frame_entry_reg:
        *s_out = "DW_OP_LLVM_call_frame_entry_reg";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0xe6. DW_OP_HP_tls */
    case DW_OP_LLVM_undefined:
        *s_out = "DW_OP_LLVM_undefined";
        return DW_DLV_OK;
    case DW_OP_LLVM_aspace_bregx:
        *s_out = "DW_OP_LLVM_aspace_bregx";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0xe8. DW_OP_INTEL_bit_piece */
    case DW_OP_LLVM_aspace_implicit_pointer:
        *s_out = "DW_OP_LLVM_aspace_implicit_pointer";
        return DW_DLV_OK;
    case DW_OP_LLVM_piece_end:
        *s_out = "DW_OP_LLVM_piece_end";
        return DW_DLV_OK;
    case DW_OP_LLVM_extend:
        *s_out = "DW_OP_LLVM_extend";
        return DW_DLV_OK;
    case DW_OP_LLVM_select_bit_piece:
        *s_out = "DW_OP_LLVM_select_bit_piece";
        return DW_DLV_OK;
    case DW_OP_WASM_location:
        *s_out = "DW_OP_WASM_location";
        return DW_DLV_OK;
    case DW_OP_WASM_location_int:
        *s_out = "DW_OP_WASM_location_int";
        return DW_DLV_OK;
    case DW_OP_GNU_uninit:
        *s_out = "DW_OP_GNU_uninit";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0xf0. DW_OP_APPLE_uninit */
    case DW_OP_GNU_encoded_addr:
        *s_out = "DW_OP_GNU_encoded_addr";
        return DW_DLV_OK;
    case DW_OP_GNU_implicit_pointer:
        *s_out = "DW_OP_GNU_implicit_pointer";
        return DW_DLV_OK;
    case DW_OP_GNU_entry_value:
        *s_out = "DW_OP_GNU_entry_value";
        return DW_DLV_OK;
    case DW_OP_GNU_const_type:
        *s_out = "DW_OP_GNU_const_type";
        return DW_DLV_OK;
    case DW_OP_GNU_regval_type:
        *s_out = "DW_OP_GNU_regval_type";
        return DW_DLV_OK;
    case DW_OP_GNU_deref_type:
        *s_out = "DW_OP_GNU_deref_type";
        return DW_DLV_OK;
    case DW_OP_GNU_convert:
        *s_out = "DW_OP_GNU_convert";
        return DW_DLV_OK;
    case DW_OP_PGI_omp_thread_num:
        *s_out = "DW_OP_PGI_omp_thread_num";
        return DW_DLV_OK;
    case DW_OP_GNU_reinterpret:
        *s_out = "DW_OP_GNU_reinterpret";
        return DW_DLV_OK;
    case DW_OP_GNU_parameter_ref:
        *s_out = "DW_OP_GNU_parameter_ref";
        return DW_DLV_OK;
    case DW_OP_GNU_addr_index:
        *s_out = "DW_OP_GNU_addr_index";
        return DW_DLV_OK;
    case DW_OP_GNU_const_index:
        *s_out = "DW_OP_GNU_const_index";
        return DW_DLV_OK;
    case DW_OP_GNU_variable_value:
        *s_out = "DW_OP_GNU_variable_value";
        return DW_DLV_OK;
    case DW_OP_hi_user:
        *s_out = "DW_OP_hi_user";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_ATE_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_ATE_address:
        *s_out = "DW_ATE_address";
        return DW_DLV_OK;
    case DW_ATE_boolean:
        *s_out = "DW_ATE_boolean";
        return DW_DLV_OK;
    case DW_ATE_complex_float:
        *s_out = "DW_ATE_complex_float";
        return DW_DLV_OK;
    case DW_ATE_float:
        *s_out = "DW_ATE_float";
        return DW_DLV_OK;
    case DW_ATE_signed:
        *s_out = "DW_ATE_signed";
        return DW_DLV_OK;
    case DW_ATE_signed_char:
        *s_out = "DW_ATE_signed_char";
        return DW_DLV_OK;
    case DW_ATE_unsigned:
        *s_out = "DW_ATE_unsigned";
        return DW_DLV_OK;
    case DW_ATE_unsigned_char:
        *s_out = "DW_ATE_unsigned_char";
        return DW_DLV_OK;
    case DW_ATE_imaginary_float:
        *s_out = "DW_ATE_imaginary_float";
        return DW_DLV_OK;
    case DW_ATE_packed_decimal:
        *s_out = "DW_ATE_packed_decimal";
        return DW_DLV_OK;
    case DW_ATE_numeric_string:
        *s_out = "DW_ATE_numeric_string";
        return DW_DLV_OK;
    case DW_ATE_edited:
        *s_out = "DW_ATE_edited";
        return DW_DLV_OK;
    case DW_ATE_signed_fixed:
        *s_out = "DW_ATE_signed_fixed";
        return DW_DLV_OK;
    case DW_ATE_unsigned_fixed:
        *s_out = "DW_ATE_unsigned_fixed";
        return DW_DLV_OK;
    case DW_ATE_decimal_float:
        *s_out = "DW_ATE_decimal_float";
        return DW_DLV_OK;
    case DW_ATE_UTF:
        *s_out = "DW_ATE_UTF";
        return DW_DLV_OK;
    case DW_ATE_UCS:
        *s_out = "DW_ATE_UCS";
        return DW_DLV_OK;
    case DW_ATE_ASCII:
        *s_out = "DW_ATE_ASCII";
        return DW_DLV_OK;
    case DW_ATE_ALTIUM_fract:
        *s_out = "DW_ATE_ALTIUM_fract";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x80. DW_ATE_lo_user */
    /*  Skipping alternate spelling of value
        0x80. DW_ATE_HP_float80 */
    case DW_ATE_ALTIUM_accum:
        *s_out = "DW_ATE_ALTIUM_accum";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x81. DW_ATE_HP_complex_float80 */
    case DW_ATE_HP_float128:
        *s_out = "DW_ATE_HP_float128";
        return DW_DLV_OK;
    case DW_ATE_HP_complex_float128:
        *s_out = "DW_ATE_HP_complex_float128";
        return DW_DLV_OK;
    case DW_ATE_HP_floathpintel:
        *s_out = "DW_ATE_HP_floathpintel";
        return DW_DLV_OK;
    case DW_ATE_HP_imaginary_float80:
        *s_out = "DW_ATE_HP_imaginary_float80";
        return DW_DLV_OK;
    case DW_ATE_HP_imaginary_float128:
        *s_out = "DW_ATE_HP_imaginary_float128";
        return DW_DLV_OK;
    case DW_ATE_HP_VAX_float:
        *s_out = "DW_ATE_HP_VAX_float";
        return DW_DLV_OK;
    case DW_ATE_HP_VAX_float_d:
        *s_out = "DW_ATE_HP_VAX_float_d";
        return DW_DLV_OK;
    case DW_ATE_HP_packed_decimal:
        *s_out = "DW_ATE_HP_packed_decimal";
        return DW_DLV_OK;
    case DW_ATE_HP_zoned_decimal:
        *s_out = "DW_ATE_HP_zoned_decimal";
        return DW_DLV_OK;
    case DW_ATE_HP_edited:
        *s_out = "DW_ATE_HP_edited";
        return DW_DLV_OK;
    case DW_ATE_HP_signed_fixed:
        *s_out = "DW_ATE_HP_signed_fixed";
        return DW_DLV_OK;
    case DW_ATE_HP_unsigned_fixed:
        *s_out = "DW_ATE_HP_unsigned_fixed";
        return DW_DLV_OK;
    case DW_ATE_HP_VAX_complex_float:
        *s_out = "DW_ATE_HP_VAX_complex_float";
        return DW_DLV_OK;
    case DW_ATE_HP_VAX_complex_float_d:
        *s_out = "DW_ATE_HP_VAX_complex_float_d";
        return DW_DLV_OK;
    case DW_ATE_SUN_interval_float:
        *s_out = "DW_ATE_SUN_interval_float";
        return DW_DLV_OK;
    case DW_ATE_SUN_imaginary_float:
        *s_out = "DW_ATE_SUN_imaginary_float";
        return DW_DLV_OK;
    case DW_ATE_hi_user:
        *s_out = "DW_ATE_hi_user";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_DEFAULTED_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_DEFAULTED_no:
        *s_out = "DW_DEFAULTED_no";
        return DW_DLV_OK;
    case DW_DEFAULTED_in_class:
        *s_out = "DW_DEFAULTED_in_class";
        return DW_DLV_OK;
    case DW_DEFAULTED_out_of_class:
        *s_out = "DW_DEFAULTED_out_of_class";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_IDX_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_IDX_compile_unit:
        *s_out = "DW_IDX_compile_unit";
        return DW_DLV_OK;
    case DW_IDX_type_unit:
        *s_out = "DW_IDX_type_unit";
        return DW_DLV_OK;
    case DW_IDX_die_offset:
        *s_out = "DW_IDX_die_offset";
        return DW_DLV_OK;
    case DW_IDX_parent:
        *s_out = "DW_IDX_parent";
        return DW_DLV_OK;
    case DW_IDX_type_hash:
        *s_out = "DW_IDX_type_hash";
        return DW_DLV_OK;
    case DW_IDX_GNU_internal:
        *s_out = "DW_IDX_GNU_internal";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x2000. DW_IDX_lo_user */
    case DW_IDX_GNU_external:
        *s_out = "DW_IDX_GNU_external";
        return DW_DLV_OK;
    case DW_IDX_GNU_main:
        *s_out = "DW_IDX_GNU_main";
        return DW_DLV_OK;
    case DW_IDX_GNU_language:
        *s_out = "DW_IDX_GNU_language";
        return DW_DLV_OK;
    case DW_IDX_GNU_linkage_name:
        *s_out = "DW_IDX_GNU_linkage_name";
        return DW_DLV_OK;
    case DW_IDX_hi_user:
        *s_out = "DW_IDX_hi_user";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_LLEX_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_LLEX_end_of_list_entry:
        *s_out = "DW_LLEX_end_of_list_entry";
        return DW_DLV_OK;
    case DW_LLEX_base_address_selection_entry:
        *s_out = "DW_LLEX_base_address_selection_entry";
        return DW_DLV_OK;
    case DW_LLEX_start_end_entry:
        *s_out = "DW_LLEX_start_end_entry";
        return DW_DLV_OK;
    case DW_LLEX_start_length_entry:
        *s_out = "DW_LLEX_start_length_entry";
        return DW_DLV_OK;
    case DW_LLEX_offset_pair_entry:
        *s_out = "DW_LLEX_offset_pair_entry";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_LLE_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_LLE_end_of_list:
        *s_out = "DW_LLE_end_of_list";
        return DW_DLV_OK;
    case DW_LLE_base_addressx:
        *s_out = "DW_LLE_base_addressx";
        return DW_DLV_OK;
    case DW_LLE_startx_endx:
        *s_out = "DW_LLE_startx_endx";
        return DW_DLV_OK;
    case DW_LLE_startx_length:
        *s_out = "DW_LLE_startx_length";
        return DW_DLV_OK;
    case DW_LLE_offset_pair:
        *s_out = "DW_LLE_offset_pair";
        return DW_DLV_OK;
    case DW_LLE_default_location:
        *s_out = "DW_LLE_default_location";
        return DW_DLV_OK;
    case DW_LLE_base_address:
        *s_out = "DW_LLE_base_address";
        return DW_DLV_OK;
    case DW_LLE_start_end:
        *s_out = "DW_LLE_start_end";
        return DW_DLV_OK;
    case DW_LLE_start_length:
        *s_out = "DW_LLE_start_length";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_RLE_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_RLE_end_of_list:
        *s_out = "DW_RLE_end_of_list";
        return DW_DLV_OK;
    case DW_RLE_base_addressx:
        *s_out = "DW_RLE_base_addressx";
        return DW_DLV_OK;
    case DW_RLE_startx_endx:
        *s_out = "DW_RLE_startx_endx";
        return DW_DLV_OK;
    case DW_RLE_startx_length:
        *s_out = "DW_RLE_startx_length";
        return DW_DLV_OK;
    case DW_RLE_offset_pair:
        *s_out = "DW_RLE_offset_pair";
        return DW_DLV_OK;
    case DW_RLE_base_address:
        *s_out = "DW_RLE_base_address";
        return DW_DLV_OK;
    case DW_RLE_start_end:
        *s_out = "DW_RLE_start_end";
        return DW_DLV_OK;
    case DW_RLE_start_length:
        *s_out = "DW_RLE_start_length";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_GNUIVIS_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_GNUIVIS_global:
        *s_out = "DW_GNUIVIS_global";
        return DW_DLV_OK;
    case DW_GNUIVIS_static:
        *s_out = "DW_GNUIVIS_static";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_GNUIKIND_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_GNUIKIND_none:
        *s_out = "DW_GNUIKIND_none";
        return DW_DLV_OK;
    case DW_GNUIKIND_type:
        *s_out = "DW_GNUIKIND_type";
        return DW_DLV_OK;
    case DW_GNUIKIND_variable:
        *s_out = "DW_GNUIKIND_variable";
        return DW_DLV_OK;
    case DW_GNUIKIND_function:
        *s_out = "DW_GNUIKIND_function";
        return DW_DLV_OK;
    case DW_GNUIKIND_other:
        *s_out = "DW_GNUIKIND_other";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_UT_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_UT_compile:
        *s_out = "DW_UT_compile";
        return DW_DLV_OK;
    case DW_UT_type:
        *s_out = "DW_UT_type";
        return DW_DLV_OK;
    case DW_UT_partial:
        *s_out = "DW_UT_partial";
        return DW_DLV_OK;
    case DW_UT_skeleton:
        *s_out = "DW_UT_skeleton";
        return DW_DLV_OK;
    case DW_UT_split_compile:
        *s_out = "DW_UT_split_compile";
        return DW_DLV_OK;
    case DW_UT_split_type:
        *s_out = "DW_UT_split_type";
        return DW_DLV_OK;
    case DW_UT_lo_user:
        *s_out = "DW_UT_lo_user";
        return DW_DLV_OK;
    case DW_UT_hi_user:
        *s_out = "DW_UT_hi_user";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_SECT_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_SECT_INFO:
        *s_out = "DW_SECT_INFO";
        return DW_DLV_OK;
    case DW_SECT_TYPES:
        *s_out = "DW_SECT_TYPES";
        return DW_DLV_OK;
    case DW_SECT_ABBREV:
        *s_out = "DW_SECT_ABBREV";
        return DW_DLV_OK;
    case DW_SECT_LINE:
        *s_out = "DW_SECT_LINE";
        return DW_DLV_OK;
    case DW_SECT_LOCLISTS:
        *s_out = "DW_SECT_LOCLISTS";
        return DW_DLV_OK;
    case DW_SECT_STR_OFFSETS:
        *s_out = "DW_SECT_STR_OFFSETS";
        return DW_DLV_OK;
    case DW_SECT_MACRO:
        *s_out = "DW_SECT_MACRO";
        return DW_DLV_OK;
    case DW_SECT_RNGLISTS:
        *s_out = "DW_SECT_RNGLISTS";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_DS_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_DS_unsigned:
        *s_out = "DW_DS_unsigned";
        return DW_DLV_OK;
    case DW_DS_leading_overpunch:
        *s_out = "DW_DS_leading_overpunch";
        return DW_DLV_OK;
    case DW_DS_trailing_overpunch:
        *s_out = "DW_DS_trailing_overpunch";
        return DW_DLV_OK;
    case DW_DS_leading_separate:
        *s_out = "DW_DS_leading_separate";
        return DW_DLV_OK;
    case DW_DS_trailing_separate:
        *s_out = "DW_DS_trailing_separate";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_END_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_END_default:
        *s_out = "DW_END_default";
        return DW_DLV_OK;
    case DW_END_big:
        *s_out = "DW_END_big";
        return DW_DLV_OK;
    case DW_END_little:
        *s_out = "DW_END_little";
        return DW_DLV_OK;
    case DW_END_lo_user:
        *s_out = "DW_END_lo_user";
        return DW_DLV_OK;
    case DW_END_hi_user:
        *s_out = "DW_END_hi_user";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_ATCF_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_ATCF_lo_user:
        *s_out = "DW_ATCF_lo_user";
        return DW_DLV_OK;
    case DW_ATCF_SUN_mop_bitfield:
        *s_out = "DW_ATCF_SUN_mop_bitfield";
        return DW_DLV_OK;
    case DW_ATCF_SUN_mop_spill:
        *s_out = "DW_ATCF_SUN_mop_spill";
        return DW_DLV_OK;
    case DW_ATCF_SUN_mop_scopy:
        *s_out = "DW_ATCF_SUN_mop_scopy";
        return DW_DLV_OK;
    case DW_ATCF_SUN_func_start:
        *s_out = "DW_ATCF_SUN_func_start";
        return DW_DLV_OK;
    case DW_ATCF_SUN_end_ctors:
        *s_out = "DW_ATCF_SUN_end_ctors";
        return DW_DLV_OK;
    case DW_ATCF_SUN_branch_target:
        *s_out = "DW_ATCF_SUN_branch_target";
        return DW_DLV_OK;
    case DW_ATCF_SUN_mop_stack_probe:
        *s_out = "DW_ATCF_SUN_mop_stack_probe";
        return DW_DLV_OK;
    case DW_ATCF_SUN_func_epilog:
        *s_out = "DW_ATCF_SUN_func_epilog";
        return DW_DLV_OK;
    case DW_ATCF_hi_user:
        *s_out = "DW_ATCF_hi_user";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_ACCESS_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_ACCESS_public:
        *s_out = "DW_ACCESS_public";
        return DW_DLV_OK;
    case DW_ACCESS_protected:
        *s_out = "DW_ACCESS_protected";
        return DW_DLV_OK;
    case DW_ACCESS_private:
        *s_out = "DW_ACCESS_private";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_VIS_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_VIS_local:
        *s_out = "DW_VIS_local";
        return DW_DLV_OK;
    case DW_VIS_exported:
        *s_out = "DW_VIS_exported";
        return DW_DLV_OK;
    case DW_VIS_qualified:
        *s_out = "DW_VIS_qualified";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_VIRTUALITY_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_VIRTUALITY_none:
        *s_out = "DW_VIRTUALITY_none";
        return DW_DLV_OK;
    case DW_VIRTUALITY_virtual:
        *s_out = "DW_VIRTUALITY_virtual";
        return DW_DLV_OK;
    case DW_VIRTUALITY_pure_virtual:
        *s_out = "DW_VIRTUALITY_pure_virtual";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_LANG_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_LANG_C89:
        *s_out = "DW_LANG_C89";
        return DW_DLV_OK;
    case DW_LANG_C:
        *s_out = "DW_LANG_C";
        return DW_DLV_OK;
    case DW_LANG_Ada83:
        *s_out = "DW_LANG_Ada83";
        return DW_DLV_OK;
    case DW_LANG_C_plus_plus:
        *s_out = "DW_LANG_C_plus_plus";
        return DW_DLV_OK;
    case DW_LANG_Cobol74:
        *s_out = "DW_LANG_Cobol74";
        return DW_DLV_OK;
    case DW_LANG_Cobol85:
        *s_out = "DW_LANG_Cobol85";
        return DW_DLV_OK;
    case DW_LANG_Fortran77:
        *s_out = "DW_LANG_Fortran77";
        return DW_DLV_OK;
    case DW_LANG_Fortran90:
        *s_out = "DW_LANG_Fortran90";
        return DW_DLV_OK;
    case DW_LANG_Pascal83:
        *s_out = "DW_LANG_Pascal83";
        return DW_DLV_OK;
    case DW_LANG_Modula2:
        *s_out = "DW_LANG_Modula2";
        return DW_DLV_OK;
    case DW_LANG_Java:
        *s_out = "DW_LANG_Java";
        return DW_DLV_OK;
    case DW_LANG_C99:
        *s_out = "DW_LANG_C99";
        return DW_DLV_OK;
    case DW_LANG_Ada95:
        *s_out = "DW_LANG_Ada95";
        return DW_DLV_OK;
    case DW_LANG_Fortran95:
        *s_out = "DW_LANG_Fortran95";
        return DW_DLV_OK;
    case DW_LANG_PLI:
        *s_out = "DW_LANG_PLI";
        return DW_DLV_OK;
    case DW_LANG_ObjC:
        *s_out = "DW_LANG_ObjC";
        return DW_DLV_OK;
    case DW_LANG_ObjC_plus_plus:
        *s_out = "DW_LANG_ObjC_plus_plus";
        return DW_DLV_OK;
    case DW_LANG_UPC:
        *s_out = "DW_LANG_UPC";
        return DW_DLV_OK;
    case DW_LANG_D:
        *s_out = "DW_LANG_D";
        return DW_DLV_OK;
    case DW_LANG_Python:
        *s_out = "DW_LANG_Python";
        return DW_DLV_OK;
    case DW_LANG_OpenCL:
        *s_out = "DW_LANG_OpenCL";
        return DW_DLV_OK;
    case DW_LANG_Go:
        *s_out = "DW_LANG_Go";
        return DW_DLV_OK;
    case DW_LANG_Modula3:
        *s_out = "DW_LANG_Modula3";
        return DW_DLV_OK;
    case DW_LANG_Haskel:
        *s_out = "DW_LANG_Haskel";
        return DW_DLV_OK;
    case DW_LANG_C_plus_plus_03:
        *s_out = "DW_LANG_C_plus_plus_03";
        return DW_DLV_OK;
    case DW_LANG_C_plus_plus_11:
        *s_out = "DW_LANG_C_plus_plus_11";
        return DW_DLV_OK;
    case DW_LANG_OCaml:
        *s_out = "DW_LANG_OCaml";
        return DW_DLV_OK;
    case DW_LANG_Rust:
        *s_out = "DW_LANG_Rust";
        return DW_DLV_OK;
    case DW_LANG_C11:
        *s_out = "DW_LANG_C11";
        return DW_DLV_OK;
    case DW_LANG_Swift:
        *s_out = "DW_LANG_Swift";
        return DW_DLV_OK;
    case DW_LANG_Julia:
        *s_out = "DW_LANG_Julia";
        return DW_DLV_OK;
    case DW_LANG_Dylan:
        *s_out = "DW_LANG_Dylan";
        return DW_DLV_OK;
    case DW_LANG_C_plus_plus_14:
        *s_out = "DW_LANG_C_plus_plus_14";
        return DW_DLV_OK;
    case DW_LANG_Fortran03:
        *s_out = "DW_LANG_Fortran03";
        return DW_DLV_OK;
    case DW_LANG_Fortran08:
        *s_out = "DW_LANG_Fortran08";
        return DW_DLV_OK;
    case DW_LANG_RenderScript:
        *s_out = "DW_LANG_RenderScript";
        return DW_DLV_OK;
    case DW_LANG_BLISS:
        *s_out = "DW_LANG_BLISS";
        return DW_DLV_OK;
    case DW_LANG_Kotlin:
        *s_out = "DW_LANG_Kotlin";
        return DW_DLV_OK;
    case DW_LANG_Zig:
        *s_out = "DW_LANG_Zig";
        return DW_DLV_OK;
    case DW_LANG_Crystal:
        *s_out = "DW_LANG_Crystal";
        return DW_DLV_OK;
    case DW_LANG_C_plus_plus_17:
        *s_out = "DW_LANG_C_plus_plus_17";
        return DW_DLV_OK;
    case DW_LANG_C_plus_plus_20:
        *s_out = "DW_LANG_C_plus_plus_20";
        return DW_DLV_OK;
    case DW_LANG_C17:
        *s_out = "DW_LANG_C17";
        return DW_DLV_OK;
    case DW_LANG_Fortran18:
        *s_out = "DW_LANG_Fortran18";
        return DW_DLV_OK;
    case DW_LANG_Ada2005:
        *s_out = "DW_LANG_Ada2005";
        return DW_DLV_OK;
    case DW_LANG_Ada2012:
        *s_out = "DW_LANG_Ada2012";
        return DW_DLV_OK;
    case DW_LANG_HIP:
        *s_out = "DW_LANG_HIP";
        return DW_DLV_OK;
    case DW_LANG_Assembly:
        *s_out = "DW_LANG_Assembly";
        return DW_DLV_OK;
    case DW_LANG_C_sharp:
        *s_out = "DW_LANG_C_sharp";
        return DW_DLV_OK;
    case DW_LANG_Mojo:
        *s_out = "DW_LANG_Mojo";
        return DW_DLV_OK;
    case DW_LANG_GLSL:
        *s_out = "DW_LANG_GLSL";
        return DW_DLV_OK;
    case DW_LANG_GLSL_ES:
        *s_out = "DW_LANG_GLSL_ES";
        return DW_DLV_OK;
    case DW_LANG_HLSL:
        *s_out = "DW_LANG_HLSL";
        return DW_DLV_OK;
    case DW_LANG_OpenCL_CPP:
        *s_out = "DW_LANG_OpenCL_CPP";
        return DW_DLV_OK;
    case DW_LANG_CPP_for_OpenCL:
        *s_out = "DW_LANG_CPP_for_OpenCL";
        return DW_DLV_OK;
    case DW_LANG_SYCL:
        *s_out = "DW_LANG_SYCL";
        return DW_DLV_OK;
    case DW_LANG_Ruby:
        *s_out = "DW_LANG_Ruby";
        return DW_DLV_OK;
    case DW_LANG_Move:
        *s_out = "DW_LANG_Move";
        return DW_DLV_OK;
    case DW_LANG_Hylo:
        *s_out = "DW_LANG_Hylo";
        return DW_DLV_OK;
    case DW_LANG_V:
        *s_out = "DW_LANG_V";
        return DW_DLV_OK;
    case DW_LANG_Algol68:
        *s_out = "DW_LANG_Algol68";
        return DW_DLV_OK;
    case DW_LANG_lo_user:
        *s_out = "DW_LANG_lo_user";
        return DW_DLV_OK;
    case DW_LANG_Mips_Assembler:
        *s_out = "DW_LANG_Mips_Assembler";
        return DW_DLV_OK;
    case DW_LANG_Upc:
        *s_out = "DW_LANG_Upc";
        return DW_DLV_OK;
    case DW_LANG_GOOGLE_RenderScript:
        *s_out = "DW_LANG_GOOGLE_RenderScript";
        return DW_DLV_OK;
    case DW_LANG_SUN_Assembler:
        *s_out = "DW_LANG_SUN_Assembler";
        return DW_DLV_OK;
    case DW_LANG_ALTIUM_Assembler:
        *s_out = "DW_LANG_ALTIUM_Assembler";
        return DW_DLV_OK;
    case DW_LANG_BORLAND_Delphi:
        *s_out = "DW_LANG_BORLAND_Delphi";
        return DW_DLV_OK;
    case DW_LANG_hi_user:
        *s_out = "DW_LANG_hi_user";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_LNAME_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_LNAME_Ada:
        *s_out = "DW_LNAME_Ada";
        return DW_DLV_OK;
    case DW_LNAME_BLISS:
        *s_out = "DW_LNAME_BLISS";
        return DW_DLV_OK;
    case DW_LNAME_C:
        *s_out = "DW_LNAME_C";
        return DW_DLV_OK;
    case DW_LNAME_C_plus_plus:
        *s_out = "DW_LNAME_C_plus_plus";
        return DW_DLV_OK;
    case DW_LNAME_Cobol:
        *s_out = "DW_LNAME_Cobol";
        return DW_DLV_OK;
    case DW_LNAME_Crystal:
        *s_out = "DW_LNAME_Crystal";
        return DW_DLV_OK;
    case DW_LNAME_D:
        *s_out = "DW_LNAME_D";
        return DW_DLV_OK;
    case DW_LNAME_Dylan:
        *s_out = "DW_LNAME_Dylan";
        return DW_DLV_OK;
    case DW_LNAME_Fortran:
        *s_out = "DW_LNAME_Fortran";
        return DW_DLV_OK;
    case DW_LNAME_Go:
        *s_out = "DW_LNAME_Go";
        return DW_DLV_OK;
    case DW_LNAME_Haskell:
        *s_out = "DW_LNAME_Haskell";
        return DW_DLV_OK;
    case DW_LNAME_Java:
        *s_out = "DW_LNAME_Java";
        return DW_DLV_OK;
    case DW_LNAME_Julia:
        *s_out = "DW_LNAME_Julia";
        return DW_DLV_OK;
    case DW_LNAME_Kotlin:
        *s_out = "DW_LNAME_Kotlin";
        return DW_DLV_OK;
    case DW_LNAME_Modula2:
        *s_out = "DW_LNAME_Modula2";
        return DW_DLV_OK;
    case DW_LNAME_Modula3:
        *s_out = "DW_LNAME_Modula3";
        return DW_DLV_OK;
    case DW_LNAME_ObjC:
        *s_out = "DW_LNAME_ObjC";
        return DW_DLV_OK;
    case DW_LNAME_ObjC_plus_plus:
        *s_out = "DW_LNAME_ObjC_plus_plus";
        return DW_DLV_OK;
    case DW_LNAME_OCaml:
        *s_out = "DW_LNAME_OCaml";
        return DW_DLV_OK;
    case DW_LNAME_OpenCL_C:
        *s_out = "DW_LNAME_OpenCL_C";
        return DW_DLV_OK;
    case DW_LNAME_Pascal:
        *s_out = "DW_LNAME_Pascal";
        return DW_DLV_OK;
    case DW_LNAME_PLI:
        *s_out = "DW_LNAME_PLI";
        return DW_DLV_OK;
    case DW_LNAME_Python:
        *s_out = "DW_LNAME_Python";
        return DW_DLV_OK;
    case DW_LNAME_RenderScript:
        *s_out = "DW_LNAME_RenderScript";
        return DW_DLV_OK;
    case DW_LNAME_Rust:
        *s_out = "DW_LNAME_Rust";
        return DW_DLV_OK;
    case DW_LNAME_Swift:
        *s_out = "DW_LNAME_Swift";
        return DW_DLV_OK;
    case DW_LNAME_UPC:
        *s_out = "DW_LNAME_UPC";
        return DW_DLV_OK;
    case DW_LNAME_Zig:
        *s_out = "DW_LNAME_Zig";
        return DW_DLV_OK;
    case DW_LNAME_Assembly:
        *s_out = "DW_LNAME_Assembly";
        return DW_DLV_OK;
    case DW_LNAME_C_sharp:
        *s_out = "DW_LNAME_C_sharp";
        return DW_DLV_OK;
    case DW_LNAME_Mojo:
        *s_out = "DW_LNAME_Mojo";
        return DW_DLV_OK;
    case DW_LNAME_GLSL:
        *s_out = "DW_LNAME_GLSL";
        return DW_DLV_OK;
    case DW_LNAME_GLSLES:
        *s_out = "DW_LNAME_GLSLES";
        return DW_DLV_OK;
    case DW_LNAME_HLSL:
        *s_out = "DW_LNAME_HLSL";
        return DW_DLV_OK;
    case DW_LNAME_OpenCL_CPP:
        *s_out = "DW_LNAME_OpenCL_CPP";
        return DW_DLV_OK;
    case DW_LNAME_CPP_for_OpenCL:
        *s_out = "DW_LNAME_CPP_for_OpenCL";
        return DW_DLV_OK;
    case DW_LNAME_SYCL:
        *s_out = "DW_LNAME_SYCL";
        return DW_DLV_OK;
    case DW_LNAME_Ruby:
        *s_out = "DW_LNAME_Ruby";
        return DW_DLV_OK;
    case DW_LNAME_Move:
        *s_out = "DW_LNAME_Move";
        return DW_DLV_OK;
    case DW_LNAME_Hylo:
        *s_out = "DW_LNAME_Hylo";
        return DW_DLV_OK;
    case DW_LNAME_HIP:
        *s_out = "DW_LNAME_HIP";
        return DW_DLV_OK;
    case DW_LNAME_Odin:
        *s_out = "DW_LNAME_Odin";
        return DW_DLV_OK;
    case DW_LNAME_P4:
        *s_out = "DW_LNAME_P4";
        return DW_DLV_OK;
    case DW_LNAME_Metal:
        *s_out = "DW_LNAME_Metal";
        return DW_DLV_OK;
    case DW_LNAME_V:
        *s_out = "DW_LNAME_V";
        return DW_DLV_OK;
    case DW_LNAME_Algol68:
        *s_out = "DW_LNAME_Algol68";
        return DW_DLV_OK;
    case DW_LNAME_Nim:
        *s_out = "DW_LNAME_Nim";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_ID_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_ID_case_sensitive:
        *s_out = "DW_ID_case_sensitive";
        return DW_DLV_OK;
    case DW_ID_up_case:
        *s_out = "DW_ID_up_case";
        return DW_DLV_OK;
    case DW_ID_down_case:
        *s_out = "DW_ID_down_case";
        return DW_DLV_OK;
    case DW_ID_case_insensitive:
        *s_out = "DW_ID_case_insensitive";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_CC_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_CC_normal:
        *s_out = "DW_CC_normal";
        return DW_DLV_OK;
    case DW_CC_program:
        *s_out = "DW_CC_program";
        return DW_DLV_OK;
    case DW_CC_nocall:
        *s_out = "DW_CC_nocall";
        return DW_DLV_OK;
    case DW_CC_pass_by_reference:
        *s_out = "DW_CC_pass_by_reference";
        return DW_DLV_OK;
    case DW_CC_pass_by_value:
        *s_out = "DW_CC_pass_by_value";
        return DW_DLV_OK;
    case DW_CC_GNU_renesas_sh:
        *s_out = "DW_CC_GNU_renesas_sh";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x40. DW_CC_lo_user */
    case DW_CC_GNU_borland_fastcall_i386:
        *s_out = "DW_CC_GNU_borland_fastcall_i386";
        return DW_DLV_OK;
    case DW_CC_ALTIUM_interrupt:
        *s_out = "DW_CC_ALTIUM_interrupt";
        return DW_DLV_OK;
    case DW_CC_ALTIUM_near_system_stack:
        *s_out = "DW_CC_ALTIUM_near_system_stack";
        return DW_DLV_OK;
    case DW_CC_ALTIUM_near_user_stack:
        *s_out = "DW_CC_ALTIUM_near_user_stack";
        return DW_DLV_OK;
    case DW_CC_ALTIUM_huge_user_stack:
        *s_out = "DW_CC_ALTIUM_huge_user_stack";
        return DW_DLV_OK;
    case DW_CC_GNU_BORLAND_safecall:
        *s_out = "DW_CC_GNU_BORLAND_safecall";
        return DW_DLV_OK;
    case DW_CC_GNU_BORLAND_stdcall:
        *s_out = "DW_CC_GNU_BORLAND_stdcall";
        return DW_DLV_OK;
    case DW_CC_GNU_BORLAND_pascal:
        *s_out = "DW_CC_GNU_BORLAND_pascal";
        return DW_DLV_OK;
    case DW_CC_GNU_BORLAND_msfastcall:
        *s_out = "DW_CC_GNU_BORLAND_msfastcall";
        return DW_DLV_OK;
    case DW_CC_GNU_BORLAND_msreturn:
        *s_out = "DW_CC_GNU_BORLAND_msreturn";
        return DW_DLV_OK;
    case DW_CC_GNU_BORLAND_thiscall:
        *s_out = "DW_CC_GNU_BORLAND_thiscall";
        return DW_DLV_OK;
    case DW_CC_GNU_BORLAND_fastcall:
        *s_out = "DW_CC_GNU_BORLAND_fastcall";
        return DW_DLV_OK;
    case DW_CC_LLVM_vectorcall:
        *s_out = "DW_CC_LLVM_vectorcall";
        return DW_DLV_OK;
    case DW_CC_LLVM_Win64:
        *s_out = "DW_CC_LLVM_Win64";
        return DW_DLV_OK;
    case DW_CC_LLVM_X86_64SysV:
        *s_out = "DW_CC_LLVM_X86_64SysV";
        return DW_DLV_OK;
    case DW_CC_LLVM_AAPCS:
        *s_out = "DW_CC_LLVM_AAPCS";
        return DW_DLV_OK;
    case DW_CC_LLVM_AAPCS_VFP:
        *s_out = "DW_CC_LLVM_AAPCS_VFP";
        return DW_DLV_OK;
    case DW_CC_LLVM_IntelOclBicc:
        *s_out = "DW_CC_LLVM_IntelOclBicc";
        return DW_DLV_OK;
    case DW_CC_LLVM_SpirFunction:
        *s_out = "DW_CC_LLVM_SpirFunction";
        return DW_DLV_OK;
    case DW_CC_LLVM_OpenCLKernel:
        *s_out = "DW_CC_LLVM_OpenCLKernel";
        return DW_DLV_OK;
    case DW_CC_LLVM_Swift:
        *s_out = "DW_CC_LLVM_Swift";
        return DW_DLV_OK;
    case DW_CC_LLVM_PreserveMost:
        *s_out = "DW_CC_LLVM_PreserveMost";
        return DW_DLV_OK;
    case DW_CC_LLVM_PreserveAll:
        *s_out = "DW_CC_LLVM_PreserveAll";
        return DW_DLV_OK;
    case DW_CC_LLVM_X86RegCall:
        *s_out = "DW_CC_LLVM_X86RegCall";
        return DW_DLV_OK;
    case DW_CC_GDB_IBM_OpenCL:
        *s_out = "DW_CC_GDB_IBM_OpenCL";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0xff. DW_CC_hi_user */
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_INL_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_INL_not_inlined:
        *s_out = "DW_INL_not_inlined";
        return DW_DLV_OK;
    case DW_INL_inlined:
        *s_out = "DW_INL_inlined";
        return DW_DLV_OK;
    case DW_INL_declared_not_inlined:
        *s_out = "DW_INL_declared_not_inlined";
        return DW_DLV_OK;
    case DW_INL_declared_inlined:
        *s_out = "DW_INL_declared_inlined";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_ORD_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_ORD_row_major:
        *s_out = "DW_ORD_row_major";
        return DW_DLV_OK;
    case DW_ORD_col_major:
        *s_out = "DW_ORD_col_major";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_DSC_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_DSC_label:
        *s_out = "DW_DSC_label";
        return DW_DLV_OK;
    case DW_DSC_range:
        *s_out = "DW_DSC_range";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_LNCT_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_LNCT_path:
        *s_out = "DW_LNCT_path";
        return DW_DLV_OK;
    case DW_LNCT_directory_index:
        *s_out = "DW_LNCT_directory_index";
        return DW_DLV_OK;
    case DW_LNCT_timestamp:
        *s_out = "DW_LNCT_timestamp";
        return DW_DLV_OK;
    case DW_LNCT_size:
        *s_out = "DW_LNCT_size";
        return DW_DLV_OK;
    case DW_LNCT_MD5:
        *s_out = "DW_LNCT_MD5";
        return DW_DLV_OK;
    case DW_LNCT_GNU_subprogram_name:
        *s_out = "DW_LNCT_GNU_subprogram_name";
        return DW_DLV_OK;
    case DW_LNCT_GNU_decl_file:
        *s_out = "DW_LNCT_GNU_decl_file";
        return DW_DLV_OK;
    case DW_LNCT_GNU_decl_line:
        *s_out = "DW_LNCT_GNU_decl_line";
        return DW_DLV_OK;
    case DW_LNCT_lo_user:
        *s_out = "DW_LNCT_lo_user";
        return DW_DLV_OK;
    case DW_LNCT_LLVM_source:
        *s_out = "DW_LNCT_LLVM_source";
        return DW_DLV_OK;
    case DW_LNCT_LLVM_is_MD5:
        *s_out = "DW_LNCT_LLVM_is_MD5";
        return DW_DLV_OK;
    case DW_LNCT_hi_user:
        *s_out = "DW_LNCT_hi_user";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_LNS_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_LNS_copy:
        *s_out = "DW_LNS_copy";
        return DW_DLV_OK;
    case DW_LNS_advance_pc:
        *s_out = "DW_LNS_advance_pc";
        return DW_DLV_OK;
    case DW_LNS_advance_line:
        *s_out = "DW_LNS_advance_line";
        return DW_DLV_OK;
    case DW_LNS_set_file:
        *s_out = "DW_LNS_set_file";
        return DW_DLV_OK;
    case DW_LNS_set_column:
        *s_out = "DW_LNS_set_column";
        return DW_DLV_OK;
    case DW_LNS_negate_stmt:
        *s_out = "DW_LNS_negate_stmt";
        return DW_DLV_OK;
    case DW_LNS_set_basic_block:
        *s_out = "DW_LNS_set_basic_block";
        return DW_DLV_OK;
    case DW_LNS_const_add_pc:
        *s_out = "DW_LNS_const_add_pc";
        return DW_DLV_OK;
    case DW_LNS_fixed_advance_pc:
        *s_out = "DW_LNS_fixed_advance_pc";
        return DW_DLV_OK;
    case DW_LNS_set_prologue_end:
        *s_out = "DW_LNS_set_prologue_end";
        return DW_DLV_OK;
    case DW_LNS_set_epilogue_begin:
        *s_out = "DW_LNS_set_epilogue_begin";
        return DW_DLV_OK;
    case DW_LNS_set_isa:
        *s_out = "DW_LNS_set_isa";
        return DW_DLV_OK;
    case DW_LNS_set_address_from_logical:
        *s_out = "DW_LNS_set_address_from_logical";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0xd. DW_LNS_set_subprogram */
    case DW_LNS_inlined_call:
        *s_out = "DW_LNS_inlined_call";
        return DW_DLV_OK;
    case DW_LNS_pop_context:
        *s_out = "DW_LNS_pop_context";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_LNE_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_LNE_end_sequence:
        *s_out = "DW_LNE_end_sequence";
        return DW_DLV_OK;
    case DW_LNE_set_address:
        *s_out = "DW_LNE_set_address";
        return DW_DLV_OK;
    case DW_LNE_define_file:
        *s_out = "DW_LNE_define_file";
        return DW_DLV_OK;
    case DW_LNE_set_discriminator:
        *s_out = "DW_LNE_set_discriminator";
        return DW_DLV_OK;
    case DW_LNE_HP_negate_is_UV_update:
        *s_out = "DW_LNE_HP_negate_is_UV_update";
        return DW_DLV_OK;
    case DW_LNE_HP_push_context:
        *s_out = "DW_LNE_HP_push_context";
        return DW_DLV_OK;
    case DW_LNE_HP_pop_context:
        *s_out = "DW_LNE_HP_pop_context";
        return DW_DLV_OK;
    case DW_LNE_HP_set_file_line_column:
        *s_out = "DW_LNE_HP_set_file_line_column";
        return DW_DLV_OK;
    case DW_LNE_HP_set_routine_name:
        *s_out = "DW_LNE_HP_set_routine_name";
        return DW_DLV_OK;
    case DW_LNE_HP_set_sequence:
        *s_out = "DW_LNE_HP_set_sequence";
        return DW_DLV_OK;
    case DW_LNE_HP_negate_post_semantics:
        *s_out = "DW_LNE_HP_negate_post_semantics";
        return DW_DLV_OK;
    case DW_LNE_HP_negate_function_exit:
        *s_out = "DW_LNE_HP_negate_function_exit";
        return DW_DLV_OK;
    case DW_LNE_HP_negate_front_end_logical:
        *s_out = "DW_LNE_HP_negate_front_end_logical";
        return DW_DLV_OK;
    case DW_LNE_HP_define_proc:
        *s_out = "DW_LNE_HP_define_proc";
        return DW_DLV_OK;
    case DW_LNE_HP_source_file_correlation:
        *s_out = "DW_LNE_HP_source_file_correlation";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x80. DW_LNE_lo_user */
    case DW_LNE_hi_user:
        *s_out = "DW_LNE_hi_user";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_ISA_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_ISA_UNKNOWN:
        *s_out = "DW_ISA_UNKNOWN";
        return DW_DLV_OK;
    case DW_ISA_ARM_thumb:
        *s_out = "DW_ISA_ARM_thumb";
        return DW_DLV_OK;
    case DW_ISA_ARM_arm:
        *s_out = "DW_ISA_ARM_arm";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_MACRO_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_MACRO_define:
        *s_out = "DW_MACRO_define";
        return DW_DLV_OK;
    case DW_MACRO_undef:
        *s_out = "DW_MACRO_undef";
        return DW_DLV_OK;
    case DW_MACRO_start_file:
        *s_out = "DW_MACRO_start_file";
        return DW_DLV_OK;
    case DW_MACRO_end_file:
        *s_out = "DW_MACRO_end_file";
        return DW_DLV_OK;
    case DW_MACRO_define_strp:
        *s_out = "DW_MACRO_define_strp";
        return DW_DLV_OK;
    case DW_MACRO_undef_strp:
        *s_out = "DW_MACRO_undef_strp";
        return DW_DLV_OK;
    case DW_MACRO_import:
        *s_out = "DW_MACRO_import";
        return DW_DLV_OK;
    case DW_MACRO_define_sup:
        *s_out = "DW_MACRO_define_sup";
        return DW_DLV_OK;
    case DW_MACRO_undef_sup:
        *s_out = "DW_MACRO_undef_sup";
        return DW_DLV_OK;
    case DW_MACRO_import_sup:
        *s_out = "DW_MACRO_import_sup";
        return DW_DLV_OK;
    case DW_MACRO_define_strx:
        *s_out = "DW_MACRO_define_strx";
        return DW_DLV_OK;
    case DW_MACRO_undef_strx:
        *s_out = "DW_MACRO_undef_strx";
        return DW_DLV_OK;
    case DW_MACRO_lo_user:
        *s_out = "DW_MACRO_lo_user";
        return DW_DLV_OK;
    case DW_MACRO_hi_user:
        *s_out = "DW_MACRO_hi_user";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_MACINFO_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_MACINFO_define:
        *s_out = "DW_MACINFO_define";
        return DW_DLV_OK;
    case DW_MACINFO_undef:
        *s_out = "DW_MACINFO_undef";
        return DW_DLV_OK;
    case DW_MACINFO_start_file:
        *s_out = "DW_MACINFO_start_file";
        return DW_DLV_OK;
    case DW_MACINFO_end_file:
        *s_out = "DW_MACINFO_end_file";
        return DW_DLV_OK;
    case DW_MACINFO_vendor_ext:
        *s_out = "DW_MACINFO_vendor_ext";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_CFA_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_CFA_nop:
        *s_out = "DW_CFA_nop";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x0. DW_CFA_extended */
    case DW_CFA_set_loc:
        *s_out = "DW_CFA_set_loc";
        return DW_DLV_OK;
    case DW_CFA_advance_loc1:
        *s_out = "DW_CFA_advance_loc1";
        return DW_DLV_OK;
    case DW_CFA_advance_loc2:
        *s_out = "DW_CFA_advance_loc2";
        return DW_DLV_OK;
    case DW_CFA_advance_loc4:
        *s_out = "DW_CFA_advance_loc4";
        return DW_DLV_OK;
    case DW_CFA_offset_extended:
        *s_out = "DW_CFA_offset_extended";
        return DW_DLV_OK;
    case DW_CFA_restore_extended:
        *s_out = "DW_CFA_restore_extended";
        return DW_DLV_OK;
    case DW_CFA_undefined:
        *s_out = "DW_CFA_undefined";
        return DW_DLV_OK;
    case DW_CFA_same_value:
        *s_out = "DW_CFA_same_value";
        return DW_DLV_OK;
    case DW_CFA_register:
        *s_out = "DW_CFA_register";
        return DW_DLV_OK;
    case DW_CFA_remember_state:
        *s_out = "DW_CFA_remember_state";
        return DW_DLV_OK;
    case DW_CFA_restore_state:
        *s_out = "DW_CFA_restore_state";
        return DW_DLV_OK;
    case DW_CFA_def_cfa:
        *s_out = "DW_CFA_def_cfa";
        return DW_DLV_OK;
    case DW_CFA_def_cfa_register:
        *s_out = "DW_CFA_def_cfa_register";
        return DW_DLV_OK;
    case DW_CFA_def_cfa_offset:
        *s_out = "DW_CFA_def_cfa_offset";
        return DW_DLV_OK;
    case DW_CFA_def_cfa_expression:
        *s_out = "DW_CFA_def_cfa_expression";
        return DW_DLV_OK;
    case DW_CFA_expression:
        *s_out = "DW_CFA_expression";
        return DW_DLV_OK;
    case DW_CFA_offset_extended_sf:
        *s_out = "DW_CFA_offset_extended_sf";
        return DW_DLV_OK;
    case DW_CFA_def_cfa_sf:
        *s_out = "DW_CFA_def_cfa_sf";
        return DW_DLV_OK;
    case DW_CFA_def_cfa_offset_sf:
        *s_out = "DW_CFA_def_cfa_offset_sf";
        return DW_DLV_OK;
    case DW_CFA_val_offset:
        *s_out = "DW_CFA_val_offset";
        return DW_DLV_OK;
    case DW_CFA_val_offset_sf:
        *s_out = "DW_CFA_val_offset_sf";
        return DW_DLV_OK;
    case DW_CFA_val_expression:
        *s_out = "DW_CFA_val_expression";
        return DW_DLV_OK;
    case DW_CFA_TI_soffset_extended:
        *s_out = "DW_CFA_TI_soffset_extended";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x1c. DW_CFA_lo_user */
    /*  Skipping alternate spelling of value
        0x1c. DW_CFA_low_user */
    case DW_CFA_MIPS_advance_loc8:
        *s_out = "DW_CFA_MIPS_advance_loc8";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x1d. DW_CFA_TI_def_cfa_soffset */
    case DW_CFA_GNU_window_save:
        *s_out = "DW_CFA_GNU_window_save";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x2d. DW_CFA_AARCH64_negate_ra_state */
    case DW_CFA_GNU_args_size:
        *s_out = "DW_CFA_GNU_args_size";
        return DW_DLV_OK;
    case DW_CFA_GNU_negative_offset_extended:
        *s_out = "DW_CFA_GNU_negative_offset_extended";
        return DW_DLV_OK;
    case DW_CFA_LLVM_def_aspace_cfa:
        *s_out = "DW_CFA_LLVM_def_aspace_cfa";
        return DW_DLV_OK;
    case DW_CFA_LLVM_def_aspace_cfa_sf:
        *s_out = "DW_CFA_LLVM_def_aspace_cfa_sf";
        return DW_DLV_OK;
    case DW_CFA_METAWARE_info:
        *s_out = "DW_CFA_METAWARE_info";
        return DW_DLV_OK;
    case DW_CFA_hi_user:
        *s_out = "DW_CFA_hi_user";
        return DW_DLV_OK;
    /*  Skipping alternate spelling of value
        0x3f. DW_CFA_high_user */
    case DW_CFA_advance_loc:
        *s_out = "DW_CFA_advance_loc";
        return DW_DLV_OK;
    case DW_CFA_offset:
        *s_out = "DW_CFA_offset";
        return DW_DLV_OK;
    case DW_CFA_restore:
        *s_out = "DW_CFA_restore";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_EH_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_EH_PE_absptr:
        *s_out = "DW_EH_PE_absptr";
        return DW_DLV_OK;
    case DW_EH_PE_uleb128:
        *s_out = "DW_EH_PE_uleb128";
        return DW_DLV_OK;
    case DW_EH_PE_udata2:
        *s_out = "DW_EH_PE_udata2";
        return DW_DLV_OK;
    case DW_EH_PE_udata4:
        *s_out = "DW_EH_PE_udata4";
        return DW_DLV_OK;
    case DW_EH_PE_udata8:
        *s_out = "DW_EH_PE_udata8";
        return DW_DLV_OK;
    case DW_EH_PE_sleb128:
        *s_out = "DW_EH_PE_sleb128";
        return DW_DLV_OK;
    case DW_EH_PE_sdata2:
        *s_out = "DW_EH_PE_sdata2";
        return DW_DLV_OK;
    case DW_EH_PE_sdata4:
        *s_out = "DW_EH_PE_sdata4";
        return DW_DLV_OK;
    case DW_EH_PE_sdata8:
        *s_out = "DW_EH_PE_sdata8";
        return DW_DLV_OK;
    case DW_EH_PE_pcrel:
        *s_out = "DW_EH_PE_pcrel";
        return DW_DLV_OK;
    case DW_EH_PE_textrel:
        *s_out = "DW_EH_PE_textrel";
        return DW_DLV_OK;
    case DW_EH_PE_datarel:
        *s_out = "DW_EH_PE_datarel";
        return DW_DLV_OK;
    case DW_EH_PE_funcrel:
        *s_out = "DW_EH_PE_funcrel";
        return DW_DLV_OK;
    case DW_EH_PE_aligned:
        *s_out = "DW_EH_PE_aligned";
        return DW_DLV_OK;
    case DW_EH_PE_omit:
        *s_out = "DW_EH_PE_omit";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_FRAME_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_FRAME_LAST_REG_NUM:
        *s_out = "DW_FRAME_LAST_REG_NUM";
        return DW_DLV_OK;
    case DW_FRAME_REG1:
        *s_out = "DW_FRAME_REG1";
        return DW_DLV_OK;
    case DW_FRAME_REG2:
        *s_out = "DW_FRAME_REG2";
        return DW_DLV_OK;
    case DW_FRAME_REG3:
        *s_out = "DW_FRAME_REG3";
        return DW_DLV_OK;
    case DW_FRAME_REG4:
        *s_out = "DW_FRAME_REG4";
        return DW_DLV_OK;
    case DW_FRAME_REG5:
        *s_out = "DW_FRAME_REG5";
        return DW_DLV_OK;
    case DW_FRAME_REG6:
        *s_out = "DW_FRAME_REG6";
        return DW_DLV_OK;
    case DW_FRAME_REG7:
        *s_out = "DW_FRAME_REG7";
        return DW_DLV_OK;
    case DW_FRAME_REG8:
        *s_out = "DW_FRAME_REG8";
        return DW_DLV_OK;
    case DW_FRAME_REG9:
        *s_out = "DW_FRAME_REG9";
        return DW_DLV_OK;
    case DW_FRAME_REG10:
        *s_out = "DW_FRAME_REG10";
        return DW_DLV_OK;
    case DW_FRAME_REG11:
        *s_out = "DW_FRAME_REG11";
        return DW_DLV_OK;
    case DW_FRAME_REG12:
        *s_out = "DW_FRAME_REG12";
        return DW_DLV_OK;
    case DW_FRAME_REG13:
        *s_out = "DW_FRAME_REG13";
        return DW_DLV_OK;
    case DW_FRAME_REG14:
        *s_out = "DW_FRAME_REG14";
        return DW_DLV_OK;
    case DW_FRAME_REG15:
        *s_out = "DW_FRAME_REG15";
        return DW_DLV_OK;
    case DW_FRAME_REG16:
        *s_out = "DW_FRAME_REG16";
        return DW_DLV_OK;
    case DW_FRAME_REG17:
        *s_out = "DW_FRAME_REG17";
        return DW_DLV_OK;
    case DW_FRAME_REG18:
        *s_out = "DW_FRAME_REG18";
        return DW_DLV_OK;
    case DW_FRAME_REG19:
        *s_out = "DW_FRAME_REG19";
        return DW_DLV_OK;
    case DW_FRAME_REG20:
        *s_out = "DW_FRAME_REG20";
        return DW_DLV_OK;
    case DW_FRAME_REG21:
        *s_out = "DW_FRAME_REG21";
        return DW_DLV_OK;
    case DW_FRAME_REG22:
        *s_out = "DW_FRAME_REG22";
        return DW_DLV_OK;
    case DW_FRAME_REG23:
        *s_out = "DW_FRAME_REG23";
        return DW_DLV_OK;
    case DW_FRAME_REG24:
        *s_out = "DW_FRAME_REG24";
        return DW_DLV_OK;
    case DW_FRAME_REG25:
        *s_out = "DW_FRAME_REG25";
        return DW_DLV_OK;
    case DW_FRAME_REG26:
        *s_out = "DW_FRAME_REG26";
        return DW_DLV_OK;
    case DW_FRAME_REG27:
        *s_out = "DW_FRAME_REG27";
        return DW_DLV_OK;
    case DW_FRAME_REG28:
        *s_out = "DW_FRAME_REG28";
        return DW_DLV_OK;
    case DW_FRAME_REG29:
        *s_out = "DW_FRAME_REG29";
        return DW_DLV_OK;
    case DW_FRAME_REG30:
        *s_out = "DW_FRAME_REG30";
        return DW_DLV_OK;
    case DW_FRAME_REG31:
        *s_out = "DW_FRAME_REG31";
        return DW_DLV_OK;
    case DW_FRAME_FREG0:
        *s_out = "DW_FRAME_FREG0";
        return DW_DLV_OK;
    case DW_FRAME_FREG1:
        *s_out = "DW_FRAME_FREG1";
        return DW_DLV_OK;
    case DW_FRAME_FREG2:
        *s_out = "DW_FRAME_FREG2";
        return DW_DLV_OK;
    case DW_FRAME_FREG3:
        *s_out = "DW_FRAME_FREG3";
        return DW_DLV_OK;
    case DW_FRAME_FREG4:
        *s_out = "DW_FRAME_FREG4";
        return DW_DLV_OK;
    case DW_FRAME_FREG5:
        *s_out = "DW_FRAME_FREG5";
        return DW_DLV_OK;
    case DW_FRAME_FREG6:
        *s_out = "DW_FRAME_FREG6";
        return DW_DLV_OK;
    case DW_FRAME_FREG7:
        *s_out = "DW_FRAME_FREG7";
        return DW_DLV_OK;
    case DW_FRAME_FREG8:
        *s_out = "DW_FRAME_FREG8";
        return DW_DLV_OK;
    case DW_FRAME_FREG9:
        *s_out = "DW_FRAME_FREG9";
        return DW_DLV_OK;
    case DW_FRAME_FREG10:
        *s_out = "DW_FRAME_FREG10";
        return DW_DLV_OK;
    case DW_FRAME_FREG11:
        *s_out = "DW_FRAME_FREG11";
        return DW_DLV_OK;
    case DW_FRAME_FREG12:
        *s_out = "DW_FRAME_FREG12";
        return DW_DLV_OK;
    case DW_FRAME_FREG13:
        *s_out = "DW_FRAME_FREG13";
        return DW_DLV_OK;
    case DW_FRAME_FREG14:
        *s_out = "DW_FRAME_FREG14";
        return DW_DLV_OK;
    case DW_FRAME_FREG15:
        *s_out = "DW_FRAME_FREG15";
        return DW_DLV_OK;
    case DW_FRAME_FREG16:
        *s_out = "DW_FRAME_FREG16";
        return DW_DLV_OK;
    case DW_FRAME_FREG17:
        *s_out = "DW_FRAME_FREG17";
        return DW_DLV_OK;
    case DW_FRAME_FREG18:
        *s_out = "DW_FRAME_FREG18";
        return DW_DLV_OK;
    case DW_FRAME_FREG19:
        *s_out = "DW_FRAME_FREG19";
        return DW_DLV_OK;
    case DW_FRAME_FREG20:
        *s_out = "DW_FRAME_FREG20";
        return DW_DLV_OK;
    case DW_FRAME_FREG21:
        *s_out = "DW_FRAME_FREG21";
        return DW_DLV_OK;
    case DW_FRAME_FREG22:
        *s_out = "DW_FRAME_FREG22";
        return DW_DLV_OK;
    case DW_FRAME_FREG23:
        *s_out = "DW_FRAME_FREG23";
        return DW_DLV_OK;
    case DW_FRAME_FREG24:
        *s_out = "DW_FRAME_FREG24";
        return DW_DLV_OK;
    case DW_FRAME_FREG25:
        *s_out = "DW_FRAME_FREG25";
        return DW_DLV_OK;
    case DW_FRAME_FREG26:
        *s_out = "DW_FRAME_FREG26";
        return DW_DLV_OK;
    case DW_FRAME_FREG27:
        *s_out = "DW_FRAME_FREG27";
        return DW_DLV_OK;
    case DW_FRAME_FREG28:
        *s_out = "DW_FRAME_FREG28";
        return DW_DLV_OK;
    case DW_FRAME_FREG29:
        *s_out = "DW_FRAME_FREG29";
        return DW_DLV_OK;
    case DW_FRAME_FREG30:
        *s_out = "DW_FRAME_FREG30";
        return DW_DLV_OK;
    case DW_FRAME_FREG31:
        *s_out = "DW_FRAME_FREG31";
        return DW_DLV_OK;
    case DW_FRAME_FREG32:
        *s_out = "DW_FRAME_FREG32";
        return DW_DLV_OK;
    case DW_FRAME_FREG33:
        *s_out = "DW_FRAME_FREG33";
        return DW_DLV_OK;
    case DW_FRAME_FREG34:
        *s_out = "DW_FRAME_FREG34";
        return DW_DLV_OK;
    case DW_FRAME_FREG35:
        *s_out = "DW_FRAME_FREG35";
        return DW_DLV_OK;
    case DW_FRAME_FREG36:
        *s_out = "DW_FRAME_FREG36";
        return DW_DLV_OK;
    case DW_FRAME_FREG37:
        *s_out = "DW_FRAME_FREG37";
        return DW_DLV_OK;
    case DW_FRAME_FREG38:
        *s_out = "DW_FRAME_FREG38";
        return DW_DLV_OK;
    case DW_FRAME_FREG39:
        *s_out = "DW_FRAME_FREG39";
        return DW_DLV_OK;
    case DW_FRAME_FREG40:
        *s_out = "DW_FRAME_FREG40";
        return DW_DLV_OK;
    case DW_FRAME_FREG41:
        *s_out = "DW_FRAME_FREG41";
        return DW_DLV_OK;
    case DW_FRAME_FREG42:
        *s_out = "DW_FRAME_FREG42";
        return DW_DLV_OK;
    case DW_FRAME_FREG43:
        *s_out = "DW_FRAME_FREG43";
        return DW_DLV_OK;
    case DW_FRAME_FREG44:
        *s_out = "DW_FRAME_FREG44";
        return DW_DLV_OK;
    case DW_FRAME_FREG45:
        *s_out = "DW_FRAME_FREG45";
        return DW_DLV_OK;
    case DW_FRAME_FREG46:
        *s_out = "DW_FRAME_FREG46";
        return DW_DLV_OK;
    case DW_FRAME_FREG47:
        *s_out = "DW_FRAME_FREG47";
        return DW_DLV_OK;
    case DW_FRAME_FREG48:
        *s_out = "DW_FRAME_FREG48";
        return DW_DLV_OK;
    case DW_FRAME_FREG49:
        *s_out = "DW_FRAME_FREG49";
        return DW_DLV_OK;
    case DW_FRAME_FREG50:
        *s_out = "DW_FRAME_FREG50";
        return DW_DLV_OK;
    case DW_FRAME_FREG51:
        *s_out = "DW_FRAME_FREG51";
        return DW_DLV_OK;
    case DW_FRAME_FREG52:
        *s_out = "DW_FRAME_FREG52";
        return DW_DLV_OK;
    case DW_FRAME_FREG53:
        *s_out = "DW_FRAME_FREG53";
        return DW_DLV_OK;
    case DW_FRAME_FREG54:
        *s_out = "DW_FRAME_FREG54";
        return DW_DLV_OK;
    case DW_FRAME_FREG55:
        *s_out = "DW_FRAME_FREG55";
        return DW_DLV_OK;
    case DW_FRAME_FREG56:
        *s_out = "DW_FRAME_FREG56";
        return DW_DLV_OK;
    case DW_FRAME_FREG57:
        *s_out = "DW_FRAME_FREG57";
        return DW_DLV_OK;
    case DW_FRAME_FREG58:
        *s_out = "DW_FRAME_FREG58";
        return DW_DLV_OK;
    case DW_FRAME_FREG59:
        *s_out = "DW_FRAME_FREG59";
        return DW_DLV_OK;
    case DW_FRAME_FREG60:
        *s_out = "DW_FRAME_FREG60";
        return DW_DLV_OK;
    case DW_FRAME_FREG61:
        *s_out = "DW_FRAME_FREG61";
        return DW_DLV_OK;
    case DW_FRAME_FREG62:
        *s_out = "DW_FRAME_FREG62";
        return DW_DLV_OK;
    case DW_FRAME_FREG63:
        *s_out = "DW_FRAME_FREG63";
        return DW_DLV_OK;
    case DW_FRAME_FREG64:
        *s_out = "DW_FRAME_FREG64";
        return DW_DLV_OK;
    case DW_FRAME_FREG65:
        *s_out = "DW_FRAME_FREG65";
        return DW_DLV_OK;
    case DW_FRAME_FREG66:
        *s_out = "DW_FRAME_FREG66";
        return DW_DLV_OK;
    case DW_FRAME_FREG67:
        *s_out = "DW_FRAME_FREG67";
        return DW_DLV_OK;
    case DW_FRAME_FREG68:
        *s_out = "DW_FRAME_FREG68";
        return DW_DLV_OK;
    case DW_FRAME_FREG69:
        *s_out = "DW_FRAME_FREG69";
        return DW_DLV_OK;
    case DW_FRAME_FREG70:
        *s_out = "DW_FRAME_FREG70";
        return DW_DLV_OK;
    case DW_FRAME_FREG71:
        *s_out = "DW_FRAME_FREG71";
        return DW_DLV_OK;
    case DW_FRAME_FREG72:
        *s_out = "DW_FRAME_FREG72";
        return DW_DLV_OK;
    case DW_FRAME_FREG73:
        *s_out = "DW_FRAME_FREG73";
        return DW_DLV_OK;
    case DW_FRAME_FREG74:
        *s_out = "DW_FRAME_FREG74";
        return DW_DLV_OK;
    case DW_FRAME_FREG75:
        *s_out = "DW_FRAME_FREG75";
        return DW_DLV_OK;
    case DW_FRAME_FREG76:
        *s_out = "DW_FRAME_FREG76";
        return DW_DLV_OK;
    case DW_FRAME_HIGHEST_NORMAL_REGISTER:
        *s_out = "DW_FRAME_HIGHEST_NORMAL_REGISTER";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_CHILDREN_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_CHILDREN_no:
        *s_out = "DW_CHILDREN_no";
        return DW_DLV_OK;
    case DW_CHILDREN_yes:
        *s_out = "DW_CHILDREN_yes";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}
/* ARGSUSED */
int
dwarf_get_ADDR_name (unsigned int val,
    const char ** s_out)
{
    switch (val) {
    case DW_ADDR_none:
        *s_out = "DW_ADDR_none";
        return DW_DLV_OK;
    case DW_ADDR_TI_PTR8:
        *s_out = "DW_ADDR_TI_PTR8";
        return DW_DLV_OK;
    case DW_ADDR_TI_PTR16:
        *s_out = "DW_ADDR_TI_PTR16";
        return DW_DLV_OK;
    case DW_ADDR_TI_PTR22:
        *s_out = "DW_ADDR_TI_PTR22";
        return DW_DLV_OK;
    case DW_ADDR_TI_PTR23:
        *s_out = "DW_ADDR_TI_PTR23";
        return DW_DLV_OK;
    case DW_ADDR_TI_PTR24:
        *s_out = "DW_ADDR_TI_PTR24";
        return DW_DLV_OK;
    case DW_ADDR_TI_PTR32:
        *s_out = "DW_ADDR_TI_PTR32";
        return DW_DLV_OK;
    default: break;
    }
    return DW_DLV_NO_ENTRY;
}

/* END FILE */
