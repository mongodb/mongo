/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_BinToken_h
#define frontend_BinToken_h

/**
 * Definition of Binary AST tokens.
 *
 * In the Binary AST world, an AST is composed of nodes, where a node is
 * defined by:
 * - a Kind (see `BinKind`);
 * - a list of fields, where each field is:
 *    - a Name (see `BinField`);
 *    - a Value, which may be either a node or a primitive value.
 *
 * The mapping between Kind and list of fields is determined entirely by
 * the grammar of Binary AST. The mapping between (Kind, Name) and the structure
 * of Value is also determined entirely by the grammar of Binary AST.
 *
 * As per the specifications of Binary AST, kinds may be added as the language
 * grows, but never removed. The mapping between Kind and list of fields may
 * also change to add new fields or make some fields optional, but may never
 * remove a field. Finally, the mapping between (Kind, Name) and the structure
 * of Value may be modified to add new possible values, but never to remove a
 * value.
 *
 * A Binary AST parser must be able to fail gracefully when confronted with
 * unknown Kinds or Names.
 *
 * At the time of this writing, the Binary AST defined from the Babylon AST
 * (see https://github.com/babel/babylon/blob/master/ast/spec.md) restricted
 * to ES5, with a few amendments to store additional scoping data and to
 * represent the empty AST.
 *
 * Future versions of the Binary AST will progressively grow to encompass ES6
 * and beyond.
 */

namespace js {
namespace frontend {

