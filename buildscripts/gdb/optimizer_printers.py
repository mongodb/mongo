"""GDB Pretty-printers for types in mongo::optimizer."""

import os
import sys
from pathlib import Path

import gdb
import gdb.printing

if not gdb:
    sys.path.insert(0, str(Path(os.path.abspath(__file__)).parent.parent.parent))
    from buildscripts.gdb.mongo import get_boost_optional, lookup_type

OPTIMIZER_NS = "mongo::optimizer"

# Tracks the indentation for Op* tree types.
operator_indent_level = 0


def strip_namespace(value):
    return str(value).split("::")[-1]


def eval_print_fn(val, print_fn):
    """Evaluate a print function, and return the resulting string."""

    # The generated output from explain contains the string "\n" (two characters)
    # replace them with a single EOL character so that GDB prints multi-line
    # explains nicely.
    pp_result = print_fn(val)
    pp_str = str(pp_result).replace('"', "").replace("\\n", "\n")
    return pp_str


class OptimizerTypePrinter(object):
    """Base class that pretty prints via a single argument C++ function."""

    def __init__(self, val, print_fn_name):
        """Initialize base printer."""
        self.val = val
        print_fn_symbol = gdb.lookup_symbol(print_fn_name)[0]
        if print_fn_symbol is None:
            # Couldn't find the function, this could be due to a variety of reasons but mostly
            # likely it is not included and/or optimized out of the executable being debugged
            # (e.g. there are some explain helpers which are only called from unit tests so won't
            # be included in a mongod binary).
            print("Warning: Could not find pretty print function: " + print_fn_name)
            self.print_fn = None
        else:
            self.print_fn = print_fn_symbol.value()

    @staticmethod
    def display_hint():
        """Display hint."""
        return None

    def to_string(self):
        """Return string for printing."""
        if self.print_fn is None:
            return f"<{self.val.type}>"
        return eval_print_fn(self.val, self.print_fn)


class StrongStringAliasPrinter(object):
    """Pretty-printer for mongo::optimizer::StrongStringAlias."""

    def __init__(self, val):
        """Initialize StrongStringAliasPrinter."""
        self.val = val

    @staticmethod
    def display_hint():
        """Display hint."""
        return None

    def to_string(self):
        return self.val["_value"]


class MemoPrinter(OptimizerTypePrinter):
    """Pretty-printer for mongo::optimizer::cascades::Memo."""

    def __init__(self, val):
        """Initialize MemoPrinter."""
        super().__init__(val, "ExplainGenerator::explainMemo")


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


class UnionNodePrinter(DynamicArityNodePrinter):
    """Pretty-printer for UnionNode."""

    def __init__(self, val):
        """Initialize UnionNodePrinter."""
        super().__init__(val, 2, "Union")


class ScanNodePrinter(object):
    """Pretty-printer for ScanNode."""

    def __init__(self, val):
        """Initialize ScanNodePrinter."""
        self.val = val
        # Use the FixedArityNodePrinter to handle access to the ExpressionBinder child.
        _, self.binder = next(FixedArityNodePrinter(self.val, 1, "Scan").children())

    def get_bound_projection(self):
        pp = PolyValuePrinter(self.binder)
        dynamic_type = lookup_type(pp.get_dynamic_type()).strip_typedefs()
        binder = pp.cast_control_block(dynamic_type)
        bound_projections = Vector(binder["_names"])
        if bound_projections.count() != 1:
            return "<unknown>"
        return str(bound_projections.get(0))

    def to_string(self):
        return "Scan[{}, {}]".format(self.val["_scanDefName"], self.get_bound_projection())


class FilterNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for FilterNode."""

    def __init__(self, val):
        """Initialize FilterNodePrinter."""
        super().__init__(val, 2, "Filter")


class EvaluationNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for EvaluationNode."""

    def __init__(self, val):
        """Initialize EvaluationNodePrinter."""
        super().__init__(val, 2, "Evaluation")


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


class RootNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for RootNode."""

    def __init__(self, val):
        """Initialize RootNodePrinter."""
        # The second child (References) of the RootNode will be printed inline, but allow the base
        # class to print the node child.
        super().__init__(val, 1, "RootNode")

    def to_string(self):
        projections = Vector(self.val["_projections"]["_vector"])
        return "\nRoot[{}]".format(projections.get(0))


class GroupByNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for GroupByNode."""

    def __init__(self, val):
        """Initialize GroupByNodePrinter."""
        super().__init__(val, 5, "GroupBy")


class PathComparePrinter(FixedArityNodePrinter):
    """Pretty-printer for PathCompare."""

    def __init__(self, val):
        """Initialize PathComparePrinter."""
        super().__init__(val, 1, "PathCompare")

    def to_string(self):
        return "PathCompare[{}]".format(strip_namespace(self.val["_cmp"]))


class PathTraversePrinter(FixedArityNodePrinter):
    """Pretty-printer for PathTraverse."""

    def __init__(self, val):
        """Initialize PathTraversePrinter."""
        super().__init__(val, 1, "PathTraverse")

    def to_string(self):
        depth = self.val["_maxDepth"]
        return "PathTraverse[{}]".format(str(depth) if depth != 0 else "inf")


class PathGetPrinter(FixedArityNodePrinter):
    """Pretty-printer for PathGet."""

    def __init__(self, val):
        """Initialize PathGetPrinter."""
        super().__init__(val, 1, "PathGet")

    def to_string(self):
        return "PathGet[{}]".format(self.val["_name"])


class EvalFilterPrinter(FixedArityNodePrinter):
    """Pretty-printer for EvalFilter."""

    def __init__(self, val):
        """Initialize EvalFilterPrinter."""
        super().__init__(val, 2, "EvalFilter")


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


class EvalPathPrinter(FixedArityNodePrinter):
    """Pretty-printer for EvalPath."""

    def __init__(self, val):
        """Initialize EvalPathPrinter."""
        super().__init__(val, 2, "EvalPath")


class PathComposeMPrinter(FixedArityNodePrinter):
    """Pretty-printer for PathComposeM."""

    def __init__(self, val):
        """Initialize PathComposeMPrinter."""
        super().__init__(val, 2, "PathComposeM")


class PathComposeAPrinter(FixedArityNodePrinter):
    """Pretty-printer for PathComposeA."""

    def __init__(self, val):
        """Initialize PathComposeAPrinter."""
        super().__init__(val, 2, "PathComposeA")


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


class PathFieldPrinter(FixedArityNodePrinter):
    """Pretty-printer for PathField."""

    def __init__(self, val):
        """Initialize PathFieldPrinter."""
        super().__init__(val, 1, "PathField")

    def to_string(self):
        return "PathField[{}]".format(self.val["_name"])


class PathConstantPrinter(FixedArityNodePrinter):
    """Pretty-printer for PathConstant."""

    def __init__(self, val):
        """Initialize PathConstantPrinter."""
        super().__init__(val, 1, "PathConstant")


class PathLambdaPrinter(FixedArityNodePrinter):
    """Pretty-printer for PathLambda."""

    def __init__(self, val):
        """Initialize PathLambdaPrinter."""
        super().__init__(val, 1, "PathLambda")


class PathIdentityPrinter(FixedArityNodePrinter):
    """Pretty-printer for PathIdentity."""

    def __init__(self, val):
        """Initialize PathIdentityPrinter."""
        super().__init__(val, 0, "PathIdentity")


class PathDropPrinter(FixedArityNodePrinter):
    """Pretty-printer for PathDrop."""

    def __init__(self, val):
        """Initialize PathDropPrinter."""
        super().__init__(val, 0, "PathDrop")

    def to_string(self):
        return "PathDrop[{}]".format(str(self.val["_names"]))


class PathKeepPrinter(FixedArityNodePrinter):
    """Pretty-printer for PathKeep."""

    def __init__(self, val):
        """Initialize PathKeepPrinter."""
        super().__init__(val, 0, "PathKeep")

    def to_string(self):
        return "PathKeep[{}]".format(str(self.val["_names"]))


class PathObjPrinter(FixedArityNodePrinter):
    """Pretty-printer for PathObj."""

    def __init__(self, val):
        """Initialize PathObjPrinter."""
        super().__init__(val, 0, "PathObj")


