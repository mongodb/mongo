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
IDL AST classes.

Represents the derived IDL specification after type resolution in the binding pass has occurred.

This is a lossy translation from the IDL Syntax tree as the IDL AST only contains information about
the enums and structs that need code generated for them, and just enough information to do that.
"""

import enum

from . import common


class IDLBoundSpec(object):
    """A bound IDL document or a set of errors if parsing failed."""

    def __init__(self, spec, error_collection):
        # type: (IDLAST, errors.ParserErrorCollection) -> None
        """Must specify either an IDL document or errors, not both."""
        assert (spec is None and error_collection is not None) or (
            spec is not None and error_collection is None
        )
        self.spec = spec
        self.errors = error_collection


class IDLAST(object):
    """The in-memory representation of an IDL file."""

    def __init__(self):
        # type: () -> None
        """Construct an IDLAST."""
        self.globals = None  # type: Global

        self.commands = []  # type: List[Command]
        self.enums = []  # type: List[Enum]
        self.structs = []  # type: List[Struct]
        self.server_parameters = []  # type: List[ServerParameter]
        self.configs = []  # type: List[ConfigOption]


class Global(common.SourceLocation):
    """
    IDL global object container.

    cpp_namespace and cpp_includes are only populated if the IDL document contains these YAML nodes.
    """

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a Global."""
        self.cpp_namespace = None  # type: str
        self.cpp_includes = []  # type: List[str]
        self.mod_visibility = None  # type: str
        self.configs = None  # type: ConfigGlobal

        super(Global, self).__init__(file_name, line, column)


class Type(common.SourceLocation):
    """
    IDL type information.

    The type of a struct field or command field, for the sake of C++ code generation.
    """

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a Type."""
        self.name = None  # type: str
        self.cpp_type = None  # type: str
        self.bson_serialization_type = None  # type: List[str]
        self.bindata_subtype = None  # type: str
        self.serializer = None  # type: str
        self.deserializer = None  # type: str
        self.is_enum = False  # type: bool
        self.is_array = False  # type: bool
        self.is_variant = False  # type: bool
        self.is_struct = False  # type: bool
        self.variant_types = []  # type: List[Type]
        self.variant_struct_types = None  # type: List[Type]
        self.first_element_field_name = None  # type: str
        self.deserialize_with_tenant = False  # type: bool
        self.internal_only = False  # type: bool
        # Marks whether this type is a query shape component.
        # Can only be true if is_struct is true.
        self.is_query_shape_component = False  # type: bool
        super(Type, self).__init__(file_name, line, column)


class Struct(common.SourceLocation):
    """
    IDL struct information.

    All fields are either required or have a non-None default.

    NOTE: We use this class to generate a struct's C++ class and method definitions. When a field
    has a struct type (or a field is an array of structs or a variant that can be a struct), we
    represent that struct type using ast.Type with is_struct=True.
    """

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a struct."""
        self.name = None  # type: str
        self.cpp_name = None  # type: str
        self.qualified_cpp_name = None  # type: str
        self.mod_visibility = None  # type: str
        self.description = None  # type: str
        self.strict = True  # type: bool
        self.immutable = False  # type: bool
        self.inline_chained_structs = False  # type: bool
        self.generate_comparison_operators = False  # type: bool
        self.fields = []  # type: List[Field]
        self.allow_global_collection_name = False  # type: bool
        self.non_const_getter = False  # type: bool
        self.cpp_validator_func = None  # type: str
        self.is_command_reply = False  # type: bool
        self.is_catalog_ctxt = False  # type: bool
        self.generic_list_type = None  # type: Optional[GenericListType]
        # Determines whether or not this IDL struct can be a component of a query shape. See WRITING-13831.
        self.query_shape_component = False  # type: bool
        self.unsafe_dangerous_disable_extra_field_duplicate_checks = None  # type: bool

        # Indicates if the struct is a view type or not. If a struct is a view type, then the
        # struct may not own all of it's members. If a struct is not a view type, then the struct
        # owns all of it's members.
        self.is_view = True  # type: Optional[bool]

        super(Struct, self).__init__(file_name, line, column)


class GenericListType(enum.Enum):
    ARG = 1
    REPLY = 2


class GenericFieldInfo(common.SourceLocation):
    """Options for a field in a field list."""

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a FieldListEntry."""
        self.name = None  # type: str
        self.forward_to_shards = False  # type: bool
        self.forward_from_shards = False  # type: bool
        self.arg = True  # type: bool
        super(GenericFieldInfo, self).__init__(file_name, line, column)

    def get_should_forward(self):
        """Get the shard-forwarding rule for a generic argument or reply field."""
        if self.arg:
            return self.forward_to_shards
        return self.forward_from_shards


class Expression(common.SourceLocation):
    """Literal of C++ expression representation."""

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct an Expression."""
        self.expr = None  # type: str
        self.validate_constexpr = True  # type: bool
        self.export = False  # type: bool

        super(Expression, self).__init__(file_name, line, column)


