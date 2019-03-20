# Copyright (C) 2018-present MongoDB, Inc.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the Server Side Public License, version 1,
# as published by MongoDB, Inc.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# Server Side Public License for more details.
#
# You should have received a copy of the Server Side Public License
# along with this program. If not, see
# <http://www.mongodb.com/licensing/server-side-public-license>.
#
# As a special exception, the copyright holders give permission to link the
# code of portions of this program with the OpenSSL library under certain
# conditions as described in each individual source file and distribute
# linked combinations including the program with the OpenSSL library. You
# must comply with the Server Side Public License in all respects for
# all of the code used other than as permitted herein. If you modify file(s)
# with this exception, you may extend this exception to your version of the
# file(s), but you are not obligated to do so. If you do not wish to do so,
# delete this exception statement from your version. If you delete this
# exception statement from all source files in the program, then also delete
# it in the license file.
#
"""
IDL Parser.

Converts a YAML document to an idl.syntax tree.
Only validates the document is syntatically correct, not semantically.
"""
from __future__ import absolute_import, print_function, unicode_literals

from abc import ABCMeta, abstractmethod
import io
from typing import Any, Callable, Dict, List, Set, Tuple, Union
import yaml
from yaml import nodes

from . import common
from . import cpp_types
from . import errors
from . import syntax


class _RuleDesc(object):
    """
    Describe a simple parser rule for the generic YAML node parser.

    node_type is either (scalar, bool_scalar, int_scalar, scalar_or_sequence, sequence, or mapping)
    - bool_scalar - means a scalar node which is a valid bool, populates a bool
    - int_scalar - means a scalar node which is a valid non-negative int, populates a int
    - scalar_or_sequence - means a scalar or sequence node, populates a list
    - sequence - a sequence node, populates a list
    - mapping - a mapping node, calls another parser
    - scalar_or_mapping - means a scalar of mapping node, populates a struct
    mapping_parser_func is only called when parsing a mapping or scalar_or_mapping yaml node
    """

    # TODO: after porting to Python 3, use an enum
    REQUIRED = 1
    OPTIONAL = 2

    def __init__(self, node_type, required=OPTIONAL, mapping_parser_func=None):
        # type: (unicode, int, Callable[[errors.ParserContext,yaml.nodes.MappingNode], Any]) -> None
        """Construct a parser rule description."""
        assert required == _RuleDesc.REQUIRED or required == _RuleDesc.OPTIONAL

        self.node_type = node_type  # type: unicode
        self.required = required  # type: int
        self.mapping_parser_func = mapping_parser_func  # type: Callable[[errors.ParserContext,yaml.nodes.MappingNode], Any]


def _generic_parser(
        ctxt,  # type: errors.ParserContext
        node,  # type: Union[yaml.nodes.MappingNode, yaml.nodes.ScalarNode, yaml.nodes.SequenceNode]
        syntax_node_name,  # type: unicode
        syntax_node,  # type: Any
        mapping_rules  # type: Dict[unicode, _RuleDesc]
):  # type: (...) -> None
    # pylint: disable=too-many-branches
    field_name_set = set()  # type: Set[str]

    for [first_node, second_node] in node.value:

        first_name = first_node.value

        if first_name in field_name_set:
            ctxt.add_duplicate_error(first_node, first_name)
            continue

        if first_name in mapping_rules:
            rule_desc = mapping_rules[first_name]

            if rule_desc.node_type == "scalar":
                if ctxt.is_scalar_node(second_node, first_name):
                    syntax_node.__dict__[first_name] = second_node.value
            elif rule_desc.node_type == "bool_scalar":
                if ctxt.is_scalar_bool_node(second_node, first_name):
                    syntax_node.__dict__[first_name] = ctxt.get_bool(second_node)
            elif rule_desc.node_type == "int_scalar":
                if ctxt.is_scalar_non_negative_int_node(second_node, first_name):
                    syntax_node.__dict__[first_name] = ctxt.get_non_negative_int(second_node)
            elif rule_desc.node_type == "scalar_or_sequence":
                if ctxt.is_scalar_sequence_or_scalar_node(second_node, first_name):
                    syntax_node.__dict__[first_name] = ctxt.get_list(second_node)
            elif rule_desc.node_type == "sequence":
                if ctxt.is_scalar_sequence(second_node, first_name):
                    syntax_node.__dict__[first_name] = ctxt.get_list(second_node)
            elif rule_desc.node_type == "scalar_or_mapping":
                if ctxt.is_scalar_or_mapping_node(second_node, first_name):
                    syntax_node.__dict__[first_name] = rule_desc.mapping_parser_func(
                        ctxt, second_node)
            elif rule_desc.node_type == "mapping":
                if ctxt.is_mapping_node(second_node, first_name):
                    syntax_node.__dict__[first_name] = rule_desc.mapping_parser_func(
                        ctxt, second_node)
            else:
                raise errors.IDLError("Unknown node_type '%s' for parser rule" %
                                      (rule_desc.node_type))
        else:
            ctxt.add_unknown_node_error(first_node, syntax_node_name)

        field_name_set.add(first_name)

    # Check for any missing required fields
    for name, rule_desc in mapping_rules.items():
        if not rule_desc.required == _RuleDesc.REQUIRED:
            continue

        # A bool is never "None" like other types, it simply defaults to "false".
        # It means "if bool is None" will always return false and there is no support for required
        # 'bool' at this time.
        if not rule_desc.node_type == 'bool_scalar':
            if syntax_node.__dict__[name] is None:
                ctxt.add_missing_required_field_error(node, syntax_node_name, name)
        else:
            raise errors.IDLError("Unknown node_type '%s' for parser required rule" %
                                  (rule_desc.node_type))


