/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

namespace mongo::optimizer {

/**
 * Expressions
 */
class Blackhole;
class Constant;
class Variable;
class UnaryOp;
class BinaryOp;
class If;
class Let;
class LambdaAbstraction;
class LambdaApplication;
class FunctionCall;
class EvalPath;
class EvalFilter;
class Source;

/**
 * Path elements
 */
class PathConstant;
class PathLambda;
class PathIdentity;
class PathDefault;
class PathCompare;
class PathDrop;
class PathKeep;
class PathObj;
class PathArr;
class PathTraverse;
class PathField;
class PathGet;
class PathComposeM;
class PathComposeA;

/**
 * Nodes
 */
class ScanNode;
class PhysicalScanNode;
class ValueScanNode;
class CoScanNode;
class IndexScanNode;
class SeekNode;
class MemoLogicalDelegatorNode;
class MemoPhysicalDelegatorNode;
class FilterNode;
class EvaluationNode;
class SargableNode;
class RIDIntersectNode;
class BinaryJoinNode;
class HashJoinNode;
class MergeJoinNode;
class UnionNode;
class GroupByNode;
class UnwindNode;
class UniqueNode;
class CollationNode;
class LimitSkipNode;
class ExchangeNode;
class RootNode;

/**
 * Utility
 */
class References;
class ExpressionBinder;

class PathSyntaxSort;
class ExpressionSyntaxSort;
}  // namespace mongo::optimizer
