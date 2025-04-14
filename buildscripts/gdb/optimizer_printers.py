"""GDB Pretty-printers for types in mongo::abt."""

import os
import sys
from pathlib import Path

import gdb
import gdb.printing

if not gdb:
    sys.path.insert(0, str(Path(os.path.abspath(__file__)).parent.parent.parent))
    from buildscripts.gdb.mongo import lookup_type

ABT_NS = "mongo::abt"

# Tracks the indentation for Op* tree types.
operator_indent_level = 0


def strip_namespace(value):
    return str(value).split("::")[-1]


class StrongStringAliasPrinter(object):
    """Pretty-printer for mongo::abt::StrongStringAlias."""

    def __init__(self, val):
        """Initialize StrongStringAliasPrinter."""
        self.val = val

    @staticmethod
    def display_hint():
        """Display hint."""
        return None

    def to_string(self):
        return self.val["_value"]


class FixedArityNodePrinter(object):
    """Pretty-printer for OpFixedArity."""

    def __init__(self, val, arity, name):
        """Initialize FixedArityNodePrinter."""
        self.val = val
        self.arity = arity
        self.name = name
        self.custom_children = []

    @staticmethod
    def display_hint():
        """Display hint."""
        return None

    def children(self):
        """children."""
        global operator_indent_level

        prior_indent = operator_indent_level
        current_indent = operator_indent_level + self.arity + len(self.custom_children) - 1
        for child in self.custom_children:
            lhs = "\n"
            for _ in range(current_indent):
                lhs += "|   "

            operator_indent_level = current_indent
            yield lhs, child
            current_indent -= 1

        for i in range(self.arity):
            lhs = "\n"
            for _ in range(current_indent):
                lhs += "|   "

            # A little weird, but most ABTs prefer to print the last child first.
            operator_indent_level = current_indent
            yield lhs, self.val["_nodes"][self.arity - i - 1]
            current_indent -= 1
        operator_indent_level = prior_indent

    # Adds a custom child node which is not directly contained in the "_nodes" member variable.
    def add_child(self, child):
        self.custom_children.append(child)

    def to_string(self):
        # Default for nodes which just print their type.
        return self.name


class Vector(object):
    def __init__(self, vec):
        self.vec = vec
        self.start = vec["_M_impl"]["_M_start"]
        self.finish = vec["_M_impl"]["_M_finish"]

    def __iter__(self):
        item = self.start
        while item != self.finish:
            elt = item.dereference()
            item = item + 1
            yield elt

    def count(self):
        item_type = self.vec.type.template_argument(0)
        return int((int(self.finish) - int(self.start)) / item_type.sizeof)

    def get(self, index):
        if index > self.count() - 1:
            raise gdb.GdbError(
                "Invalid Vector access at index {} with size {}".format(index, self.count())
            )
        item = self.start + index
        return item.dereference()


class DynamicArityNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for OpDynamicArity."""

    def __init__(self, val, minArity, name):
        """Initialize DynamicArityNodePrinter."""
        super().__init__(val, minArity, name)
        self.dynamic_nodes = Vector(self.val["_dyNodes"])
        self.dynamic_count = self.dynamic_nodes.count()

    def children(self):
        """children."""
        global operator_indent_level

        prior_indent = operator_indent_level
        operator_indent_level += self.dynamic_count
        for res in super().children():
            yield res

        current_indent = operator_indent_level - 1
        for child in self.dynamic_nodes:
            lhs = "\n"
            for _ in range(current_indent):
                lhs += "|   "

            operator_indent_level = current_indent
            yield lhs, child
            current_indent -= 1
        operator_indent_level = prior_indent


class ExpressionBinderPrinter(DynamicArityNodePrinter):
    """Pretty-printer for ExpressionBinder."""

    def __init__(self, val):
        """Initialize ExpressionBinderPrinter."""
        super().__init__(val, 0, "Binder")

    def to_string(self):
        res = "Binder[{"
        bindings = Vector(self.val["_names"])
        for name in bindings:
            res += str(name) + " "
        res += "}]"
        return res


class FunctionCallPrinter(DynamicArityNodePrinter):
    """Pretty-printer for FunctionCall."""

    def __init__(self, val):
        """Initialize FunctionCallPrinter."""
        super().__init__(val, 0, "FunctionCall")

    def to_string(self):
        return "FunctionCall[{}]".format(self.val["_name"])


class ConstantPrinter(object):
    """Pretty-printer for Constant."""

    def __init__(self, val):
        """Initialize ConstantPrinter."""
        self.val = val

    @staticmethod
    def display_hint():
        """Display hint."""
        return None

    @staticmethod
    def print_sbe_value(tag, value):
        value_print_fn = "mongo::sbe::value::print"
        (print_fn_symbol, _) = gdb.lookup_symbol(value_print_fn)
        if print_fn_symbol is None:
            raise gdb.GdbError("Could not find pretty print function: " + value_print_fn)
        print_fn = print_fn_symbol.value()
        return print_fn(tag, value)

    def to_string(self):
        return "Constant[{}]".format(
            ConstantPrinter.print_sbe_value(self.val["_tag"], self.val["_val"])
        )


class VariablePrinter(object):
    """Pretty-printer for Variable."""

    def __init__(self, val):
        """Initialize VariablePrinter."""
        self.val = val

    @staticmethod
    def display_hint():
        """Display hint."""
        return None

    def to_string(self):
        return "Variable[{}]".format(self.val["_name"])


class UnaryOpPrinter(FixedArityNodePrinter):
    """Pretty-printer for UnaryOp."""

    def __init__(self, val):
        """Initialize UnaryOpPrinter."""
        super().__init__(val, 1, "UnaryOp")

    def to_string(self):
        return "UnaryOp[{}]".format(strip_namespace(self.val["_op"]))


class BinaryOpPrinter(FixedArityNodePrinter):
    """Pretty-printer for BinaryOp."""

    def __init__(self, val):
        """Initialize BinaryOpPrinter."""
        super().__init__(val, 2, "BinaryOp")

    def to_string(self):
        return "BinaryOp[{}]".format(strip_namespace(self.val["_op"]))


class NaryOpPrinter(DynamicArityNodePrinter):
    """Pretty-printer for NaryOp."""

    def __init__(self, val):
        """Initialize NaryOpPrinter."""
        super().__init__(val, 0, "NaryOp")

    def to_string(self):
        return "NaryOp[{}]".format(strip_namespace(self.val["_op"]))


class BlackholePrinter(FixedArityNodePrinter):
    """Pretty-printer for Blackhole."""

    def __init__(self, val):
        """Initialize BlackholePrinter."""
        super().__init__(val, 0, "Blackhole")


class IfPrinter(FixedArityNodePrinter):
    """Pretty-printer for If."""

    def __init__(self, val):
        """Initialize IfPrinter."""
        super().__init__(val, 3, "If")


class LetPrinter(FixedArityNodePrinter):
    """Pretty-printer for Let."""

    def __init__(self, val):
        """Initialize LetPrinter."""
        super().__init__(val, 2, "Let")


class MultiLetPrinter(DynamicArityNodePrinter):
    """Pretty-printer for MultiLet."""

    def __init__(self, val):
        """Initialize MultiLetPrinter."""
        super().__init__(val, 0, "MultiLet")


class LambdaAbstractionPrinter(FixedArityNodePrinter):
    """Pretty-printer for LambdaAbstraction."""

    def __init__(self, val):
        """Initialize LambdaAbstractionPrinter."""
        super().__init__(val, 1, "LambdaAbstraction")

    def to_string(self):
        return "LambdaAbstraction[{}]".format(self.val["_varName"])


class LambdaApplicationPrinter(FixedArityNodePrinter):
    """Pretty-printer for LambdaApplication."""

    def __init__(self, val):
        """Initialize LambdaApplicationPrinter."""
        super().__init__(val, 2, "LambdaApplication")


class SourcePrinter(FixedArityNodePrinter):
    """Pretty-printer for Source."""

    def __init__(self, val):
        """Initialize SourcePrinter."""
        super().__init__(val, 0, "Source")


class SwitchPrinter(DynamicArityNodePrinter):
    """Pretty-printer for Switch."""

    def __init__(self, val):
        """Initialize SwitchPrinter."""
        super().__init__(val, 0, "Switch")


class ReferencesPrinter(DynamicArityNodePrinter):
    """Pretty-printer for References."""

    def __init__(self, val):
        """Initialize ReferencesPrinter."""
        super().__init__(val, 0, "References")


class PolyValuePrinter(object):
    """Pretty-printer for PolyValue."""

    def __init__(self, val):
        """Initialize PolyValuePrinter."""
        self.val = val
        self.control_block = self.val["_object"]
        self.tag = self.control_block.dereference()["_tag"]

        if self.val.type.code == gdb.TYPE_CODE_REF:
            self.poly_type = self.val.type.target().strip_typedefs()
        else:
            self.poly_type = self.val.type.strip_typedefs()
        self.type_set = str(self.poly_type).split("<", 1)[1]

        if self.tag < 0:
            raise gdb.GdbError("Invalid PolyValue tag: {}, must be at least 0".format(self.tag))

        # Check if the tag is out of range for the set of types that we know about.
        if self.tag > len(self.type_set.split(",")):
            raise gdb.GdbError(
                "Unknown PolyValue tag: {} (max: {}), did you add a new one?".format(
                    self.tag, str(self.type_set)
                )
            )

    @staticmethod
    def display_hint():
        """Display hint."""
        return None

    def cast_control_block(self, target_type):
        return (
            self.control_block.dereference().address.cast(target_type.pointer()).dereference()["_t"]
        )

    def get_dynamic_type(self):
        # Build up the dynamic type for the particular variant of this PolyValue instance. This is
        # basically a reinterpret cast of the '_object' member variable to the correct instance
        # of ControlBlockVTable<T, Ts...>::ConcreteType where T is the template argument derived
        # from the _tag member variable.
        poly_type = self.val.type.template_argument(self.tag)
        dynamic_type = f"mongo::algebra::ControlBlockVTable<{poly_type.name}, "
        dynamic_type += self.type_set
        dynamic_type += "::ConcreteType"
        return dynamic_type

    def to_string(self):
        dynamic_type = self.get_dynamic_type()
        try:
            dynamic_type = lookup_type(dynamic_type).strip_typedefs()
        except gdb.error:
            return "Unknown PolyValue tag: {}, did you add a new one?".format(self.tag)
        # GDB automatically formats types with children, remove the extra characters to get the
        # output that we want.
        return (
            str(self.cast_control_block(dynamic_type))
            .replace(" = ", "")
            .replace("{", "")
            .replace("}", "")
        )


def register_optimizer_printers(pp):
    """Registers a number of pretty printers related to the CQF optimizer."""

    # Utility types within the optimizer.
    pp.add("StrongStringAlias", f"{ABT_NS}::StrongStringAlias", True, StrongStringAliasPrinter)

    # Add the sub-printers for each of the possible ABT types.
    # This is the set of known ABT variants that GDB is aware of. When adding to this list, ensure
    # that a corresponding printer class exists with the name MyNewNodePrinter where "MyNewNode"
    # exactly matches the entry in this list. The printer class may choose to derive from one of
    # the arity printers to automatically print the children nodes. By default, just the name of
    # your node will be printed. If you want to display more information (e.g. scan def name),
    # then override the to_string() method.
    abt_type_set = [
        "Blackhole",
        "Constant",
        "Variable",
        "UnaryOp",
        "BinaryOp",
        "NaryOp",
        "If",
        "Let",
        "MultiLet",
        "LambdaAbstraction",
        "LambdaApplication",
        "FunctionCall",
        "Source",
        "Switch",
        "References",
        "ExpressionBinder",
    ]
    for abt_type in abt_type_set:
        pp.add(
            abt_type,
            f"{ABT_NS}::{abt_type}",
            False,
            getattr(sys.modules[__name__], abt_type + "Printer"),
        )

    # Add the generic PolyValue printer which determines the exact type at runtime and attempts to
    # invoke the printer for that type.
    pp.add("PolyValue", "mongo::algebra::PolyValue", True, PolyValuePrinter)
