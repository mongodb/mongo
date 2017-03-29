# Copyright (C) 2017 MongoDB Inc.
#
# This program is free software: you can redistribute it and/or  modify
# it under the terms of the GNU Affero General Public License, version 3,
# as published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
"""
IDL Parser.

Converts a YAML document to an idl.syntax tree.
Only validates the document is syntatically correct, not semantically.
"""
from __future__ import absolute_import, print_function, unicode_literals

# from typing import Any, Callable, Dict, List, Set, Union
from yaml import nodes
import yaml

from . import errors
from . import syntax


class _RuleDesc(object):
    """
    Describe a simple parser rule for the generic YAML node parser.

    node_type is either (scalar, scalar_bool, scalar_or_sequence, or mapping)
    - scalar_bool - means a scalar node which is a valid bool, populates a bool
    - scalar_or_sequence - means a scalar or sequence node, populates a list
    mapping_parser_func is only called when parsing a mapping yaml node
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
):
    # pylint: disable=too-many-branches
    field_name_set = set()  # type: Set[str]

    for node_pair in node.value:
        first_node = node_pair[0]
        second_node = node_pair[1]

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
            elif rule_desc.node_type == "scalar_or_sequence":
                if ctxt.is_sequence_or_scalar_node(second_node, first_name):
                    syntax_node.__dict__[first_name] = ctxt.get_list(second_node)
            elif rule_desc.node_type == "mapping":
                if ctxt.is_mapping_node(second_node, first_name):
                    syntax_node.__dict__[first_name] = rule_desc.mapping_parser_func(ctxt,
                                                                                     second_node)
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


def _parse_global(ctxt, spec, node):
    # type: (errors.ParserContext, syntax.IDLSpec, Union[yaml.nodes.MappingNode, yaml.nodes.ScalarNode, yaml.nodes.SequenceNode]) -> None
    """Parse a global section in the IDL file."""
    if not ctxt.is_mapping_node(node, "global"):
        return

    idlglobal = syntax.Global(ctxt.file_name, node.start_mark.line, node.start_mark.column)

    _generic_parser(ctxt, node, "global", idlglobal, {
        "cpp_namespace": _RuleDesc("scalar"),
        "cpp_includes": _RuleDesc("scalar_or_sequence"),
    })

    if spec.globals:
        ctxt.add_duplicate_error(node, "global")
        return

    spec.globals = idlglobal


def _parse_type(ctxt, spec, name, node):
    # type: (errors.ParserContext, syntax.IDLSpec, unicode, Union[yaml.nodes.MappingNode, yaml.nodes.ScalarNode, yaml.nodes.SequenceNode]) -> None
    """Parse a type section in the IDL file."""
    if not ctxt.is_mapping_node(node, "type"):
        return

    idltype = syntax.Type(ctxt.file_name, node.start_mark.line, node.start_mark.column)
    idltype.name = name

    _generic_parser(ctxt, node, "type", idltype, {
        "description": _RuleDesc('scalar', _RuleDesc.REQUIRED),
        "cpp_type": _RuleDesc('scalar', _RuleDesc.REQUIRED),
        "bson_serialization_type": _RuleDesc('scalar_or_sequence', _RuleDesc.REQUIRED),
        "bindata_subtype": _RuleDesc('scalar'),
        "serializer": _RuleDesc('scalar'),
        "deserializer": _RuleDesc('scalar'),
        "default": _RuleDesc('scalar'),
    })

    spec.symbols.add_type(ctxt, idltype)


def _parse_types(ctxt, spec, node):
    # type: (errors.ParserContext, syntax.IDLSpec, Union[yaml.nodes.MappingNode, yaml.nodes.ScalarNode, yaml.nodes.SequenceNode]) -> None
    """Parse a types section in the IDL file."""
    if not ctxt.is_mapping_node(node, "types"):
        return

    for node_pair in node.value:
        first_node = node_pair[0]
        second_node = node_pair[1]

        first_name = first_node.value

        _parse_type(ctxt, spec, first_name, second_node)


def _parse_field(ctxt, name, node):
    # type: (errors.ParserContext, str, Union[yaml.nodes.MappingNode, yaml.nodes.ScalarNode, yaml.nodes.SequenceNode]) -> syntax.Field
    """Parse a field in a struct/command in the IDL file."""
    field = syntax.Field(ctxt.file_name, node.start_mark.line, node.start_mark.column)
    field.name = name

    _generic_parser(ctxt, node, "field", field, {
        "description": _RuleDesc('scalar'),
        "type": _RuleDesc('scalar', _RuleDesc.REQUIRED),
        "ignore": _RuleDesc("bool_scalar"),
        "optional": _RuleDesc("bool_scalar"),
        "default": _RuleDesc('scalar'),
    })

    return field


def _parse_fields(ctxt, node):
    # type: (errors.ParserContext, yaml.nodes.MappingNode) -> List[syntax.Field]
    """Parse a fields section in a struct in the IDL file."""

    fields = []

    field_name_set = set()  # type: Set[str]

    for node_pair in node.value:
        first_node = node_pair[0]
        second_node = node_pair[1]

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


def _parse_struct(ctxt, spec, name, node):
    # type: (errors.ParserContext, syntax.IDLSpec, unicode, Union[yaml.nodes.MappingNode, yaml.nodes.ScalarNode, yaml.nodes.SequenceNode]) -> None
    """Parse a struct section in the IDL file."""
    if not ctxt.is_mapping_node(node, "struct"):
        return

    struct = syntax.Struct(ctxt.file_name, node.start_mark.line, node.start_mark.column)
    struct.name = name

    _generic_parser(ctxt, node, "struct", struct, {
        "description": _RuleDesc('scalar', _RuleDesc.REQUIRED),
        "fields": _RuleDesc('mapping', mapping_parser_func=_parse_fields),
        "strict": _RuleDesc("bool_scalar"),
    })

    if struct.fields is None:
        ctxt.add_empty_struct_error(node, struct.name)

    spec.symbols.add_struct(ctxt, struct)


def _parse_structs(ctxt, spec, node):
    # type: (errors.ParserContext, syntax.IDLSpec, Union[yaml.nodes.MappingNode, yaml.nodes.ScalarNode, yaml.nodes.SequenceNode]) -> None
    """Parse a structs section in the IDL file."""
    if not ctxt.is_mapping_node(node, "structs"):
        return

    for node_pair in node.value:
        first_node = node_pair[0]
        second_node = node_pair[1]

        first_name = first_node.value

        _parse_struct(ctxt, spec, first_name, second_node)


def parse(stream, error_file_name="unknown"):
    # type: (Any, unicode) -> syntax.IDLParsedSpec
    """
    Parse a YAML document into an idl.syntax tree.

    stream: is a io.Stream.
    error_file_name: just a file name for error messages to use.
    """

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

    for node_pair in root_node.value:
        first_node = node_pair[0]
        second_node = node_pair[1]

        first_name = first_node.value

        if first_name in field_name_set:
            ctxt.add_duplicate_error(first_node, first_name)
            continue

        if first_name == "global":
            _parse_global(ctxt, spec, second_node)
        elif first_name == "types":
            _parse_types(ctxt, spec, second_node)
        elif first_name == "structs":
            _parse_structs(ctxt, spec, second_node)
        else:
            ctxt.add_unknown_root_node_error(first_node)

        field_name_set.add(first_name)

    if ctxt.errors.has_errors():
        return syntax.IDLParsedSpec(None, ctxt.errors)
    else:
        return syntax.IDLParsedSpec(spec, None)