def _parse_mapping(
        ctxt,  # type: errors.ParserContext
        spec,  # type: syntax.IDLSpec
        node,  # type: Union[yaml.nodes.MappingNode, yaml.nodes.ScalarNode, yaml.nodes.SequenceNode]
        syntax_node_name,  # type: unicode
        func  # type: Callable[[errors.ParserContext,syntax.IDLSpec,unicode,Union[yaml.nodes.MappingNode, yaml.nodes.ScalarNode, yaml.nodes.SequenceNode]], None]
):  # type: (...) -> None
    """Parse a top-level mapping section in the IDL file."""
    if not ctxt.is_mapping_node(node, syntax_node_name):
        return

    for [first_node, second_node] in node.value:

        first_name = first_node.value

        func(ctxt, spec, first_name, second_node)


def _parse_initializer(ctxt, node):
    # type: (errors.ParserContext, Union[yaml.nodes.ScalarNode, yaml.nodes.MappingNode]) -> syntax.GlobalInitializer
    """Parse a global initializer."""
    init = syntax.GlobalInitializer(ctxt.file_name, node.start_mark.line, node.start_mark.column)

    if node.id == 'scalar':
        init.name = node.value
        return init

    _generic_parser(ctxt, node, "initializer", init, {
        "register": _RuleDesc('scalar', _RuleDesc.REQUIRED),
        "store": _RuleDesc('scalar'),
    })

    return init


def _parse_config_global(ctxt, node):
    # type: (errors.ParserContext, yaml.nodes.MappingNode) -> syntax.ConfigGlobal
    """Parse global settings for config options."""
    config = syntax.ConfigGlobal(ctxt.file_name, node.start_mark.line, node.start_mark.column)

    _generic_parser(
        ctxt, node, "configs", config, {
            "section": _RuleDesc("scalar"),
            "source": _RuleDesc("scalar_or_sequence"),
            "initializer": _RuleDesc("scalar_or_mapping", mapping_parser_func=_parse_initializer),
        })

    return config


def _parse_global(ctxt, spec, node):
    # type: (errors.ParserContext, syntax.IDLSpec, Union[yaml.nodes.MappingNode, yaml.nodes.ScalarNode, yaml.nodes.SequenceNode]) -> None
    """Parse a global section in the IDL file."""
    if not ctxt.is_mapping_node(node, "global"):
        return

    idlglobal = syntax.Global(ctxt.file_name, node.start_mark.line, node.start_mark.column)

    _generic_parser(
        ctxt, node, "global", idlglobal, {
            "cpp_namespace": _RuleDesc("scalar"), "cpp_includes": _RuleDesc("scalar_or_sequence"),
            "configs": _RuleDesc("mapping", mapping_parser_func=_parse_config_global)
        })

    spec.globals = idlglobal


