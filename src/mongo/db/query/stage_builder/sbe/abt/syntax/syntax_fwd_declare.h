// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

namespace mongo::abt {

/**
 * Expressions
 */
class Blackhole;
class Constant;
class Variable;
class UnaryOp;
class BinaryOp;
class NaryOp;
class If;
class Let;
class MultiLet;
class LambdaAbstraction;
class FunctionCall;
class Source;
class Switch;

/**
 * Utility
 */
class References;
class ExpressionBinder;

class ExpressionSyntaxSort;
}  // namespace mongo::abt