class Validator(common.SourceLocation):
    """
    An instance of a validator for a field.

    The validator must include at least one of the defined validation predicates.
    If more than one is included, they must ALL evaluate to true.
    """

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a Validator."""
        # Don't lint gt/lt as bad attribute names.
        self.gt = None  # type: Expression
        self.lt = None  # type: Expression
        self.gte = None  # type: Expression
        self.lte = None  # type: Expression
        self.callback = None  # type: Optional[str]

        super(Validator, self).__init__(file_name, line, column)


@enum.unique
class QueryShapeFieldType(enum.Enum):
    # Abstract literal from shape.
    LITERAL = enum.auto()
    # Leave value as-is in shape.
    PARAMETER = enum.auto()
    # Anonymize string value.
    ANONYMIZE = enum.auto()
    # IDL type uses custom serializer -- defer to that serializer.
    CUSTOM = enum.auto()

    @classmethod
    def bind(cls, string_value):
        # type: (Optional[str]) -> Optional[QueryShapeFieldType]
        if string_value is None:
            return None
        bindings = {
            "literal": cls.LITERAL,
            "parameter": cls.PARAMETER,
            "anonymize": cls.ANONYMIZE,
            "custom": cls.CUSTOM,
        }
        return bindings.get(string_value, None)


class Field(common.SourceLocation):
    """
    An instance of a field in a struct.

    Name is always populated.
    Not all fields are set, it depends on the input document.
    """

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a Field."""
        self.name = None  # type: str
        self.description = None  # type: str
        self.cpp_name = None  # type: str
        self.optional = False  # type: bool
        self.ignore = False  # type: bool
        # True if this field is a chained struct.
        self.chained = False  # type: bool
        self.comparison_order = -1  # type: int
        self.non_const_getter = False  # type: bool
        self.stability = None  # type: Optional[str]
        self.default = None  # type: str
        self.type = None  # type: Type
        self.always_serialize = False  # type: bool

        # Properties specific to fields which are arrays.
        self.supports_doc_sequence = False  # type: bool

        # Properties specific to fields inlined from chained_structs
        self.chained_struct_field = None  # type: Field

        # If this field is a nested chained struct, add the parent field which this field is chained from.
        self.nested_chained_parent = None  # type: Field

        # Internal fields - not generated by parser
        self.serialize_op_msg_request_only = False  # type: bool
        self.constructed = False  # type: bool

        # Validation rules.
        self.validator = None  # type: Optional[Validator]

        # Extra info for generic fields.
        self.generic_field_info = None  # type: Optional[GenericFieldInfo]

        # Determines whether or not this field represents a literal value that should be abstracted when serializing a query shape.
        # See WRITING-13831 for details on query shape.
        self.query_shape = None  # type: Optional[QueryShapeFieldType]

        super(Field, self).__init__(file_name, line, column)

    @property
    def should_serialize_with_options(self):
        # type: () -> bool
        """Returns true if the IDL compiler should add a call to serialization options for this field."""
        return self.query_shape is not None and self.query_shape in [
            QueryShapeFieldType.LITERAL,
            QueryShapeFieldType.ANONYMIZE,
        ]

    @property
    def should_shapify(self):
        return self.query_shape is not None and self.query_shape != QueryShapeFieldType.PARAMETER


class Privilege(common.SourceLocation):
    """IDL privilege information."""

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct an Privilege."""

        self.resource_pattern = None  # type: str
        self.action_type = None  # type: List[str]

        super(Privilege, self).__init__(file_name, line, column)


class AccessCheck(common.SourceLocation):
    """IDL commmand access check information."""

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct an AccessCheck."""

        self.check = None  # type: str
        self.privilege = None  # type: Privilege

        super(AccessCheck, self).__init__(file_name, line, column)


class Command(Struct):
    """
    IDL commmand information.

    All fields are either required or have a non-None default.
    """

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a command."""
        self.namespace = None  # type: str
        self.command_name = None  # type: str
        self.command_alias = None  # type: str
        self.command_field = None  # type: Field
        self.reply_type = None  # type: Field
        self.api_version = ""  # type: str
        self.is_deprecated = False  # type: bool
        self.access_checks = None  # type: List[AccessCheck]
        super(Command, self).__init__(file_name, line, column)


class EnumValue(common.SourceLocation):
    """
    IDL Enum Value information.

    All fields are either required or have a non-None default.
    """

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct an Enum."""
        self.name = None  # type: str
        self.description = None  # type: str
        self.value = None  # type: str
        self.extra_data = None  # type: Dict[str, Any]

        super(EnumValue, self).__init__(file_name, line, column)


class Enum(common.SourceLocation):
    """
    IDL Enum information.

    All fields are either required or have a non-None default.
    """

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct an Enum."""
        self.name = None  # type: str
        self.description = None  # type: str
        self.cpp_namespace = None  # type: str
        self.mod_visibility = None  # type: str
        self.type = None  # type: str
        self.values = []  # type: List[EnumValue]

        super(Enum, self).__init__(file_name, line, column)


class Condition(common.SourceLocation):
    """Condition(s) for a ServerParameter or ConfigOption."""

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a Condition."""
        self.expr = None  # type: str
        self.constexpr = None  # type: str
        self.preprocessor = None  # type: str
        self.feature_flag = None  # type: str
        self.min_fcv = None  # type: str

        super(Condition, self).__init__(file_name, line, column)