def _parse_imports(ctxt, spec, node):
    # type: (errors.ParserContext, syntax.IDLSpec, Union[yaml.nodes.MappingNode, yaml.nodes.ScalarNode, yaml.nodes.SequenceNode]) -> None
    """Parse an imports section in the IDL file."""
    if not ctxt.is_scalar_sequence(node, "imports"):
        return

    imports = syntax.Import(ctxt.file_name, node.start_mark.line, node.start_mark.column)
    imports.imports = ctxt.get_list(node)
    spec.imports = imports


def _parse_type(ctxt, spec, name, node):
    # type: (errors.ParserContext, syntax.IDLSpec, unicode, Union[yaml.nodes.MappingNode, yaml.nodes.ScalarNode, yaml.nodes.SequenceNode]) -> None
    """Parse a type section in the IDL file."""
    if not ctxt.is_mapping_node(node, "type"):
        return

    idltype = syntax.Type(ctxt.file_name, node.start_mark.line, node.start_mark.column)
    idltype.name = name

    _generic_parser(
        ctxt, node, "type", idltype, {
            "description": _RuleDesc('scalar', _RuleDesc.REQUIRED),
            "cpp_type": _RuleDesc('scalar', _RuleDesc.REQUIRED),
            "bson_serialization_type": _RuleDesc('scalar_or_sequence', _RuleDesc.REQUIRED),
            "bindata_subtype": _RuleDesc('scalar'),
            "serializer": _RuleDesc('scalar'),
            "deserializer": _RuleDesc('scalar'),
            "default": _RuleDesc('scalar'),
        })

    spec.symbols.add_type(ctxt, idltype)


def _parse_expression(ctxt, node):
    # type: (errors.ParserContext, Union[yaml.nodes.ScalarNode,yaml.nodes.MappingNode]) -> syntax.Expression
    """Parse an expression as either a scalar or a mapping."""
    expr = syntax.Expression(ctxt.file_name, node.start_mark.line, node.start_mark.column)

    if node.id == 'scalar':
        expr.literal = node.value
        return expr

    _generic_parser(ctxt, node, "expr", expr, {
        "expr": _RuleDesc('scalar', _RuleDesc.REQUIRED),
        "is_constexpr": _RuleDesc('bool_scalar'),
    })

    return expr


def _parse_validator(ctxt, node):
    # type: (errors.ParserContext, yaml.nodes.MappingNode) -> syntax.Validator
    """Parse a validator for a field."""
    validator = syntax.Validator(ctxt.file_name, node.start_mark.line, node.start_mark.column)

    _generic_parser(
        ctxt, node, "validator", validator, {
            "gt": _RuleDesc("scalar_or_mapping", mapping_parser_func=_parse_expression),
            "lt": _RuleDesc("scalar_or_mapping", mapping_parser_func=_parse_expression),
            "gte": _RuleDesc("scalar_or_mapping", mapping_parser_func=_parse_expression),
            "lte": _RuleDesc("scalar_or_mapping", mapping_parser_func=_parse_expression),
            "callback": _RuleDesc("scalar"),
        })

    return validator


def _parse_condition(ctxt, node):
    # type: (errors.ParserContext, yaml.nodes.MappingNode) -> syntax.Condition
    """Parse a condition."""
    condition = syntax.Condition(ctxt.file_name, node.start_mark.line, node.start_mark.column)

    _generic_parser(
        ctxt, node, "condition", condition, {
            "preprocessor": _RuleDesc("scalar"),
            "constexpr": _RuleDesc("scalar"),
            "expr": _RuleDesc("scalar"),
        })

    return condition


def _parse_field(ctxt, name, node):
    # type: (errors.ParserContext, str, Union[yaml.nodes.MappingNode, yaml.nodes.ScalarNode, yaml.nodes.SequenceNode]) -> syntax.Field
    """Parse a field in a struct/command in the IDL file."""
    field = syntax.Field(ctxt.file_name, node.start_mark.line, node.start_mark.column)
    field.name = name

    _generic_parser(
        ctxt, node, "field", field, {
            "description": _RuleDesc('scalar'),
            "cpp_name": _RuleDesc('scalar'),
            "type": _RuleDesc('scalar', _RuleDesc.REQUIRED),
            "ignore": _RuleDesc("bool_scalar"),
            "optional": _RuleDesc("bool_scalar"),
            "default": _RuleDesc('scalar'),
            "supports_doc_sequence": _RuleDesc("bool_scalar"),
            "comparison_order": _RuleDesc("int_scalar"),
            "validator": _RuleDesc('mapping', mapping_parser_func=_parse_validator),
        })

    return field