class PathArrPrinter(FixedArityNodePrinter):
    """Pretty-printer for PathArr."""

    def __init__(self, val):
        """Initialize PathArrPrinter."""
        super().__init__(val, 0, "PathArr")


class PathDefaultPrinter(FixedArityNodePrinter):
    """Pretty-printer for PathDefault."""

    def __init__(self, val):
        """Initialize PathDefaultPrinter."""
        super().__init__(val, 1, "PathDefault")


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


class FieldProjectionMapPrinter(object):
    """Pretty-printer for FieldProjectionMap."""

    def __init__(self, val):
        """Initialize FieldProjectionMapPrinter."""
        self.val = val

    @staticmethod
    def display_hint():
        """Display hint."""
        return None

    def to_string(self):
        rid_proj = self.val["_ridProjection"]
        root_proj = self.val["_rootProjection"]
        res = "{"
        if get_boost_optional(rid_proj) is not None:
            res += "<rid>: " + str(rid_proj) + ", "
        if get_boost_optional(root_proj) is not None:
            res += "<root>: " + str(root_proj) + ", "

        # Python reformats the string with embedded "=" characters, avoid that by replacing here.
        res += (
            str(self.val["_fieldProjections"]).replace("=", ":").replace("{", "(").replace("}", ")")
        )
        res += "}"
        return res


class PhysicalScanNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for PhysicalScanNode."""

    def __init__(self, val):
        """Initialize PhysicalScanNodePrinter."""
        super().__init__(val, 1, "PhysicalScan")

    def to_string(self):
        return "PhysicalScan[{}, {}]".format(
            str(self.val["_fieldProjectionMap"]), str(self.val["_scanDefName"])
        )


class ValueScanNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for ValueScanNode."""

    def __init__(self, val):
        """Initialize ValueScanNodePrinter."""
        super().__init__(val, 1, "ValueScan")

    def to_string(self):
        return "ValueScan[hasRID={},arraySize={}]".format(
            self.val["_hasRID"], self.val["_arraySize"]
        )


class CoScanNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for CoScanNode."""

    def __init__(self, val):
        """Initialize CoScanNodePrinter."""
        super().__init__(val, 0, "CoScan")


class IndexScanNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for IndexScanNode."""

    def __init__(self, val):
        """Initialize IndexScanNodePrinter."""
        super().__init__(val, 1, "IndexScan")

    def to_string(self):
        return "IndexScan[{{{}}}, scanDef={}, indexDef={}]".format(
            self.val["_fieldProjectionMap"],
            self.val["_scanDefName"],
            self.val["_indexDefName"],
        ).replace("\n", "")


class SeekNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for SeekNode."""

    def __init__(self, val):
        """Initialize SeekNodePrinter."""
        super().__init__(val, 2, "Seek")

    def to_string(self):
        return "Seek[rid_projection: {}, {}, scanDef: {}]".format(
            self.val["_ridProjectionName"],
            self.val["_fieldProjectionMap"],
            self.val["_scanDefName"],
        )


class RIDIntersectNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for RIDIntersectNode."""

    def __init__(self, val):
        """Initialize RIDIntersectNodePrinter."""
        super().__init__(val, 2, "RIDIntersect")

    def to_string(self):
        return "RIDIntersect[" + str(self.val["_scanProjectionName"]) + "]"


class RIDUnionNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for RIDUnionNode."""

    def __init__(self, val):
        """Initialize RIDUnionNodePrinter."""
        super().__init__(val, 4, "RIDUnion")

    def to_string(self):
        return "RIDUnion[" + str(self.val["_scanProjectionName"]) + "]"


def print_correlated_projections(projections):
    # Strip off the extra absl map prefix.
    return str(projections).split("elems ")[-1]


class BinaryJoinNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for BinaryJoinNode."""

    def __init__(self, val):
        """Initialize BinaryJoinNodePrinter."""
        super().__init__(val, 3, "BinaryJoin")

    def to_string(self):
        correlated = print_correlated_projections(self.val["_correlatedProjectionNames"])
        return (
            "BinaryJoin[type="
            + str(strip_namespace(self.val["_joinType"]))
            + ", "
            + correlated
            + "]"
        )


