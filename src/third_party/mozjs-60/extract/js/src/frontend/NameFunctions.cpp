/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/NameFunctions.h"

#include "mozilla/MemoryChecking.h"
#include "mozilla/Sprintf.h"

#include "frontend/BytecodeCompiler.h"
#include "frontend/ParseNode.h"
#include "frontend/SharedContext.h"
#include "util/StringBuffer.h"
#include "vm/JSFunction.h"

using namespace js;
using namespace js::frontend;

namespace {

class NameResolver
{
    static const size_t MaxParents = 100;

    JSContext* cx;
    size_t nparents;                /* number of parents in the parents array */
    MOZ_INIT_OUTSIDE_CTOR
    ParseNode* parents[MaxParents]; /* history of ParseNodes we've been looking at */
    StringBuffer* buf;              /* when resolving, buffer to append to */

    /* Test whether a ParseNode represents a function invocation */
    bool call(ParseNode* pn) {
        return pn && pn->isKind(ParseNodeKind::Call);
    }

    /*
     * Append a reference to a property named |name| to |buf|. If |name| is
     * a proper identifier name, then we append '.name'; otherwise, we
     * append '["name"]'.
     *
     * Note that we need the IsIdentifier check for atoms from both
     * ParseNodeKind::Name nodes and ParseNodeKind::String nodes:
     * given code like a["b c"], the front end will produce a ParseNodeKind::Dot
     * with a ParseNodeKind::Name child whose name contains spaces.
     */
    bool appendPropertyReference(JSAtom* name) {
        if (IsIdentifier(name))
            return buf->append('.') && buf->append(name);

        /* Quote the string as needed. */
        JSString* source = QuoteString(cx, name, '"');
        return source && buf->append('[') && buf->append(source) && buf->append(']');
    }

    /* Append a number to buf. */
    bool appendNumber(double n) {
        char number[30];
        int digits = SprintfLiteral(number, "%g", n);
        return buf->append(number, digits);
    }

    /* Append "[<n>]" to buf, referencing a property named by a numeric literal. */
    bool appendNumericPropertyReference(double n) {
        return buf->append("[") && appendNumber(n) && buf->append(']');
    }

    /*
     * Walk over the given ParseNode, attempting to convert it to a stringified
     * name that respresents where the function is being assigned to.
     *
     * |*foundName| is set to true if a name is found for the expression.
     */
    bool nameExpression(ParseNode* n, bool* foundName) {
        switch (n->getKind()) {
          case ParseNodeKind::Dot:
            if (!nameExpression(n->expr(), foundName))
                return false;
            if (!*foundName)
                return true;
            return appendPropertyReference(n->pn_atom);

          case ParseNodeKind::Name:
            *foundName = true;
            return buf->append(n->pn_atom);

          case ParseNodeKind::This:
            *foundName = true;
            return buf->append("this");

          case ParseNodeKind::Elem:
            if (!nameExpression(n->pn_left, foundName))
                return false;
            if (!*foundName)
                return true;
            if (!buf->append('[') || !nameExpression(n->pn_right, foundName))
                return false;
            if (!*foundName)
                return true;
            return buf->append(']');

          case ParseNodeKind::Number:
            *foundName = true;
            return appendNumber(n->pn_dval);

          default:
            /* We're confused as to what to call this function. */
            *foundName = false;
            return true;
        }
    }