def _parse_fields(ctxt, node):
    # type: (errors.ParserContext, yaml.nodes.MappingNode) -> List[syntax.Field]
    """Parse a fields section in a struct in the IDL file."""

    fields = []

    field_name_set = set()  # type: Set[str]

    for [first_node, second_node] in node.value:

        first_name = first_node.value

        if first_name in field_name_set:
            ctxt.add_duplicate_error(first_node, first_name)
            continue

        # Simple Type
        if second_node.id == "scalar":
            field = syntax.Field(ctxt.file_name, node.start_mark.line, node.start_mark.column)
            field.name = first_name
            field.type = second_node.value
            fields.append(field)
        else:
            field = _parse_field(ctxt, first_name, second_node)
            fields.append(field)

        field_name_set.add(first_name)

    return fields


def _parse_chained_type(ctxt, name, node):
    # type: (errors.ParserContext, str, yaml.nodes.MappingNode) -> syntax.ChainedType
    """Parse a chained type in a struct in the IDL file."""
    chain = syntax.ChainedType(ctxt.file_name, node.start_mark.line, node.start_mark.column)
    chain.name = name

    _generic_parser(ctxt, node, "chain", chain, {
        "cpp_name": _RuleDesc('scalar'),
    })

    return chain


def _parse_chained_types(ctxt, node):
    # type: (errors.ParserContext, yaml.nodes.MappingNode) -> List[syntax.ChainedType]
    """Parse a chained types section in a struct in the IDL file."""
    chained_items = []

    field_name_set = set()  # type: Set[str]

    for [first_node, second_node] in node.value:
        first_name = first_node.value

        if first_name in field_name_set:
            ctxt.add_duplicate_error(first_node, first_name)
            continue

        # Simple Scalar
        if second_node.id == "scalar":
            chain = syntax.ChainedType(ctxt.file_name, node.start_mark.line, node.start_mark.column)
            chain.name = first_name
            chain.cpp_name = second_node.value
            chained_items.append(chain)
        else:
            chain = _parse_chained_type(ctxt, first_name, second_node)
            chained_items.append(chain)

        field_name_set.add(first_name)

    return chained_items


def _parse_chained_struct(ctxt, name, node):
    # type: (errors.ParserContext, str, yaml.nodes.MappingNode) -> syntax.ChainedStruct
    """Parse a chained struct in a struct in the IDL file."""
    chain = syntax.ChainedStruct(ctxt.file_name, node.start_mark.line, node.start_mark.column)
    chain.name = name

    _generic_parser(ctxt, node, "chain", chain, {
        "cpp_name": _RuleDesc('scalar'),
    })

    return chain


def _parse_chained_structs(ctxt, node):
    # type: (errors.ParserContext, yaml.nodes.MappingNode) -> List[syntax.ChainedStruct]
    """Parse a chained structs in a struct in the IDL file."""
    chained_items = []

    field_name_set = set()  # type: Set[str]

    for [first_node, second_node] in node.value:

        first_name = first_node.value

        if first_name in field_name_set:
            ctxt.add_duplicate_error(first_node, first_name)
            continue

        # Simple Scalar
        if second_node.id == "scalar":
            chain = syntax.ChainedStruct(ctxt.file_name, node.start_mark.line,
                                         node.start_mark.column)
            chain.name = first_name
            chain.cpp_name = second_node.value
            chained_items.append(chain)
        else:
            chain = _parse_chained_struct(ctxt, first_name, second_node)
            chained_items.append(chain)

        field_name_set.add(first_name)

    return chained_items