class ServerParameterClass(common.SourceLocation):
    """ServerParameter as C++ class specialization."""

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a ServerParameterClass."""

        self.name = None  # type: str
        self.data = None  # type: str
        self.override_ctor = False  # type: bool
        self.override_set = False  # type: bool
        self.override_validate = False  # type: bool
        self.override_warn_if_deprecated = False  # type: bool

        super(ServerParameterClass, self).__init__(file_name, line, column)


@enum.unique
class FeatureFlagRolloutPhase(enum.Enum):
    # Normal feature flag; only configurable at startup.
    NOT_FOR_INCREMENTAL_ROLLOUT = enum.auto()
    # Incremental rollout flag for in-development feature; defaults to disabled.
    IN_DEVELOPMENT = enum.auto()
    # Incremental rollout flag for feature that is ready to roll out; defaults to disabled.
    ROLLOUT = enum.auto()
    # Incremental rollout flag for feature that is completely rolled out; defaults to enabled.
    RELEASED = enum.auto()

    @classmethod
    def bind(cls, string_value):
        # type: (Optional[str]) -> Optional[FeatureFlagRolloutPhase]
        if string_value is None:
            return None
        bindings = {
            "not_for_incremental_rollout": cls.NOT_FOR_INCREMENTAL_ROLLOUT,
            "in_development": cls.IN_DEVELOPMENT,
            "rollout": cls.ROLLOUT,
            "released": cls.RELEASED,
        }
        return bindings.get(string_value, None)

    def __str__(self):
        bindings = {
            FeatureFlagRolloutPhase.NOT_FOR_INCREMENTAL_ROLLOUT: "not_for_incremental_rollout",
            FeatureFlagRolloutPhase.IN_DEVELOPMENT: "in_development",
            FeatureFlagRolloutPhase.ROLLOUT: "rollout",
            FeatureFlagRolloutPhase.RELEASED: "released",
        }
        return bindings.get(self)

    def to_camel_case_string(self):
        bindings = {
            FeatureFlagRolloutPhase.NOT_FOR_INCREMENTAL_ROLLOUT: "notForIncrementalRollout",
            FeatureFlagRolloutPhase.IN_DEVELOPMENT: "inDevelopment",
            FeatureFlagRolloutPhase.ROLLOUT: "rollout",
            FeatureFlagRolloutPhase.RELEASED: "released",
        }
        return bindings.get(self)


class ServerParameter(common.SourceLocation):
    """IDL ServerParameter setting."""

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a ServerParameter."""
        self.name = None  # type: str
        self.set_at = None  # type: str
        self.description = None  # type: str
        self.cpp_class = None  # type: ServerParameterClass
        self.cpp_vartype = None  # type: str
        self.cpp_varname = None  # type: str
        self.condition = None  # type: Condition
        self.redact = False  # type: bool
        self.omit_in_ftdc = False  # type: bool
        self.test_only = False  # type: bool
        self.deprecated_name = []  # type: List[str]
        self.default = None  # type: Expression
        self.feature_flag_phase = None  # type: FeatureFlagRolloutPhase

        # Only valid if cpp_varname is specified.
        self.validator = None  # type: Validator
        self.on_update = None  # type: str

        self.is_deprecated = False  # type: bool

        super(ServerParameter, self).__init__(file_name, line, column)


class GlobalInitializer(common.SourceLocation):
    """Initializer details for custom registration/storage."""

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a GlobalInitializer."""

        self.name = None  # type: str
        self.register = None  # type: str
        self.store = None  # type: str

        super(GlobalInitializer, self).__init__(file_name, line, column)


class ConfigGlobal(common.SourceLocation):
    """IDL ConfigOption Globals."""

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a ConfigGlobal."""

        # Other config globals are consumed in bind phase.
        self.initializer = None  # type: GlobalInitializer

        super(ConfigGlobal, self).__init__(file_name, line, column)


class ConfigOption(common.SourceLocation):
    """IDL ConfigOption setting."""

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a ConfigOption."""
        self.name = None  # type: str
        self.short_name = None  # type: str
        self.deprecated_name = []  # type: List[str]
        self.deprecated_short_name = []  # type: List[str]

        self.description = None  # type: Expression
        self.section = None  # type: str
        self.arg_vartype = None  # type: str
        self.cpp_vartype = None  # type: str
        self.cpp_varname = None  # type: str
        self.condition = None  # type: Condition

        self.conflicts = []  # type: List[str]
        self.requires = []  # type: List[str]
        self.hidden = False  # type: bool
        self.redact = False  # type: bool
        self.default = None  # type: Expression
        self.implicit = None  # type: Expression
        self.source = None  # type: str
        self.canonicalize = None  # type: str

        self.duplicates_append = False  # type: bool
        self.positional_start = None  # type: int
        self.positional_end = None  # type: int
        self.validator = None  # type: Validator

        super(ConfigOption, self).__init__(file_name, line, column)
