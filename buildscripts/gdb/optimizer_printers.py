"""GDB Pretty-printers for types in mongo::optimizer."""

import os
import sys
from pathlib import Path
import gdb
import gdb.printing

if not gdb:
    sys.path.insert(0, str(Path(os.path.abspath(__file__)).parent.parent.parent))
    from buildscripts.gdb.mongo import get_boost_optional, lookup_type


def eval_print_fn(val, print_fn):
    """Evaluate a print function, and return the resulting string."""

    # The generated output from explain contains the string "\n" (two characters)
    # replace them with a single EOL character so that GDB prints multi-line
    # explains nicely.
    pp_result = print_fn(val)
    pp_str = str(pp_result).replace("\"", "").replace("\\n", "\n")
    return pp_str


class OptimizerTypePrinter(object):
    """Base class that pretty prints via a single argument C++ function."""

    def __init__(self, val, print_fn_name):
        """Initialize base printer."""
        self.val = val
        (print_fn_symbol, _) = gdb.lookup_symbol(print_fn_name)
        if print_fn_symbol is None:
            raise gdb.GdbError("Could not find pretty print function: " + print_fn_name)
        self.print_fn = print_fn_symbol.value()

    @staticmethod
    def display_hint():
        """Display hint."""
        return None

    def to_string(self):
        """Return string for printing."""
        return eval_print_fn(self.val, self.print_fn)


class IntervalPrinter(OptimizerTypePrinter):
    """Pretty-printer for mongo::optimizer::IntervalRequirement."""

    def __init__(self, val):
        """Initialize IntervalPrinter."""
        super().__init__(val, "ExplainGenerator::explainInterval")


class CandidateIndexEntryPrinter(OptimizerTypePrinter):
    """Pretty-printer for mongo::optimizer::CandidateIndexEntry."""

    def __init__(self, val):
        """Initialize CandidateIndexEntryPrinter."""
        super().__init__(val, "ExplainGenerator::explainCandidateIndex")


class IntervalExprPrinter(OptimizerTypePrinter):
    """Pretty-printer for mongo::optimizer::IntervalRequirement::Node."""

    def __init__(self, val):
        """Initialize IntervalExprPrinter."""
        super().__init__(val, "ExplainGenerator::explainIntervalExpr")


class PartialSchemaReqMapPrinter(OptimizerTypePrinter):
    """Pretty-printer for mongo::optimizer::PartialSchemaRequirements."""

    def __init__(self, val):
        """Initialize PartialSchemaReqMapPrinter."""
        super().__init__(val, "ExplainGenerator::explainPartialSchemaReqMap")


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

        prior_indent = ABTPrinter.indent_level
        current_indent = ABTPrinter.indent_level + self.arity + len(self.custom_children) - 1
        for child in self.custom_children:
            lhs = "\n"
            for _ in range(current_indent):
                lhs += "|   "

            ABTPrinter.indent_level = current_indent
            yield lhs, child
            current_indent -= 1

        for i in range(self.arity):
            lhs = "\n"
            for _ in range(current_indent):
                lhs += "|   "

            # A little weird, but most ABTs prefer to print the last child first.
            ABTPrinter.indent_level = current_indent
            yield lhs, self.val["_nodes"][self.arity - i - 1]
            current_indent -= 1
        ABTPrinter.indent_level = prior_indent

    # Adds a custom child node which is not directly contained in the "_nodes" member variable.
    def add_child(self, child):
        self.custom_children.append(child)

    def to_string(self):
        # Default for nodes which just print their type.
        return self.name