def _parse_struct(ctxt, spec, name, node):
    # type: (errors.ParserContext, syntax.IDLSpec, unicode, Union[yaml.nodes.MappingNode, yaml.nodes.ScalarNode, yaml.nodes.SequenceNode]) -> None
    """Parse a struct section in the IDL file."""
    if not ctxt.is_mapping_node(node, "struct"):
        return

    struct = syntax.Struct(ctxt.file_name, node.start_mark.line, node.start_mark.column)
    struct.name = name

    _generic_parser(
        ctxt, node, "struct", struct, {
            "description": _RuleDesc('scalar', _RuleDesc.REQUIRED),
            "fields": _RuleDesc('mapping', mapping_parser_func=_parse_fields),
            "chained_types": _RuleDesc('mapping', mapping_parser_func=_parse_chained_types),
            "chained_structs": _RuleDesc('mapping', mapping_parser_func=_parse_chained_structs),
            "strict": _RuleDesc("bool_scalar"),
            "inline_chained_structs": _RuleDesc("bool_scalar"),
            "immutable": _RuleDesc('bool_scalar'),
            "generate_comparison_operators": _RuleDesc("bool_scalar"),
        })

    # TODO: SHOULD WE ALLOW STRUCTS ONLY WITH CHAINED STUFF and no fields???
    if struct.fields is None and struct.chained_types is None and struct.chained_structs is None:
        ctxt.add_empty_struct_error(node, struct.name)

    spec.symbols.add_struct(ctxt, struct)


def _parse_enum_values(ctxt, node):
    # type: (errors.ParserContext, yaml.nodes.MappingNode) -> List[syntax.EnumValue]
    """Parse a values section in an enum in the IDL file."""

    enum_values = []

    field_name_set = set()  # type: Set[str]

    for [first_node, second_node] in node.value:

        first_name = first_node.value

        if first_name in field_name_set:
            ctxt.add_duplicate_error(first_node, first_name)
            continue

        # Simple Type
        if ctxt.is_scalar_node(second_node, first_name):
            enum_value = syntax.EnumValue(ctxt.file_name, node.start_mark.line,
                                          node.start_mark.column)
            enum_value.name = first_name
            enum_value.value = second_node.value
            enum_values.append(enum_value)

        field_name_set.add(first_name)

    return enum_values


def _parse_enum(ctxt, spec, name, node):
    # type: (errors.ParserContext, syntax.IDLSpec, unicode, Union[yaml.nodes.MappingNode, yaml.nodes.ScalarNode, yaml.nodes.SequenceNode]) -> None
    """Parse an enum section in the IDL file."""
    if not ctxt.is_mapping_node(node, "struct"):
        return

    idl_enum = syntax.Enum(ctxt.file_name, node.start_mark.line, node.start_mark.column)
    idl_enum.name = name

    _generic_parser(
        ctxt, node, "enum", idl_enum, {
            "description": _RuleDesc('scalar', _RuleDesc.REQUIRED),
            "type": _RuleDesc('scalar', _RuleDesc.REQUIRED),
            "values": _RuleDesc('mapping', mapping_parser_func=_parse_enum_values),
        })

    if idl_enum.values is None:
        ctxt.add_empty_enum_error(node, idl_enum.name)

    spec.symbols.add_enum(ctxt, idl_enum)