    /*
     * When naming an anonymous function, the process works loosely by walking
     * up the AST and then translating that to a string. The stringification
     * happens from some far-up assignment and then going back down the parse
     * tree to the function definition point.
     *
     * This function will walk up the parse tree, gathering relevant nodes used
     * for naming, and return the assignment node if there is one. The provided
     * array and size will be filled in, and the returned node could be nullptr
     * if no assignment is found. The first element of the array will be the
     * innermost node relevant to naming, and the last element will be the
     * outermost node.
     */
    ParseNode* gatherNameable(ParseNode** nameable, size_t* size) {
        *size = 0;

        for (int pos = nparents - 1; pos >= 0; pos--) {
            ParseNode* cur = parents[pos];
            if (cur->isAssignment())
                return cur;

            switch (cur->getKind()) {
              case ParseNodeKind::Name:     return cur;  /* found the initialized declaration */
              case ParseNodeKind::This:     return cur;  /* Setting a property of 'this'. */
              case ParseNodeKind::Function: return nullptr; /* won't find an assignment or declaration */

              case ParseNodeKind::Return:
                /*
                 * Normally the relevant parent of a node is its direct parent, but
                 * sometimes with code like:
                 *
                 *    var foo = (function() { return function() {}; })();
                 *
                 * the outer function is just a helper to create a scope for the
                 * returned function. Hence the name of the returned function should
                 * actually be 'foo'.  This loop sees if the current node is a
                 * ParseNodeKind::Return, and if there is a direct function
                 * call we skip to that.
                 */
                for (int tmp = pos - 1; tmp > 0; tmp--) {
                    if (isDirectCall(tmp, cur)) {
                        pos = tmp;
                        break;
                    } else if (call(cur)) {
                        /* Don't skip too high in the tree */
                        break;
                    }
                    cur = parents[tmp];
                }
                break;

              case ParseNodeKind::Colon:
              case ParseNodeKind::Shorthand:
                /*
                 * Record the ParseNodeKind::Colon/SHORTHAND but skip the
                 * ParseNodeKind::Object so we're not flagged as a
                 * contributor.
                 */
                pos--;
                MOZ_FALLTHROUGH;

              default:
                /* Save any other nodes we encounter on the way up. */
                MOZ_ASSERT(*size < MaxParents);
                nameable[(*size)++] = cur;
                break;
            }
        }

        return nullptr;
    }

    /*
     * Resolve the name of a function. If the function already has a name
     * listed, then it is skipped. Otherwise an intelligent name is guessed to
     * assign to the function's displayAtom field.
     */
    bool resolveFun(ParseNode* pn, HandleAtom prefix, MutableHandleAtom retAtom) {
        MOZ_ASSERT(pn != nullptr);
        MOZ_ASSERT(pn->isKind(ParseNodeKind::Function));
        MOZ_ASSERT(pn->isArity(PN_CODE));
        RootedFunction fun(cx, pn->pn_funbox->function());

        StringBuffer buf(cx);
        this->buf = &buf;

        retAtom.set(nullptr);

        /* If the function already has a name, use that */
        if (fun->displayAtom() != nullptr) {
            if (prefix == nullptr) {
                retAtom.set(fun->displayAtom());
                return true;
            }
            if (!buf.append(prefix) ||
                !buf.append('/') ||
                !buf.append(fun->displayAtom()))
                return false;
            retAtom.set(buf.finishAtom());
            return !!retAtom;
        }

        /* If a prefix is specified, then it is a form of namespace */
        if (prefix != nullptr && (!buf.append(prefix) || !buf.append('/')))
            return false;

        /* Gather all nodes relevant to naming */
        ParseNode* toName[MaxParents];
        size_t size;
        ParseNode* assignment = gatherNameable(toName, &size);

        /* If the function is assigned to something, then that is very relevant */
        if (assignment) {
            if (assignment->isAssignment())
                assignment = assignment->pn_left;
            bool foundName = false;
            if (!nameExpression(assignment, &foundName))
                return false;
            if (!foundName)
                return true;
        }

        /*
         * Other than the actual assignment, other relevant nodes to naming are
         * those in object initializers and then particular nodes marking a
         * contribution.
         */
        for (int pos = size - 1; pos >= 0; pos--) {
            ParseNode* node = toName[pos];

            if (node->isKind(ParseNodeKind::Colon) || node->isKind(ParseNodeKind::Shorthand)) {
                ParseNode* left = node->pn_left;
                if (left->isKind(ParseNodeKind::ObjectPropertyName) ||
                    left->isKind(ParseNodeKind::String))
                {
                    if (!appendPropertyReference(left->pn_atom))
                        return false;
                } else if (left->isKind(ParseNodeKind::Number)) {
                    if (!appendNumericPropertyReference(left->pn_dval))
                        return false;
                } else {
                    MOZ_ASSERT(left->isKind(ParseNodeKind::ComputedName));
                }
            } else {
                /*
                 * Don't have consecutive '<' characters, and also don't start
                 * with a '<' character.
                 */
                if (!buf.empty() && buf.getChar(buf.length() - 1) != '<' && !buf.append('<'))
                    return false;
            }
        }

        /*
         * functions which are "genuinely anonymous" but are contained in some
         * other namespace are rather considered as "contributing" to the outer
         * function, so give them a contribution symbol here.
         */
        if (!buf.empty() && buf.getChar(buf.length() - 1) == '/' && !buf.append('<'))
            return false;

        if (buf.empty())
            return true;

        retAtom.set(buf.finishAtom());
        if (!retAtom)
            return false;
        fun->setGuessedAtom(retAtom);
        return true;
    }