def print_eq_join_condition(leftKeys, rightKeys):
    condition = "Condition["
    for i in range(leftKeys.count()):
        condition += str(leftKeys.get(i)) + "==" + str(rightKeys.get(i)) + ","
    condition += "]"
    return condition


class HashJoinNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for HashJoinNode."""

    def __init__(self, val):
        """Initialize HashJoinNodePrinter."""
        super().__init__(val, 3, "HashJoin")

        # Manually add the child which prints the sets of keys.
        leftKeys = Vector(self.val["_leftKeys"])
        rightKeys = Vector(self.val["_rightKeys"])
        self.add_child(print_eq_join_condition(leftKeys, rightKeys))

    def to_string(self):
        return "HashJoin[type=" + strip_namespace(self.val["_joinType"]) + "]"


class MergeJoinNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for MergeJoinNode."""

    def __init__(self, val):
        """Initialize MergeJoinNodePrinter."""
        super().__init__(val, 3, "MergeJoin")

        # Manually add the collation ops.
        collationOps = Vector(self.val["_collation"])
        collationChild = (
            "Collation[" + ", ".join(str(collation) for collation in collationOps) + "]"
        )
        self.add_child(collationChild)

        # Manually add the child which prints the sets of keys.
        leftKeys = Vector(self.val["_leftKeys"])
        rightKeys = Vector(self.val["_rightKeys"])
        self.add_child(print_eq_join_condition(leftKeys, rightKeys))

    def to_string(self):
        return "MergeJoin"


def print_collation_req(req):
    spec = Vector(req["_spec"])
    return ", ".join(
        str(entry["first"]) + ": " + strip_namespace(entry["second"]) for entry in spec
    )


class SortedMergeNodePrinter(DynamicArityNodePrinter):
    """Pretty-printer for SortedMergeNode."""

    def __init__(self, val):
        """Initialize SortedMergeNodePrinter."""
        super().__init__(val, 2, "MergeJoin")

        self.add_child("collation[" + print_collation_req(self.val["_collationReq"]) + "]")

    def to_string(self):
        return "SortedMerge"


class NestedLoopJoinNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for NestedLoopJoinNode."""

    def __init__(self, val):
        """Initialize NestedLoopJoinNodePrinter."""
        super().__init__(val, 3, "NestedLoopJoin")

    def to_string(self):
        correlated = print_correlated_projections(self.val["_correlatedProjectionNames"])
        return (
            "NestedLoopJoin[type="
            + strip_namespace(self.val["_joinType"])
            + ", "
            + correlated
            + "]"
        )


class UnwindNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for UnwindNode."""

    def __init__(self, val):
        """Initialize UnwindNodePrinter."""
        super().__init__(val, 3, "Unwind")


class UniqueNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for UniqueNode."""

    def __init__(self, val):
        """Initialize UniqueNodePrinter."""
        super().__init__(val, 2, "Unique")


class SpoolProducerNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for SpoolProducerNode."""

    def __init__(self, val):
        """Initialize SpoolProducerNodePrinter."""
        super().__init__(val, 4, "SpoolProducer")

    def to_string(self):
        return (
            "SpoolProducer["
            + strip_namespace(self.val["_type"])
            + ", id:"
            + str(self.val["_spoolId"])
            + "]"
        )


class SpoolConsumerNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for SpoolConsumerNode."""

    def __init__(self, val):
        """Initialize SpoolConsumerNodePrinter."""
        super().__init__(val, 1, "SpoolConsumer")

    def to_string(self):
        return (
            "SpoolConsumer["
            + strip_namespace(self.val["_type"])
            + ", id:"
            + str(self.val["_spoolId"])
            + "]"
        )


class CollationNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for CollationNode."""

    def __init__(self, val):
        """Initialize CollationNodePrinter."""

        # Don't print the references, will print them inline below.
        super().__init__(val, 1, "Collation")

    def to_string(self):
        return "Collation[" + print_collation_req(self.val["_collationSpec"]) + "]"


class LimitSkipNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for LimitSkipNode."""

    def __init__(self, val):
        """Initialize LimitSkipNodePrinter."""
        super().__init__(val, 1, "LimitSkip")

    def to_string(self):
        return (
            "LimitSkip[limit: "
            + str(self.val["_limit"])
            + ", skip: "
            + str(self.val["_skip"])
            + "]"
        )


class ExchangeNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for ExchangeNode."""

    def __init__(self, val):
        """Initialize ExchangeNodePrinter."""
        super().__init__(val, 2, "Exchange")

    def to_string(self):
        return (
            "Exchange[type: "
            + str(self.val["_distribution"]["_distributionAndProjections"]["_type"])
            + ", projections: "
            + str(self.val["_distribution"]["_distributionAndProjections"]["_projectionNames"])
            + "]"
        )


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
        dynamic_type = f"{OPTIMIZER_NS}::algebra::ControlBlockVTable<{poly_type.name}, "
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


class AtomPrinter(object):
    """Pretty-printer for Atom."""

    def __init__(self, val):
        """Initialize AtomPrinter."""
        self.val = val

    def to_string(self):
        return self.val["_expr"]


class ConjunctionPrinter(object):
    """Pretty-printer for Conjunction."""

    def __init__(self, val, separator=" ^ "):
        """Initialize ConjunctionPrinter."""
        self.val = val
        self.dynamic_nodes = Vector(self.val["_dyNodes"])
        self.dynamic_count = self.dynamic_nodes.count()
        self.separator = separator

    def to_string(self):
        if self.dynamic_count == 0:
            return "<empty>"

        res = ""
        first = True
        for child in self.dynamic_nodes:
            if first:
                first = False
            else:
                res += self.separator

            res += str(child)
        return res


class DisjunctionPrinter(ConjunctionPrinter):
    """Pretty-printer for Disjunction."""

    def __init__(self, val):
        super().__init__(val, " U ")


def register_optimizer_printers(pp):
    """Registers a number of pretty printers related to the CQF optimizer."""

    # Memo printer.
    pp.add("Memo", f"{OPTIMIZER_NS}::cascades::Memo", False, MemoPrinter)

    # Utility types within the optimizer.
    pp.add(
        "StrongStringAlias", f"{OPTIMIZER_NS}::StrongStringAlias", True, StrongStringAliasPrinter
    )
    pp.add(
        "FieldProjectionMap",
        f"{OPTIMIZER_NS}::FieldProjectionMap",
        False,
        FieldProjectionMapPrinter,
    )

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
        "If",
        "Let",
        "LambdaAbstraction",
        "LambdaApplication",
        "FunctionCall",
        "EvalPath",
        "EvalFilter",
        "Source",
        "PathConstant",
        "PathLambda",
        "PathIdentity",
        "PathDefault",
        "PathCompare",
        "PathDrop",
        "PathKeep",
        "PathObj",
        "PathArr",
        "PathTraverse",
        "PathField",
        "PathGet",
        "PathComposeM",
        "PathComposeA",
        "ScanNode",
        "PhysicalScanNode",
        "ValueScanNode",
        "CoScanNode",
        "IndexScanNode",
        "SeekNode",
        "FilterNode",
        "EvaluationNode",
        "RIDIntersectNode",
        "RIDUnionNode",
        "BinaryJoinNode",
        "HashJoinNode",
        "MergeJoinNode",
        "SortedMergeNode",
        "NestedLoopJoinNode",
        "UnionNode",
        "GroupByNode",
        "UnwindNode",
        "UniqueNode",
        "SpoolProducerNode",
        "SpoolConsumerNode",
        "CollationNode",
        "LimitSkipNode",
        "ExchangeNode",
        "RootNode",
        "References",
        "ExpressionBinder",
    ]
    for abt_type in abt_type_set:
        pp.add(
            abt_type,
            f"{OPTIMIZER_NS}::{abt_type}",
            False,
            getattr(sys.modules[__name__], abt_type + "Printer"),
        )

    # Add the generic PolyValue printer which determines the exact type at runtime and attempts to
    # invoke the printer for that type.
    pp.add("PolyValue", OPTIMIZER_NS + "::algebra::PolyValue", True, PolyValuePrinter)