class Vector(object):
    def __init__(self, vec):
        self.vec = vec
        self.start = vec['_M_impl']['_M_start']
        self.finish = vec['_M_impl']['_M_finish']

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
            raise gdb.GdbError("Invalid Vector access at index {} with size {}".format(
                index, self.count()))
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

        prior_indent = ABTPrinter.indent_level
        ABTPrinter.indent_level += self.dynamic_count
        for res in super().children():
            yield res

        current_indent = ABTPrinter.indent_level - 1
        for child in self.dynamic_nodes:
            lhs = "\n"
            for _ in range(current_indent):
                lhs += "|   "

            ABTPrinter.indent_level = current_indent
            yield lhs, child
            current_indent -= 1
        ABTPrinter.indent_level = prior_indent


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
        bound_projections = ABTPrinter.get_bound_projections(self.binder)
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
        value_print_fn = "sbe::value::print"
        (print_fn_symbol, _) = gdb.lookup_symbol(value_print_fn)
        if print_fn_symbol is None:
            raise gdb.GdbError("Could not find pretty print function: " + value_print_fn)
        print_fn = print_fn_symbol.value()
        return print_fn(tag, value)

    def to_string(self):
        return "Constant[{}]".format(
            ConstantPrinter.print_sbe_value(self.val["_tag"], self.val["_val"]))


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
        projections = Vector(self.val["_property"]["_projections"]["_vector"])
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
        return "PathCompare[{}]".format(op_to_string(self.val["_cmp"]))


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


def op_to_string(op):
    return str(op).split("::")[-1]


class UnaryOpPrinter(FixedArityNodePrinter):
    """Pretty-printer for UnaryOp."""

    def __init__(self, val):
        """Initialize UnaryOpPrinter."""
        super().__init__(val, 1, "UnaryOp")

    def to_string(self):
        return "UnaryOp[{}]".format(op_to_string(self.val["_op"]))


class BinaryOpPrinter(FixedArityNodePrinter):
    """Pretty-printer for BinaryOp."""

    def __init__(self, val):
        """Initialize BinaryOpPrinter."""
        super().__init__(val, 2, "BinaryOp")

    def to_string(self):
        return "BinaryOp[{}]".format(op_to_string(self.val["_op"]))


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
        # Rely on default printer for std::set, but remove the extra metadata at the start.
        field_projections = self.val["_fieldProjections"]
        res += "<empty>" if field_projections["size_"] == 0 else str(field_projections).split(
            "elems  =")[-1]
        res += "}"
        return res


class PhysicalScanNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for PhysicalScanNode."""

    def __init__(self, val):
        """Initialize PhysicalScanNodePrinter."""
        super().__init__(val, 1, "PhysicalScan")

    def to_string(self):
        return "PhysicalScan[{}, {}]".format(
            str(self.val["_fieldProjectionMap"]), str(self.val["_scanDefName"]))


class ValueScanNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for ValueScanNode."""

    def __init__(self, val):
        """Initialize ValueScanNodePrinter."""
        super().__init__(val, 1, "ValueScan")

    def to_string(self):
        return "ValueScan[hasRID={},arraySize={}]".format(self.val["_hasRID"],
                                                          self.val["_arraySize"])


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
        return "IndexScan[{{{}}}, scanDef={}, indexDef={}, interval={}]".format(
            self.val["_fieldProjectionMap"], self.val["_scanDefName"], self.val["_indexDefName"],
            self.val["_indexInterval"]).replace("\n", "")


class SeekNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for SeekNode."""

    def __init__(self, val):
        """Initialize SeekNodePrinter."""
        super().__init__(val, 2, "Seek")

    def to_string(self):
        return "Seek[rid_projection: {}, {}, scanDef: {}]".format(self.val["_ridProjectionName"],
                                                                  self.val["_fieldProjectionMap"],
                                                                  self.val["_scanDefName"])


class MemoLogicalDelegatorNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for MemoLogicalDelegatorNode."""

    def __init__(self, val):
        """Initialize MemoLogicalDelegatorNodePrinter."""
        super().__init__(val, 0, "MemoLogicalDelegator")

    def to_string(self):
        return "MemoLogicalDelegator[{}]".format(self.val["_groupId"])


class MemoPhysicalDelegatorNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for MemoPhysicalDelegatorNode."""

    def __init__(self, val):
        """Initialize MemoPhysicalDelegatorNodePrinter."""
        super().__init__(val, 0, "MemoPhysicalDelegator")

    def to_string(self):
        return "MemoPhysicalDelegator[group: {}, index: {}]".format(self.val["_nodeId"]["_groupId"],
                                                                    self.val["_nodeId"]["_index"])


class ResidualRequirementPrinter(object):
    """Pretty-printer for ResidualRequirement."""

    def __init__(self, val):
        """Initialize ResidualRequirementPrinter."""
        self.val = val

    def to_string(self):
        key = self.val["_key"]
        req = self.val["_req"]
        res = "<"
        if get_boost_optional(key["_projectionName"]) is not None:
            res += "refProj: " + str(get_boost_optional(key["_projectionName"])) + ", "

        res += "path: '" + str(key["_path"]).replace("|   ", "").replace("\n", " -> ") + "'"

        if get_boost_optional(req["_boundProjectionName"]) is not None:
            res += "boundProj: " + str(get_boost_optional(req["_boundProjectionName"])) + ", "

        res += ">"
        return res


class SargableNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for SargableNode."""

    def __init__(self, val):
        """Initialize SargableNodePrinter."""
        # Although Sargable technically has 3 children, avoid printing the refs (child1) and bind block (child2).
        super().__init__(val, 1, "Sargable")

        # Add children for requirements, candidateIndex, and scan_params.
        self.add_child(str(self.val["_reqMap"]).replace("\n", ""))
        self.add_child(self.print_candidate_indexes())

        self.scan_params = get_boost_optional(self.val["_scanParams"])
        if self.scan_params is not None:
            self.add_child(self.print_scan_params())

    def print_scan_params(self):
        res = "scan_params: (proj: " + str(self.scan_params["_fieldProjectionMap"]) + ", "
        residual_reqs = get_boost_optional(self.scan_params["_residualRequirements"])
        if residual_reqs is not None:
            res += "residual: " + str(residual_reqs)
        res += ")"
        return res

    def print_candidate_indexes(self):
        res = "candidateIndexes: ["
        indexes = Vector(self.val["_candidateIndexes"])
        for i in range(indexes.count()):
            if i > 0:
                res += ", "
            res += "<id: " + str(i) + ", " + str(indexes.get(i)).replace("\n", "") + ">"
        res += "]"
        return res

    @staticmethod
    def index_req_to_string(index_req):
        req_map = ["Index", "Seek", "Complete"]
        return req_map[index_req]

    def to_string(self):
        return "Sargable [" + self.index_req_to_string(self.val["_target"]) + "]"


class RIDIntersectNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for RIDIntersectNode."""

    def __init__(self, val):
        """Initialize RIDIntersectNodePrinter."""
        super().__init__(val, 2, "RIDIntersect")


class RIDUnionNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for RIDUnionNode."""

    def __init__(self, val):
        """Initialize RIDUnionNodePrinter."""
        super().__init__(val, 2, "RIDUnion")


class BinaryJoinNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for BinaryJoinNode."""

    def __init__(self, val):
        """Initialize BinaryJoinNodePrinter."""
        super().__init__(val, 3, "BinaryJoin")


class HashJoinNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for HashJoinNode."""

    def __init__(self, val):
        """Initialize HashJoinNodePrinter."""
        super().__init__(val, 3, "HashJoin")


class MergeJoinNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for MergeJoinNode."""

    def __init__(self, val):
        """Initialize MergeJoinNodePrinter."""
        super().__init__(val, 3, "MergeJoin")


class SortedMergeNodePrinter(DynamicArityNodePrinter):
    """Pretty-printer for SortedMergeNode."""

    def __init__(self, val):
        """Initialize SortedMergeNodePrinter."""
        super().__init__(val, 2, "MergeJoin")


class NestedLoopJoinNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for NestedLoopJoinNode."""

    def __init__(self, val):
        """Initialize NestedLoopJoinNodePrinter."""
        super().__init__(val, 3, "NestedLoopJoin")


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


class SpoolConsumerNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for SpoolConsumerNode."""

    def __init__(self, val):
        """Initialize SpoolConsumerNodePrinter."""
        super().__init__(val, 1, "SpoolConsumer")


class CollationNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for CollationNode."""

    def __init__(self, val):
        """Initialize CollationNodePrinter."""
        super().__init__(val, 2, "Collation")


class LimitSkipNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for LimitSkipNode."""

    def __init__(self, val):
        """Initialize LimitSkipNodePrinter."""
        super().__init__(val, 1, "LimitSkip")


class ExchangeNodePrinter(FixedArityNodePrinter):
    """Pretty-printer for ExchangeNode."""

    def __init__(self, val):
        """Initialize ExchangeNodePrinter."""
        super().__init__(val, 2, "Exchange")


class ReferencesPrinter(DynamicArityNodePrinter):
    """Pretty-printer for References."""

    def __init__(self, val):
        """Initialize ReferencesPrinter."""
        super().__init__(val, 0, "References")


class PolyValuePrinter(object):
    """Pretty-printer for PolyValue."""

    # This printer must be given the full set of variant types that the PolyValue can hold in
    # 'type_set'.
    def __init__(self, type_set, type_namespace, val):
        """Initialize PolyValuePrinter."""
        self.val = val
        self.control_block = self.val["_object"]
        self.tag = self.control_block.dereference()["_tag"]
        self.type_set = type_set
        self.type_namespace = type_namespace

        if self.tag < 0:
            raise gdb.GdbError("Invalid PolyValue tag: {}, must be at least 0".format(self.tag))

        # Check if the tag is out of range for the set of types that we know about.
        if self.tag > len(self.type_set):
            raise gdb.GdbError("Unknown PolyValue tag: {} (max: {}), did you add a new one?".format(
                self.tag, len(self.type_set)))

    @staticmethod
    def display_hint():
        """Display hint."""
        return None

    def cast_control_block(self, target_type):
        return self.control_block.dereference().address.cast(
            target_type.pointer()).dereference()["_t"]

    def get_dynamic_type(self):
        # Build up the dynamic type for the particular variant of this PolyValue instance. This is
        # basically a reinterpret cast of the '_object' member variable to the correct instance
        # of ControlBlockVTable<T, Ts...>::ConcreteType where T is the template argument derived
        # from the _tag member variable.
        poly_type = self.val.type.template_argument(self.tag)
        dynamic_type = "mongo::optimizer::algebra::ControlBlockVTable<" + poly_type.name
        for i in range(len(self.type_set)):
            if i < len(self.type_set):
                dynamic_type += ", "
            dynamic_type += self.type_namespace + self.type_set[i]
        dynamic_type += ">::ConcreteType"
        return dynamic_type

    def to_string(self):
        dynamic_type = self.get_dynamic_type()
        try:
            dynamic_type = lookup_type(dynamic_type).strip_typedefs()
        except gdb.error:
            return "Unknown PolyValue tag: {}, did you add a new one?".format(self.tag)
        # GDB automatically formats types with children, remove the extra characters to get the
        # output that we want.
        return str(self.cast_control_block(dynamic_type)).replace(" = ", "").replace("{",
                                                                                     "").replace(
                                                                                         "}", "")


class ABTPrinter(PolyValuePrinter):
    """Pretty-printer for ABT."""

    indent_level = 0
    abt_namespace = "mongo::optimizer::"

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
        "MemoLogicalDelegatorNode",
        "MemoPhysicalDelegatorNode",
        "FilterNode",
        "EvaluationNode",
        "SargableNode",
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

    @staticmethod
    def get_bound_projections(node):
        # Casts the input node to an ExpressionBinder and returns the set of bound projection names.
        pp = PolyValuePrinter(ABTPrinter.abt_type_set, ABTPrinter.abt_namespace, node)
        dynamic_type = lookup_type(pp.get_dynamic_type()).strip_typedefs()
        binder = pp.cast_control_block(dynamic_type)
        return Vector(binder["_names"])

    def __init__(self, val):
        """Initialize ABTPrinter."""
        super().__init__(ABTPrinter.abt_type_set, ABTPrinter.abt_namespace, val)


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


class BoolExprPrinter(PolyValuePrinter):
    """Pretty-printer for BoolExpr."""

    type_set = ["Atom", "Conjunction", "Disjunction"]

    def __init__(self, val, template_type):
        """Initialize BoolExprPrinter."""
        namespace = "mongo::optimizer::BoolExpr<" + template_type + ">::"
        super().__init__(BoolExprPrinter.type_set, namespace, val)


class ResidualReqExprPrinter(BoolExprPrinter):
    """Pretty-printer for BoolExpr<ResidualRequirement>."""

    def __init__(self, val):
        """Initialize ResidualReqExprPrinter."""
        super().__init__(val, "mongo::optimizer::ResidualRequirement")


def register_abt_printers(pp):
    """Registers a number of pretty printers related to the CQF optimizer."""

    # IntervalRequirement printer.
    pp.add("Interval", "mongo::optimizer::IntervalRequirement", False, IntervalPrinter)
    pp.add("CompoundInterval", "mongo::optimizer::CompoundIntervalRequirement", False,
           IntervalPrinter)

    # IntervalReqExpr::Node printer.
    pp.add(
        "IntervalExpr",
        ("mongo::optimizer::algebra::PolyValue<" +
         "mongo::optimizer::BoolExpr<mongo::optimizer::IntervalRequirement>::Atom, " +
         "mongo::optimizer::BoolExpr<mongo::optimizer::IntervalRequirement>::Conjunction, " +
         "mongo::optimizer::BoolExpr<mongo::optimizer::IntervalRequirement>::Disjunction>"),
        False,
        IntervalExprPrinter,
    )

    # Memo printer.
    pp.add("Memo", "mongo::optimizer::cascades::Memo", False, MemoPrinter)

    # PartialSchemaRequirements printer.
    pp.add("PartialSchemaRequirements", "mongo::optimizer::PartialSchemaRequirements", False,
           PartialSchemaReqMapPrinter)

    # ResidualRequirement printer.
    pp.add("ResidualRequirement", "mongo::optimizer::ResidualRequirement", False,
           ResidualRequirementPrinter)

    # CandidateIndexEntry printer.
    pp.add("CandidateIndexEntry", "mongo::optimizer::CandidateIndexEntry", False,
           CandidateIndexEntryPrinter)

    pp.add(
        "ResidualRequirementExpr",
        ("mongo::optimizer::algebra::PolyValue<" +
         "mongo::optimizer::BoolExpr<mongo::optimizer::ResidualRequirement>::Atom, " +
         "mongo::optimizer::BoolExpr<mongo::optimizer::ResidualRequirement>::Conjunction, " +
         "mongo::optimizer::BoolExpr<mongo::optimizer::ResidualRequirement>::Disjunction>"),
        False,
        ResidualReqExprPrinter,
    )
    for bool_type in BoolExprPrinter.type_set:
        pp.add(bool_type,
               "mongo::optimizer::BoolExpr<mongo::optimizer::ResidualRequirement>::" + bool_type,
               False, getattr(sys.modules[__name__], bool_type + "Printer"))

    # Utility types within the optimizer.
    pp.add("StrongStringAlias", "mongo::optimizer::StrongStringAlias", True,
           StrongStringAliasPrinter)
    pp.add("FieldProjectionMap", "mongo::optimizer::FieldProjectionMap", False,
           FieldProjectionMapPrinter)

    # Attempt to dynamically load the ABT type since it has a templated type set that is bound to
    # change. This may fail on certain builds, such as those with dynamically linked libraries, so
    # we catch the lookup error and fallback to registering the static type name which may be
    # stale.
    try:
        # ABT printer.
        abt_type = lookup_type("mongo::optimizer::ABT").strip_typedefs()
        pp.add('ABT', abt_type.name, False, ABTPrinter)

        abt_ref_type = abt_type.name + "::Reference"
        # We can re-use the same printer since an ABT is contructable from an ABT::Reference.
        pp.add('ABT::Reference', abt_ref_type, False, ABTPrinter)
    except gdb.error:
        # ABT printer.
        abt_type = "mongo::optimizer::algebra::PolyValue<"
        for type_name in ABTPrinter.abt_type_set:
            abt_type += "mongo::optimizer::" + type_name
            if type_name != "ExpressionBinder":
                abt_type += ", "
        abt_type += ">"
        pp.add('ABT', abt_type, False, ABTPrinter)

        abt_ref_type = abt_type + "::Reference"
        # We can re-use the same printer since an ABT is contructable from an ABT::Reference.
        pp.add('ABT::Reference', abt_ref_type, False, ABTPrinter)

    # Add the sub-printers for each of the possible ABT types.
    for abt_type in ABTPrinter.abt_type_set:
        pp.add(abt_type, "mongo::optimizer::" + abt_type, False,
               getattr(sys.modules[__name__], abt_type + "Printer"))