    /*
     * Tests whether parents[pos] is a function call whose callee is cur.
     * This is the case for functions which do things like simply create a scope
     * for new variables and then return an anonymous function using this scope.
     */
    bool isDirectCall(int pos, ParseNode* cur) {
        return pos >= 0 && call(parents[pos]) && parents[pos]->pn_head == cur;
    }

    bool resolveTemplateLiteral(ParseNode* node, HandleAtom prefix) {
        MOZ_ASSERT(node->isKind(ParseNodeKind::TemplateStringList));
        ParseNode* element = node->pn_head;
        while (true) {
            MOZ_ASSERT(element->isKind(ParseNodeKind::TemplateString));

            element = element->pn_next;
            if (!element)
                return true;

            if (!resolve(element, prefix))
                return false;

            element = element->pn_next;
        }
    }

    bool resolveTaggedTemplate(ParseNode* node, HandleAtom prefix) {
        MOZ_ASSERT(node->isKind(ParseNodeKind::TaggedTemplate));

        ParseNode* element = node->pn_head;

        // The list head is a leading expression, e.g. |tag| in |tag`foo`|,
        // that might contain functions.
        if (!resolve(element, prefix))
            return false;

        // Next is the callsite object node.  This node only contains
        // internal strings or undefined and an array -- no user-controlled
        // expressions.
        element = element->pn_next;
#ifdef DEBUG
        {
            MOZ_ASSERT(element->isKind(ParseNodeKind::CallSiteObj));
            ParseNode* array = element->pn_head;
            MOZ_ASSERT(array->isKind(ParseNodeKind::Array));
            for (ParseNode* kid = array->pn_head; kid; kid = kid->pn_next)
                MOZ_ASSERT(kid->isKind(ParseNodeKind::TemplateString));
            for (ParseNode* next = array->pn_next; next; next = next->pn_next) {
                MOZ_ASSERT(next->isKind(ParseNodeKind::TemplateString) ||
                           next->isKind(ParseNodeKind::RawUndefined));
            }
        }
#endif

        // Next come any interpolated expressions in the tagged template.
        ParseNode* interpolated = element->pn_next;
        for (; interpolated; interpolated = interpolated->pn_next) {
            if (!resolve(interpolated, prefix))
                return false;
        }

        return true;
    }

  public:
    explicit NameResolver(JSContext* cx) : cx(cx), nparents(0), buf(nullptr) {}