def _parse_command(ctxt, spec, name, node):
    # type: (errors.ParserContext, syntax.IDLSpec, unicode, Union[yaml.nodes.MappingNode, yaml.nodes.ScalarNode, yaml.nodes.SequenceNode]) -> None
    """Parse a command section in the IDL file."""
    if not ctxt.is_mapping_node(node, "command"):
        return

    command = syntax.Command(ctxt.file_name, node.start_mark.line, node.start_mark.column)
    command.name = name

    _generic_parser(
        ctxt, node, "command", command, {
            "description": _RuleDesc('scalar', _RuleDesc.REQUIRED),
            "chained_types": _RuleDesc('mapping', mapping_parser_func=_parse_chained_types),
            "chained_structs": _RuleDesc('mapping', mapping_parser_func=_parse_chained_structs),
            "fields": _RuleDesc('mapping', mapping_parser_func=_parse_fields),
            "namespace": _RuleDesc('scalar', _RuleDesc.REQUIRED),
            "cpp_name": _RuleDesc('scalar'),
            "type": _RuleDesc('scalar'),
            "strict": _RuleDesc("bool_scalar"),
            "inline_chained_structs": _RuleDesc("bool_scalar"),
            "immutable": _RuleDesc('bool_scalar'),
            "generate_comparison_operators": _RuleDesc("bool_scalar"),
        })

    valid_commands = [
        common.COMMAND_NAMESPACE_CONCATENATE_WITH_DB, common.COMMAND_NAMESPACE_IGNORED,
        common.COMMAND_NAMESPACE_TYPE, common.COMMAND_NAMESPACE_CONCATENATE_WITH_DB_OR_UUID
    ]

    if command.namespace:
        if command.namespace not in valid_commands:
            ctxt.add_bad_command_namespace_error(command, command.name, command.namespace,
                                                 valid_commands)

        # type property must be specified for a namespace = type
        if command.namespace == common.COMMAND_NAMESPACE_TYPE and not command.type:
            ctxt.add_missing_required_field_error(node, "command", "type")

        if command.namespace != common.COMMAND_NAMESPACE_TYPE and command.type:
            ctxt.add_extranous_command_type(command, command.name)

    # Commands may only have the first parameter, ensure the fields property is an empty array.
    if not command.fields:
        command.fields = []

    spec.symbols.add_command(ctxt, command)


def _parse_server_parameter_class(ctxt, node):
    # type: (errors.ParserContext, Union[yaml.nodes.ScalarNode,yaml.nodes.MappingNode]) -> syntax.ServerParameterClass
    """Parse a server_parameter.cpp_class as either a scalar or a mapping."""
    spc = syntax.ServerParameterClass(ctxt.file_name, node.start_mark.line, node.start_mark.column)

    if node.id == 'scalar':
        spc.name = node.value
        return spc

    _generic_parser(
        ctxt, node, "cpp_class", spc, {
            "name": _RuleDesc('scalar', _RuleDesc.REQUIRED),
            "data": _RuleDesc('scalar'),
            "override_ctor": _RuleDesc('bool_scalar'),
            "override_set": _RuleDesc('bool_scalar'),
        })

    return spc


def _parse_server_parameter(ctxt, spec, name, node):
    # type: (errors.ParserContext, syntax.IDLSpec, unicode, Union[yaml.nodes.MappingNode, yaml.nodes.ScalarNode, yaml.nodes.SequenceNode]) -> None
    """Parse a server_parameters section in the IDL file."""
    if not ctxt.is_mapping_node(node, "server_parameters"):
        return

    param = syntax.ServerParameter(ctxt.file_name, node.start_mark.line, node.start_mark.column)
    param.name = name

    # Declare as local to avoid ugly formatting with long line.
    map_class = _parse_server_parameter_class

    _generic_parser(
        ctxt, node, "server_parameters", param, {
            "set_at": _RuleDesc('scalar_or_sequence', _RuleDesc.REQUIRED),
            "description": _RuleDesc('scalar', _RuleDesc.REQUIRED),
            "cpp_vartype": _RuleDesc('scalar'),
            "cpp_varname": _RuleDesc('scalar'),
            "condition": _RuleDesc('mapping', mapping_parser_func=_parse_condition),
            "redact": _RuleDesc('bool_scalar'),
            "default": _RuleDesc('scalar_or_mapping', mapping_parser_func=_parse_expression),
            "test_only": _RuleDesc('bool_scalar'),
            "deprecated_name": _RuleDesc('scalar_or_sequence'),
            "validator": _RuleDesc('mapping', mapping_parser_func=_parse_validator),
            "on_update": _RuleDesc("scalar"),
            "cpp_class": _RuleDesc('scalar_or_mapping', mapping_parser_func=map_class),
        })

    spec.server_parameters.append(param)