 /**
 * The different kinds of Binary AST nodes, as per the specifications of
 * Binary AST.
 *
 * These kinds match roughly with the `ParseNodeKind` used internally.
 *
 * Usage:
 *
 * ```c++
 * #define WITH_KIND(CPP_NAME, SPEC_NAME) ...
 * FOR_EACH_BIN_KIND(WITH_KIND)
 * ```
 *
 *
 * (sorted by alphabetical order)
 */
#define FOR_EACH_BIN_KIND(F) \
    F(ArrayExpression, ArrayExpression) \
    F(AssignmentExpression, AssignmentExpression) \
    F(AssignmentOperator, AssignmentOperator) \
    F(BinaryExpression, BinaryExpression) \
    F(BinaryOperator, BinaryOperator) \
    F(BINJS_Scope, BINJS:Scope) \
    F(BlockStatement, BlockStatement) \
    F(BooleanLiteral, BooleanLiteral) \
    F(BracketExpression, BracketExpression) \
    F(BreakStatement, BreakStatement) \
    F(CallExpression, CallExpression) \
    F(CatchClause, CatchClause) \
    F(ComputedPropertyName, ComputedPropertyName) \
    F(ConditionalExpression, ConditionalExpression) \
    F(ContinueStatement, ContinueStatement) \
    F(DebuggerStatement, DebuggerStatement) \
    F(Declaration, Declaration) \
    F(Directive, Directive) \
    F(DirectiveLiteral, DirectiveLiteral) \
    F(DotExpression, DotExpression) \
    F(DoWhileStatement, DoWhileStatement) \
    F(Elision, Elision) \
    F(EmptyStatement, EmptyStatement) \
    F(Expression, Expression) \
    F(ExpressionStatement, ExpressionStatement) \
    F(ForStatement, ForStatement) \
    F(ForInStatement, ForInStatement) \
    F(FunctionExpression, FunctionExpression) \
    F(FunctionDeclaration, FunctionDeclaration) \
    F(Identifier, Identifier) \
    F(IfStatement, IfStatement) \
    F(LabeledStatement, LabeledStatement) \
    F(Literal, Literal) \
    F(LogicalExpression, LogicalExpression) \
    F(LogicalOperator, LogicalOperator) \
    F(NewExpression, NewExpression) \
    F(NullLiteral, NullLiteral) \
    F(NumericLiteral, NumericLiteral) \
    F(ObjectExpression, ObjectExpression) \
    F(ObjectGetter, ObjectGetter) \
    F(ObjectMethod, ObjectMethod) \
    F(ObjectSetter, ObjectSetter) \
    F(ObjectProperty, ObjectProperty) \
    F(Pattern, Pattern) \
    F(Program, Program) \
    F(PropertyKind, PropertyKind) \
    F(RegExpLiteral, RegExpLiteral) \
    F(ReturnStatement, ReturnStatement) \
    F(SequenceExpression, SequenceExpression) \
    F(StringLiteral, StringLiteral) \
    F(Statement, Statement) \
    F(SwitchCase, SwitchCase) \
    F(SwitchStatement, SwitchStatement) \
    F(ThisExpression, ThisExpression) \
    F(ThrowStatement, ThrowStatement) \
    F(TryStatement, TryStatement) \
    F(UnaryExpression, UnaryExpression) \
    F(UnaryOperator, UnaryOperator) \
    F(UpdateExpression, UpdateExpression) \
    F(UpdateOperator, UpdateOperator) \
    F(VariableDeclaration, VariableDeclaration) \
    F(VariableDeclarator, VariableDeclarator) \
    F(VariableKind, VariableKind) \
    F(WhileStatement, WhileStatement) \
    F(WithStatement, WithStatement)

enum class BinKind {
#define EMIT_ENUM(name, _) name,
    FOR_EACH_BIN_KIND(EMIT_ENUM)
#undef EMIT_ENUM
    BINKIND_LIMIT /* domain size */
};

const char* describeBinKind(const BinKind& kind);

/**
 * The different fields of Binary AST nodes, as per the specifications of
 * Binary AST.
 *
 * Usage:
 *
 * ```c++
 * #define WITH_FIELD(CPP_NAME, SPEC_NAME) ...
 * FOR_EACH_BIN_FIELD(WITH_FIELD)
 * ```
 *
 * (sorted by alphabetical order)
 */
 #define FOR_EACH_BIN_FIELD(F) \
    F(Alternate, alternate) \
    F(Argument, argument) \
    F(Arguments, arguments) \
    F(BINJS_CapturedNames, BINJS:CapturedNames) \
    F(BINJS_ConstDeclaredNames, BINJS:ConstDeclaredNames) \
    F(BINJS_HasDirectEval, BINJS:HasDirectEval) \
    F(BINJS_LetDeclaredNames, BINJS:LetDeclaredNames) \
    F(BINJS_VarDeclaredNames, BINJS:VarDeclaredNames) \
    F(BINJS_Scope, BINJS:Scope) \
    F(Block, block) \
    F(Callee, callee) \
    F(Cases, cases) \
    F(Consequent, consequent) \
    F(Body, body) \
    F(Declarations, declarations) \
    F(Directives, directives) \
    F(Discriminant, discriminant) \
    F(Elements, elements) \
    F(Expression, expression) \
    F(Expressions, expressions) \
    F(Finalizer, finalizer) \
    F(Flags, flags) \
    F(Handler, handler) \
    F(Id, id) \
    F(Init, init) \
    F(Key, key) \
    F(Kind, kind) \
    F(Label, label) \
    F(Left, left) \
    F(Name, name) \
    F(Object, object) \
    F(Operator, operator) \
    F(Param, param) \
    F(Params, params) \
    F(Pattern, pattern) \
    F(Prefix, prefix) \
    F(Properties, properties) \
    F(Property, property) \
    F(Right, right) \
    F(Test, test) \
    F(Update, update) \
    F(Value, value)

enum class BinField {
#define EMIT_ENUM(name, _) name,
    FOR_EACH_BIN_FIELD(EMIT_ENUM)
#undef EMIT_ENUM
    BINFIELD_LIMIT /* domain size */
};

const char* describeBinField(const BinField& kind);

} // namespace frontend
} // namespace js

#endif // frontend_BinToken_h