    /*
     * Resolve all names for anonymous functions recursively within the
     * ParseNode instance given. The prefix is for each subsequent name, and
     * should initially be nullptr.
     */
    bool resolve(ParseNode* const cur, HandleAtom prefixArg = nullptr) {
        RootedAtom prefix(cx, prefixArg);
        if (cur == nullptr)
            return true;

        MOZ_ASSERT(cur->isArity(PN_CODE) == (cur->isKind(ParseNodeKind::Function) ||
                                             cur->isKind(ParseNodeKind::Module)));
        if (cur->isKind(ParseNodeKind::Function)) {
            RootedAtom prefix2(cx);
            if (!resolveFun(cur, prefix, &prefix2))
                return false;

            /*
             * If a function looks like (function(){})() where the parent node
             * of the definition of the function is a call, then it shouldn't
             * contribute anything to the namespace, so don't bother updating
             * the prefix to whatever was returned.
             */
            if (!isDirectCall(nparents - 1, cur))
                prefix = prefix2;
        }

        if (nparents >= MaxParents)
            return true;

        auto initialParents = nparents;
        parents[initialParents] = cur;
        nparents++;

        switch (cur->getKind()) {
          // Nodes with no children that might require name resolution need no
          // further work.
          case ParseNodeKind::EmptyStatement:
          case ParseNodeKind::String:
          case ParseNodeKind::TemplateString:
          case ParseNodeKind::RegExp:
          case ParseNodeKind::True:
          case ParseNodeKind::False:
          case ParseNodeKind::Null:
          case ParseNodeKind::RawUndefined:
          case ParseNodeKind::Elision:
          case ParseNodeKind::Generator:
          case ParseNodeKind::Number:
          case ParseNodeKind::Break:
          case ParseNodeKind::Continue:
          case ParseNodeKind::Debugger:
          case ParseNodeKind::ExportBatchSpec:
          case ParseNodeKind::ObjectPropertyName:
          case ParseNodeKind::PosHolder:
            MOZ_ASSERT(cur->isArity(PN_NULLARY));
            break;

          case ParseNodeKind::TypeOfName:
          case ParseNodeKind::SuperBase:
            MOZ_ASSERT(cur->isArity(PN_UNARY));
            MOZ_ASSERT(cur->pn_kid->isKind(ParseNodeKind::Name));
            MOZ_ASSERT(!cur->pn_kid->expr());
            break;

          case ParseNodeKind::NewTarget:
            MOZ_ASSERT(cur->isArity(PN_BINARY));
            MOZ_ASSERT(cur->pn_left->isKind(ParseNodeKind::PosHolder));
            MOZ_ASSERT(cur->pn_right->isKind(ParseNodeKind::PosHolder));
            break;

          // Nodes with a single non-null child requiring name resolution.
          case ParseNodeKind::ExpressionStatement:
          case ParseNodeKind::TypeOfExpr:
          case ParseNodeKind::Void:
          case ParseNodeKind::Not:
          case ParseNodeKind::BitNot:
          case ParseNodeKind::Throw:
          case ParseNodeKind::DeleteName:
          case ParseNodeKind::DeleteProp:
          case ParseNodeKind::DeleteElem:
          case ParseNodeKind::DeleteExpr:
          case ParseNodeKind::Neg:
          case ParseNodeKind::Pos:
          case ParseNodeKind::PreIncrement:
          case ParseNodeKind::PostIncrement:
          case ParseNodeKind::PreDecrement:
          case ParseNodeKind::PostDecrement:
          case ParseNodeKind::ComputedName:
          case ParseNodeKind::Spread:
          case ParseNodeKind::MutateProto:
          case ParseNodeKind::Export:
            MOZ_ASSERT(cur->isArity(PN_UNARY));
            if (!resolve(cur->pn_kid, prefix))
                return false;
            break;

          // Nodes with a single nullable child.
          case ParseNodeKind::This:
            MOZ_ASSERT(cur->isArity(PN_UNARY));
            if (ParseNode* expr = cur->pn_kid) {
                if (!resolve(expr, prefix))
                    return false;
            }
            break;

          // Binary nodes with two non-null children.
          case ParseNodeKind::Assign:
          case ParseNodeKind::AddAssign:
          case ParseNodeKind::SubAssign:
          case ParseNodeKind::BitOrAssign:
          case ParseNodeKind::BitXorAssign:
          case ParseNodeKind::BitAndAssign:
          case ParseNodeKind::LshAssign:
          case ParseNodeKind::RshAssign:
          case ParseNodeKind::UrshAssign:
          case ParseNodeKind::MulAssign:
          case ParseNodeKind::DivAssign:
          case ParseNodeKind::ModAssign:
          case ParseNodeKind::PowAssign:
          case ParseNodeKind::Colon:
          case ParseNodeKind::Shorthand:
          case ParseNodeKind::DoWhile:
          case ParseNodeKind::While:
          case ParseNodeKind::Switch:
          case ParseNodeKind::For:
          case ParseNodeKind::ClassMethod:
          case ParseNodeKind::SetThis:
            MOZ_ASSERT(cur->isArity(PN_BINARY));
            if (!resolve(cur->pn_left, prefix))
                return false;
            if (!resolve(cur->pn_right, prefix))
                return false;
            break;

          case ParseNodeKind::Elem:
            MOZ_ASSERT(cur->isArity(PN_BINARY));
            if (!cur->as<PropertyByValue>().isSuper() && !resolve(cur->pn_left, prefix))
                return false;
            if (!resolve(cur->pn_right, prefix))
                return false;
            break;

          case ParseNodeKind::With:
            MOZ_ASSERT(cur->isArity(PN_BINARY));
            if (!resolve(cur->pn_left, prefix))
                return false;
            if (!resolve(cur->pn_right, prefix))
                return false;
            break;

          case ParseNodeKind::Case:
            MOZ_ASSERT(cur->isArity(PN_BINARY));
            if (ParseNode* caseExpr = cur->pn_left) {
                if (!resolve(caseExpr, prefix))
                    return false;
            }
            if (!resolve(cur->pn_right, prefix))
                return false;
            break;

          case ParseNodeKind::InitialYield:
            MOZ_ASSERT(cur->pn_kid->isKind(ParseNodeKind::Assign) &&
                       cur->pn_kid->pn_left->isKind(ParseNodeKind::Name) &&
                       cur->pn_kid->pn_right->isKind(ParseNodeKind::Generator));
            break;

          case ParseNodeKind::YieldStar:
            MOZ_ASSERT(cur->isArity(PN_UNARY));
            if (!resolve(cur->pn_kid, prefix))
                return false;
            break;

          case ParseNodeKind::Yield:
          case ParseNodeKind::Await:
            MOZ_ASSERT(cur->isArity(PN_UNARY));
            if (cur->pn_kid) {
                if (!resolve(cur->pn_kid, prefix))
                    return false;
            }
            break;

          case ParseNodeKind::Return:
            MOZ_ASSERT(cur->isArity(PN_UNARY));
            if (ParseNode* returnValue = cur->pn_kid) {
                if (!resolve(returnValue, prefix))
                    return false;
            }
            break;

          case ParseNodeKind::Import:
          case ParseNodeKind::ExportFrom:
          case ParseNodeKind::ExportDefault:
            MOZ_ASSERT(cur->isArity(PN_BINARY));
            // The left halves of these nodes don't contain any unconstrained
            // expressions, but it's very hard to assert this to safely rely on
            // it.  So recur anyway.
            if (!resolve(cur->pn_left, prefix))
                return false;
            MOZ_ASSERT_IF(!cur->isKind(ParseNodeKind::ExportDefault),
                          cur->pn_right->isKind(ParseNodeKind::String));
            break;

          // Ternary nodes with three expression children.
          case ParseNodeKind::Conditional:
            MOZ_ASSERT(cur->isArity(PN_TERNARY));
            if (!resolve(cur->pn_kid1, prefix))
                return false;
            if (!resolve(cur->pn_kid2, prefix))
                return false;
            if (!resolve(cur->pn_kid3, prefix))
                return false;
            break;

          // The first part of a for-in/of is the declaration in the loop (or
          // null if no declaration).  The latter two parts are the location
          // assigned each loop and the value being looped over; obviously,
          // either might contain functions to name.  Declarations may (through
          // computed property names, and possibly through [deprecated!]
          // initializers) also contain functions to name.
          case ParseNodeKind::ForIn:
          case ParseNodeKind::ForOf:
            MOZ_ASSERT(cur->isArity(PN_TERNARY));
            if (ParseNode* decl = cur->pn_kid1) {
                if (!resolve(decl, prefix))
                    return false;
            }
            if (!resolve(cur->pn_kid2, prefix))
                return false;
            if (!resolve(cur->pn_kid3, prefix))
                return false;
            break;

          // Every part of a for(;;) head may contain a function needing name
          // resolution.
          case ParseNodeKind::ForHead:
            MOZ_ASSERT(cur->isArity(PN_TERNARY));
            if (ParseNode* init = cur->pn_kid1) {
                if (!resolve(init, prefix))
                    return false;
            }
            if (ParseNode* cond = cur->pn_kid2) {
                if (!resolve(cond, prefix))
                    return false;
            }
            if (ParseNode* step = cur->pn_kid3) {
                if (!resolve(step, prefix))
                    return false;
            }
            break;

          // The first child of a class is a pair of names referring to it,
          // inside and outside the class.  The second is the class's heritage,
          // if any.  The third is the class body.
          case ParseNodeKind::Class:
            MOZ_ASSERT(cur->isArity(PN_TERNARY));
            MOZ_ASSERT_IF(cur->pn_kid1, cur->pn_kid1->isKind(ParseNodeKind::ClassNames));
            MOZ_ASSERT_IF(cur->pn_kid1, cur->pn_kid1->isArity(PN_BINARY));
            MOZ_ASSERT_IF(cur->pn_kid1 && cur->pn_kid1->pn_left,
                          cur->pn_kid1->pn_left->isKind(ParseNodeKind::Name));
            MOZ_ASSERT_IF(cur->pn_kid1 && cur->pn_kid1->pn_left,
                          !cur->pn_kid1->pn_left->expr());
            MOZ_ASSERT_IF(cur->pn_kid1, cur->pn_kid1->pn_right->isKind(ParseNodeKind::Name));
            MOZ_ASSERT_IF(cur->pn_kid1, !cur->pn_kid1->pn_right->expr());
            if (cur->pn_kid2) {
                if (!resolve(cur->pn_kid2, prefix))
                    return false;
            }
            if (!resolve(cur->pn_kid3, prefix))
                return false;
            break;

          // The condition and consequent are non-optional, but the alternative
          // might be omitted.
          case ParseNodeKind::If:
            MOZ_ASSERT(cur->isArity(PN_TERNARY));
            if (!resolve(cur->pn_kid1, prefix))
                return false;
            if (!resolve(cur->pn_kid2, prefix))
                return false;
            if (cur->pn_kid3) {
                if (!resolve(cur->pn_kid3, prefix))
                    return false;
            }
            break;

          // The statements in the try-block are mandatory.  The catch-blocks
          // and finally block are optional (but at least one or the other must
          // be present).
          case ParseNodeKind::Try:
            MOZ_ASSERT(cur->isArity(PN_TERNARY));
            if (!resolve(cur->pn_kid1, prefix))
                return false;
            MOZ_ASSERT(cur->pn_kid2 || cur->pn_kid3);
            if (ParseNode* catchScope = cur->pn_kid2) {
                MOZ_ASSERT(catchScope->isKind(ParseNodeKind::LexicalScope));
                MOZ_ASSERT(catchScope->scopeBody()->isKind(ParseNodeKind::Catch));
                MOZ_ASSERT(catchScope->scopeBody()->isArity(PN_BINARY));
                if (!resolve(catchScope->scopeBody(), prefix))
                    return false;
            }
            if (ParseNode* finallyBlock = cur->pn_kid3) {
                if (!resolve(finallyBlock, prefix))
                    return false;
            }
            break;

          // The first child, the catch-pattern, may contain functions via
          // computed property names.  The optional catch-conditions may
          // contain any expression.  The catch statements, of course, may
          // contain arbitrary expressions.
          case ParseNodeKind::Catch:
            MOZ_ASSERT(cur->isArity(PN_BINARY));
            if (cur->pn_left) {
              if (!resolve(cur->pn_left, prefix))
                  return false;
            }
            if (!resolve(cur->pn_right, prefix))
                return false;
            break;

          // Nodes with arbitrary-expression children.
          case ParseNodeKind::Or:
          case ParseNodeKind::And:
          case ParseNodeKind::BitOr:
          case ParseNodeKind::BitXor:
          case ParseNodeKind::BitAnd:
          case ParseNodeKind::StrictEq:
          case ParseNodeKind::Eq:
          case ParseNodeKind::StrictNe:
          case ParseNodeKind::Ne:
          case ParseNodeKind::Lt:
          case ParseNodeKind::Le:
          case ParseNodeKind::Gt:
          case ParseNodeKind::Ge:
          case ParseNodeKind::InstanceOf:
          case ParseNodeKind::In:
          case ParseNodeKind::Lsh:
          case ParseNodeKind::Rsh:
          case ParseNodeKind::Ursh:
          case ParseNodeKind::Add:
          case ParseNodeKind::Sub:
          case ParseNodeKind::Star:
          case ParseNodeKind::Div:
          case ParseNodeKind::Mod:
          case ParseNodeKind::Pow:
          case ParseNodeKind::Pipeline:
          case ParseNodeKind::Comma:
          case ParseNodeKind::New:
          case ParseNodeKind::Call:
          case ParseNodeKind::SuperCall:
          case ParseNodeKind::Array:
          case ParseNodeKind::StatementList:
          case ParseNodeKind::ParamsBody:
          // Initializers for individual variables, and computed property names
          // within destructuring patterns, may contain unnamed functions.
          case ParseNodeKind::Var:
          case ParseNodeKind::Const:
          case ParseNodeKind::Let:
            MOZ_ASSERT(cur->isArity(PN_LIST));
            for (ParseNode* element = cur->pn_head; element; element = element->pn_next) {
                if (!resolve(element, prefix))
                    return false;
            }
            break;

          case ParseNodeKind::Object:
          case ParseNodeKind::ClassMethodList:
            MOZ_ASSERT(cur->isArity(PN_LIST));
            for (ParseNode* element = cur->pn_head; element; element = element->pn_next) {
                if (!resolve(element, prefix))
                    return false;
            }
            break;

          // A template string list's contents alternate raw template string
          // contents with expressions interpolated into the overall literal.
          case ParseNodeKind::TemplateStringList:
            MOZ_ASSERT(cur->isArity(PN_LIST));
            if (!resolveTemplateLiteral(cur, prefix))
                return false;
            break;

          case ParseNodeKind::TaggedTemplate:
            MOZ_ASSERT(cur->isArity(PN_LIST));
            if (!resolveTaggedTemplate(cur, prefix))
                return false;
            break;

          // Import/export spec lists contain import/export specs containing
          // only pairs of names. Alternatively, an export spec lists may
          // contain a single export batch specifier.
          case ParseNodeKind::ExportSpecList:
          case ParseNodeKind::ImportSpecList: {
            MOZ_ASSERT(cur->isArity(PN_LIST));
#ifdef DEBUG
            bool isImport = cur->isKind(ParseNodeKind::ImportSpecList);
            ParseNode* item = cur->pn_head;
            if (!isImport && item && item->isKind(ParseNodeKind::ExportBatchSpec)) {
                MOZ_ASSERT(item->isArity(PN_NULLARY));
                break;
            }
            for (; item; item = item->pn_next) {
                MOZ_ASSERT(item->isKind(isImport
                                        ? ParseNodeKind::ImportSpec
                                        : ParseNodeKind::ExportSpec));
                MOZ_ASSERT(item->isArity(PN_BINARY));
                MOZ_ASSERT(item->pn_left->isKind(ParseNodeKind::Name));
                MOZ_ASSERT(!item->pn_left->expr());
                MOZ_ASSERT(item->pn_right->isKind(ParseNodeKind::Name));
                MOZ_ASSERT(!item->pn_right->expr());
            }
#endif
            break;
          }

          case ParseNodeKind::Dot:
            MOZ_ASSERT(cur->isArity(PN_NAME));

            // Super prop nodes do not have a meaningful LHS
            if (cur->as<PropertyAccess>().isSuper())
                break;
            if (!resolve(cur->expr(), prefix))
                return false;
            break;

          case ParseNodeKind::Label:
            MOZ_ASSERT(cur->isArity(PN_NAME));
            if (!resolve(cur->expr(), prefix))
                return false;
            break;

          case ParseNodeKind::Name:
            MOZ_ASSERT(cur->isArity(PN_NAME));
            if (!resolve(cur->expr(), prefix))
                return false;
            break;

          case ParseNodeKind::LexicalScope:
            MOZ_ASSERT(cur->isArity(PN_SCOPE));
            if (!resolve(cur->scopeBody(), prefix))
                return false;
            break;

          case ParseNodeKind::Function:
          case ParseNodeKind::Module:
            MOZ_ASSERT(cur->isArity(PN_CODE));
            if (!resolve(cur->pn_body, prefix))
                return false;
            break;

          // Kinds that should be handled by parent node resolution.

          case ParseNodeKind::ImportSpec: // by ParseNodeKind::ImportSpecList
          case ParseNodeKind::ExportSpec: // by ParseNodeKind::ExportSpecList
          case ParseNodeKind::CallSiteObj: // by ParseNodeKind::TaggedTemplate
          case ParseNodeKind::ClassNames:  // by ParseNodeKind::Class
            MOZ_CRASH("should have been handled by a parent node");

          case ParseNodeKind::Limit: // invalid sentinel value
            MOZ_CRASH("invalid node kind");
        }

        nparents--;
        MOZ_ASSERT(initialParents == nparents, "nparents imbalance detected");

        // It would be nice to common up the repeated |parents[initialParents]|
        // in a single variable, but the #if condition required to prevent an
        // unused-variable warning across three separate conditionally-expanded
        // macros would be super-ugly.  :-(
        MOZ_ASSERT(parents[initialParents] == cur,
                   "pushed child shouldn't change underneath us");

        JS_POISON(&parents[initialParents], 0xFF, sizeof(parents[initialParents]));
        MOZ_MAKE_MEM_UNDEFINED(&parents[initialParents], sizeof(parents[initialParents]));

        return true;
    }
};

} /* anonymous namespace */

bool
frontend::NameFunctions(JSContext* cx, ParseNode* pn)
{
    AutoTraceLog traceLog(TraceLoggerForCurrentThread(cx), TraceLogger_BytecodeNameFunctions);
    NameResolver nr(cx);
    return nr.resolve(pn);
}