def _parse_config_option(ctxt, spec, name, node):
    # type: (errors.ParserContext, syntax.IDLSpec, unicode, Union[yaml.nodes.MappingNode, yaml.nodes.ScalarNode, yaml.nodes.SequenceNode]) -> None
    """Parse a configs section in the IDL file."""
    if not ctxt.is_mapping_node(node, "configs"):
        return

    option = syntax.ConfigOption(ctxt.file_name, node.start_mark.line, node.start_mark.column)
    option.name = name

    _generic_parser(
        ctxt, node, "configs", option, {
            "short_name": _RuleDesc('scalar'),
            "single_name": _RuleDesc('scalar'),
            "deprecated_name": _RuleDesc('scalar_or_sequence'),
            "deprecated_short_name": _RuleDesc('scalar_or_sequence'),
            "description": _RuleDesc('scalar_or_mapping', _RuleDesc.REQUIRED, _parse_expression),
            "section": _RuleDesc('scalar'),
            "arg_vartype": _RuleDesc('scalar', _RuleDesc.REQUIRED),
            "cpp_vartype": _RuleDesc('scalar'),
            "cpp_varname": _RuleDesc('scalar'),
            "condition": _RuleDesc('mapping', mapping_parser_func=_parse_condition),
            "conflicts": _RuleDesc('scalar_or_sequence'),
            "requires": _RuleDesc('scalar_or_sequence'),
            "hidden": _RuleDesc('bool_scalar'),
            "redact": _RuleDesc('bool_scalar'),
            "default": _RuleDesc('scalar_or_mapping', mapping_parser_func=_parse_expression),
            "implicit": _RuleDesc('scalar_or_mapping', mapping_parser_func=_parse_expression),
            "source": _RuleDesc('scalar_or_sequence'),
            "canonicalize": _RuleDesc('scalar'),
            "duplicate_behavior": _RuleDesc('scalar'),
            "positional": _RuleDesc('scalar'),
            "validator": _RuleDesc('mapping', mapping_parser_func=_parse_validator),
        })

    spec.configs.append(option)


def _prefix_with_namespace(cpp_namespace, cpp_name):
    # type: (unicode, unicode) -> unicode
    """Preface a C++ type name with a namespace if not already qualified or a primitive type."""
    if "::" in cpp_name or cpp_types.is_primitive_scalar_type(cpp_name):
        return cpp_name

    return cpp_namespace + "::" + cpp_name


def _propagate_globals(spec):
    # type: (syntax.IDLSpec) -> None
    """Propagate the globals information to each type and struct as needed."""
    if not spec.globals or not spec.globals.cpp_namespace:
        return

    cpp_namespace = spec.globals.cpp_namespace

    for struct in spec.symbols.structs:
        struct.cpp_namespace = cpp_namespace

    for command in spec.symbols.commands:
        command.cpp_namespace = cpp_namespace

    for idlenum in spec.symbols.enums:
        idlenum.cpp_namespace = cpp_namespace

    for idltype in spec.symbols.types:
        idltype.cpp_type = _prefix_with_namespace(cpp_namespace, idltype.cpp_type)


def _parse(stream, error_file_name):
    # type: (Any, unicode) -> syntax.IDLParsedSpec
    """
    Parse a YAML document into an idl.syntax tree.

    stream: is a io.Stream.
    error_file_name: just a file name for error messages to use.
    """
    # pylint: disable=too-many-branches

    # This will raise an exception if the YAML parse fails
    root_node = yaml.compose(stream)

    ctxt = errors.ParserContext(error_file_name, errors.ParserErrorCollection())

    spec = syntax.IDLSpec()

    # If the document is empty, we are done
    if not root_node:
        return syntax.IDLParsedSpec(spec, None)

    if not root_node.id == "mapping":
        raise errors.IDLError(
            "Expected a YAML mapping node as root node of IDL document, got '%s' instead" %
            root_node.id)

    field_name_set = set()  # type: Set[str]

    for [first_node, second_node] in root_node.value:

        first_name = first_node.value

        if first_name in field_name_set:
            ctxt.add_duplicate_error(first_node, first_name)
            continue

        if first_name == "global":
            _parse_global(ctxt, spec, second_node)
        elif first_name == "imports":
            _parse_imports(ctxt, spec, second_node)
        elif first_name == "enums":
            _parse_mapping(ctxt, spec, second_node, 'enums', _parse_enum)
        elif first_name == "types":
            _parse_mapping(ctxt, spec, second_node, 'types', _parse_type)
        elif first_name == "structs":
            _parse_mapping(ctxt, spec, second_node, 'structs', _parse_struct)
        elif first_name == "commands":
            _parse_mapping(ctxt, spec, second_node, 'commands', _parse_command)
        elif first_name == "server_parameters":
            _parse_mapping(ctxt, spec, second_node, "server_parameters", _parse_server_parameter)
        elif first_name == "configs":
            _parse_mapping(ctxt, spec, second_node, "configs", _parse_config_option)
        else:
            ctxt.add_unknown_root_node_error(first_node)

        field_name_set.add(first_name)

    if ctxt.errors.has_errors():
        return syntax.IDLParsedSpec(None, ctxt.errors)

    _propagate_globals(spec)

    return syntax.IDLParsedSpec(spec, None)


class ImportResolverBase(object):
    """Base class for resolving imported files."""

    __metaclass__ = ABCMeta

    def __init__(self):
        # type: () -> None
        """Construct a ImportResolver."""
        pass

    @abstractmethod
    def resolve(self, base_file, imported_file_name):
        # type: (unicode, unicode) -> unicode
        """Return the complete path to an imported file name."""
        pass

    @abstractmethod
    def open(self, resolved_file_name):
        # type: (unicode) -> Any
        """Return an io.Stream for the requested file."""
        pass


def parse(stream, input_file_name, resolver):
    # type: (Any, unicode, ImportResolverBase) -> syntax.IDLParsedSpec
    """
    Parse a YAML document into an idl.syntax tree.

    stream: is a io.Stream.
    input_file_name: a file name for error messages to use, and to help resolve imported files.
    """
    # pylint: disable=too-many-locals

    root_doc = _parse(stream, input_file_name)

    if root_doc.errors:
        return root_doc

    imports = []  # type: List[Tuple[common.SourceLocation, unicode, unicode]]
    needs_include = []  # type: List[unicode]
    if root_doc.spec.imports:
        imports = [(root_doc.spec.imports, input_file_name, import_file_name)
                   for import_file_name in root_doc.spec.imports.imports]

    resolved_file_names = []  # type: List[unicode]

    ctxt = errors.ParserContext(input_file_name, errors.ParserErrorCollection())

    # Process imports in a breadth-first search
    while imports:
        file_import_tuple = imports[0]
        imports = imports[1:]

        import_location = file_import_tuple[0]
        base_file_name = file_import_tuple[1]
        imported_file_name = file_import_tuple[2]

        # Check for already resolved file
        resolved_file_name = resolver.resolve(base_file_name, imported_file_name)
        if not resolved_file_name:
            ctxt.add_cannot_find_import(import_location, imported_file_name)
            return syntax.IDLParsedSpec(None, ctxt.errors)

        if resolved_file_name in resolved_file_names:
            continue

        resolved_file_names.append(resolved_file_name)

        # Parse imported file
        with resolver.open(resolved_file_name) as file_stream:
            parsed_doc = _parse(file_stream, resolved_file_name)

        # Check for errors
        if parsed_doc.errors:
            return parsed_doc

        # We need to generate includes for imported IDL files which have structs
        if base_file_name == input_file_name and parsed_doc.spec.symbols.structs:
            needs_include.append(imported_file_name)

        # Add other imported files to the list of files to parse
        if parsed_doc.spec.imports:
            imports += [(parsed_doc.spec.imports, resolved_file_name, import_file_name)
                        for import_file_name in parsed_doc.spec.imports.imports]

        # Merge cpp_includes as needed
        if parsed_doc.spec.globals and parsed_doc.spec.globals.cpp_includes:
            root_doc.spec.globals.cpp_includes = list(
                set(root_doc.spec.globals.cpp_includes + parsed_doc.spec.globals.cpp_includes))

        # Merge symbol tables together
        root_doc.spec.symbols.add_imported_symbol_table(ctxt, parsed_doc.spec.symbols)
        if ctxt.errors.has_errors():
            return syntax.IDLParsedSpec(None, ctxt.errors)

    # Resolve the direct imports which contain structs for root document so they can be translated
    # into include file paths in generated code.
    for needs_include_name in needs_include:
        resolved_file_name = resolver.resolve(base_file_name, needs_include_name)
        root_doc.spec.imports.resolved_imports.append(resolved_file_name)

    if root_doc.spec.imports:
        root_doc.spec.imports.dependencies = resolved_file_names

    return root_doc
