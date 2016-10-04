/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/NameFunctions.h"

#include "jsfun.h"
#include "jsprf.h"

#include "frontend/BytecodeCompiler.h"
#include "frontend/ParseNode.h"
#include "frontend/SharedContext.h"
#include "vm/StringBuffer.h"

using namespace js;
using namespace js::frontend;

namespace {

class NameResolver
{
    static const size_t MaxParents = 100;

    ExclusiveContext* cx;
    size_t nparents;                /* number of parents in the parents array */
    ParseNode* parents[MaxParents]; /* history of ParseNodes we've been looking at */
    StringBuffer* buf;              /* when resolving, buffer to append to */

    /* Test whether a ParseNode represents a function invocation */
    bool call(ParseNode* pn) {
        return pn && pn->isKind(PNK_CALL);
    }

    /*
     * Append a reference to a property named |name| to |buf|. If |name| is
     * a proper identifier name, then we append '.name'; otherwise, we
     * append '["name"]'.
     *
     * Note that we need the IsIdentifier check for atoms from both
     * PNK_NAME nodes and PNK_STRING nodes: given code like a["b c"], the
     * front end will produce a PNK_DOT with a PNK_NAME child whose name
     * contains spaces.
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
        int digits = JS_snprintf(number, sizeof(number), "%g", n);
        return buf->append(number, digits);
    }

    /* Append "[<n>]" to buf, referencing a property named by a numeric literal. */
    bool appendNumericPropertyReference(double n) {
        return buf->append("[") && appendNumber(n) && buf->append(']');
    }

    /*
     * Walk over the given ParseNode, converting it to a stringified name that
     * respresents where the function is being assigned to.
     */
    bool nameExpression(ParseNode* n) {
        switch (n->getKind()) {
          case PNK_DOT:
            return nameExpression(n->expr()) && appendPropertyReference(n->pn_atom);

          case PNK_NAME:
            return buf->append(n->pn_atom);

          case PNK_THIS:
            return buf->append("this");

          case PNK_ELEM:
            return nameExpression(n->pn_left) &&
                   buf->append('[') &&
                   nameExpression(n->pn_right) &&
                   buf->append(']');

          case PNK_NUMBER:
            return appendNumber(n->pn_dval);

          default:
            /*
             * Technically this isn't an "abort" situation, we're just confused
             * on what to call this function, but failures in naming aren't
             * treated as fatal.
             */
            return false;
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
              case PNK_NAME:     return cur;  /* found the initialized declaration */
              case PNK_THIS:     return cur;  /* Setting a property of 'this'. */
              case PNK_FUNCTION: return nullptr; /* won't find an assignment or declaration */

              case PNK_RETURN:
                /*
                 * Normally the relevant parent of a node is its direct parent, but
                 * sometimes with code like:
                 *
                 *    var foo = (function() { return function() {}; })();
                 *
                 * the outer function is just a helper to create a scope for the
                 * returned function. Hence the name of the returned function should
                 * actually be 'foo'.  This loop sees if the current node is a
                 * PNK_RETURN, and if there is a direct function call we skip to
                 * that.
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

              case PNK_COLON:
              case PNK_SHORTHAND:
                /*
                 * Record the PNK_COLON/SHORTHAND but skip the PNK_OBJECT so we're not
                 * flagged as a contributor.
                 */
                pos--;
                /* fallthrough */

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
     * assign to the function's displayAtom field
     */
    bool resolveFun(ParseNode* pn, HandleAtom prefix, MutableHandleAtom retAtom) {
        MOZ_ASSERT(pn != nullptr);
        MOZ_ASSERT(pn->isKind(PNK_FUNCTION));
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
            if (!nameExpression(assignment))
                return true;
        }

        /*
         * Other than the actual assignment, other relevant nodes to naming are
         * those in object initializers and then particular nodes marking a
         * contribution.
         */
        for (int pos = size - 1; pos >= 0; pos--) {
            ParseNode* node = toName[pos];

            if (node->isKind(PNK_COLON) || node->isKind(PNK_SHORTHAND)) {
                ParseNode* left = node->pn_left;
                if (left->isKind(PNK_OBJECT_PROPERTY_NAME) || left->isKind(PNK_STRING)) {
                    if (!appendPropertyReference(left->pn_atom))
                        return false;
                } else if (left->isKind(PNK_NUMBER)) {
                    if (!appendNumericPropertyReference(left->pn_dval))
                        return false;
                } else {
                    MOZ_ASSERT(left->isKind(PNK_COMPUTED_NAME));
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
        MOZ_ASSERT(node->isKind(PNK_TEMPLATE_STRING_LIST));
        ParseNode* element = node->pn_head;
        while (true) {
            MOZ_ASSERT(element->isKind(PNK_TEMPLATE_STRING));

            element = element->pn_next;
            if (!element)
                return true;

            if (!resolve(element, prefix))
                return false;

            element = element->pn_next;
        }
    }

    bool resolveTaggedTemplate(ParseNode* node, HandleAtom prefix) {
        MOZ_ASSERT(node->isKind(PNK_TAGGED_TEMPLATE));

        ParseNode* element = node->pn_head;

        // The list head is a leading expression, e.g. |tag| in |tag`foo`|,
        // that might contain functions.
        if (!resolve(element, prefix))
            return false;

        // Next is the callsite object node.  This node only contains
        // internal strings and an array -- no user-controlled expressions.
        element = element->pn_next;
#ifdef DEBUG
        {
            MOZ_ASSERT(element->isKind(PNK_CALLSITEOBJ));
            ParseNode* array = element->pn_head;
            MOZ_ASSERT(array->isKind(PNK_ARRAY));
            for (ParseNode* kid = array->pn_head; kid; kid = kid->pn_next)
                MOZ_ASSERT(kid->isKind(PNK_TEMPLATE_STRING));
            for (ParseNode* next = array->pn_next; next; next = next->pn_next)
                MOZ_ASSERT(next->isKind(PNK_TEMPLATE_STRING));
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
    explicit NameResolver(ExclusiveContext* cx) : cx(cx), nparents(0), buf(nullptr) {}

    /*
     * Resolve all names for anonymous functions recursively within the
     * ParseNode instance given. The prefix is for each subsequent name, and
     * should initially be nullptr.
     */
    bool resolve(ParseNode* cur, HandleAtom prefixArg = nullptr) {
        RootedAtom prefix(cx, prefixArg);
        if (cur == nullptr)
            return true;

        MOZ_ASSERT((cur->isKind(PNK_FUNCTION) || cur->isKind(PNK_MODULE)) == cur->isArity(PN_CODE));
        if (cur->isKind(PNK_FUNCTION)) {
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
        parents[nparents++] = cur;

        switch (cur->getKind()) {
          // Nodes with no children that might require name resolution need no
          // further work.
          case PNK_NOP:
          case PNK_STRING:
          case PNK_TEMPLATE_STRING:
          case PNK_REGEXP:
          case PNK_TRUE:
          case PNK_FALSE:
          case PNK_NULL:
          case PNK_ELISION:
          case PNK_GENERATOR:
          case PNK_NUMBER:
          case PNK_BREAK:
          case PNK_CONTINUE:
          case PNK_DEBUGGER:
          case PNK_EXPORT_BATCH_SPEC:
          case PNK_OBJECT_PROPERTY_NAME:
          case PNK_POSHOLDER:
            MOZ_ASSERT(cur->isArity(PN_NULLARY));
            break;

          case PNK_TYPEOFNAME:
          case PNK_SUPERBASE:
            MOZ_ASSERT(cur->isArity(PN_UNARY));
            MOZ_ASSERT(cur->pn_kid->isKind(PNK_NAME));
            MOZ_ASSERT(!cur->pn_kid->maybeExpr());
            break;

          case PNK_NEWTARGET:
            MOZ_ASSERT(cur->isArity(PN_BINARY));
            MOZ_ASSERT(cur->pn_left->isKind(PNK_POSHOLDER));
            MOZ_ASSERT(cur->pn_right->isKind(PNK_POSHOLDER));
            break;

          // Nodes with a single non-null child requiring name resolution.
          case PNK_TYPEOFEXPR:
          case PNK_VOID:
          case PNK_NOT:
          case PNK_BITNOT:
          case PNK_THROW:
          case PNK_DELETENAME:
          case PNK_DELETEPROP:
          case PNK_DELETEELEM:
          case PNK_DELETEEXPR:
          case PNK_NEG:
          case PNK_POS:
          case PNK_PREINCREMENT:
          case PNK_POSTINCREMENT:
          case PNK_PREDECREMENT:
          case PNK_POSTDECREMENT:
          case PNK_COMPUTED_NAME:
          case PNK_ARRAYPUSH:
          case PNK_SPREAD:
          case PNK_MUTATEPROTO:
          case PNK_EXPORT:
            MOZ_ASSERT(cur->isArity(PN_UNARY));
            if (!resolve(cur->pn_kid, prefix))
                return false;
            break;

          // Nodes with a single nullable child.
          case PNK_SEMI:
          case PNK_THIS:
            MOZ_ASSERT(cur->isArity(PN_UNARY));
            if (ParseNode* expr = cur->pn_kid) {
                if (!resolve(expr, prefix))
                    return false;
            }
            break;

          // Binary nodes with two non-null children.
          case PNK_ASSIGN:
          case PNK_ADDASSIGN:
          case PNK_SUBASSIGN:
          case PNK_BITORASSIGN:
          case PNK_BITXORASSIGN:
          case PNK_BITANDASSIGN:
          case PNK_LSHASSIGN:
          case PNK_RSHASSIGN:
          case PNK_URSHASSIGN:
          case PNK_MULASSIGN:
          case PNK_DIVASSIGN:
          case PNK_MODASSIGN:
          case PNK_POWASSIGN:
          case PNK_COLON:
          case PNK_SHORTHAND:
          case PNK_DOWHILE:
          case PNK_WHILE:
          case PNK_SWITCH:
          case PNK_LETBLOCK:
          case PNK_FOR:
          case PNK_COMPREHENSIONFOR:
          case PNK_CLASSMETHOD:
          case PNK_SETTHIS:
            MOZ_ASSERT(cur->isArity(PN_BINARY));
            if (!resolve(cur->pn_left, prefix))
                return false;
            if (!resolve(cur->pn_right, prefix))
                return false;
            break;

          case PNK_ELEM:
            MOZ_ASSERT(cur->isArity(PN_BINARY));
            if (!cur->as<PropertyByValue>().isSuper() && !resolve(cur->pn_left, prefix))
                return false;
            if (!resolve(cur->pn_right, prefix))
                return false;
            break;

          case PNK_WITH:
            MOZ_ASSERT(cur->isArity(PN_BINARY_OBJ));
            if (!resolve(cur->pn_left, prefix))
                return false;
            if (!resolve(cur->pn_right, prefix))
                return false;
            break;

          case PNK_CASE:
            MOZ_ASSERT(cur->isArity(PN_BINARY));
            if (ParseNode* caseExpr = cur->pn_left) {
                if (!resolve(caseExpr, prefix))
                    return false;
            }
            if (!resolve(cur->pn_right, prefix))
                return false;
            break;

          case PNK_YIELD_STAR:
            MOZ_ASSERT(cur->isArity(PN_BINARY));
            MOZ_ASSERT(cur->pn_right->isKind(PNK_NAME));
            MOZ_ASSERT(!cur->pn_right->isAssigned());
            if (!resolve(cur->pn_left, prefix))
                return false;
            break;

          case PNK_YIELD:
            MOZ_ASSERT(cur->isArity(PN_BINARY));
            if (cur->pn_left) {
                if (!resolve(cur->pn_left, prefix))
                    return false;
            }
            MOZ_ASSERT((cur->pn_right->isKind(PNK_NAME) && !cur->pn_right->isAssigned()) ||
                       (cur->pn_right->isKind(PNK_ASSIGN) &&
                        cur->pn_right->pn_left->isKind(PNK_NAME) &&
                        cur->pn_right->pn_right->isKind(PNK_GENERATOR)));
            break;

          case PNK_RETURN:
            MOZ_ASSERT(cur->isArity(PN_UNARY));
            if (ParseNode* returnValue = cur->pn_kid) {
                if (!resolve(returnValue, prefix))
                    return false;
            }
            break;

          case PNK_IMPORT:
          case PNK_EXPORT_FROM:
          case PNK_EXPORT_DEFAULT:
            MOZ_ASSERT(cur->isArity(PN_BINARY));
            // The left halves of these nodes don't contain any unconstrained
            // expressions, but it's very hard to assert this to safely rely on
            // it.  So recur anyway.
            if (!resolve(cur->pn_left, prefix))
                return false;
            MOZ_ASSERT_IF(!cur->isKind(PNK_EXPORT_DEFAULT),
                          cur->pn_right->isKind(PNK_STRING));
            break;

          // Ternary nodes with three expression children.
          case PNK_CONDITIONAL:
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
          case PNK_FORIN:
          case PNK_FOROF:
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
          case PNK_FORHEAD:
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
          case PNK_CLASS:
            MOZ_ASSERT(cur->isArity(PN_TERNARY));
            MOZ_ASSERT_IF(cur->pn_kid1, cur->pn_kid1->isKind(PNK_CLASSNAMES));
            MOZ_ASSERT_IF(cur->pn_kid1, cur->pn_kid1->isArity(PN_BINARY));
            MOZ_ASSERT_IF(cur->pn_kid1 && cur->pn_kid1->pn_left,
                          cur->pn_kid1->pn_left->isKind(PNK_NAME));
            MOZ_ASSERT_IF(cur->pn_kid1 && cur->pn_kid1->pn_left,
                          !cur->pn_kid1->pn_left->maybeExpr());
            MOZ_ASSERT_IF(cur->pn_kid1, cur->pn_kid1->pn_right->isKind(PNK_NAME));
            MOZ_ASSERT_IF(cur->pn_kid1, !cur->pn_kid1->pn_right->maybeExpr());
            if (cur->pn_kid2) {
                if (!resolve(cur->pn_kid2, prefix))
                    return false;
            }
            if (!resolve(cur->pn_kid3, prefix))
                return false;
            break;

          // The condition and consequent are non-optional, but the alternative
          // might be omitted.
          case PNK_IF:
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
          case PNK_TRY:
            MOZ_ASSERT(cur->isArity(PN_TERNARY));
            if (!resolve(cur->pn_kid1, prefix))
                return false;
            MOZ_ASSERT(cur->pn_kid2 || cur->pn_kid3);
            if (ParseNode* catchList = cur->pn_kid2) {
                MOZ_ASSERT(catchList->isKind(PNK_CATCHLIST));
                if (!resolve(catchList, prefix))
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
          case PNK_CATCH:
            MOZ_ASSERT(cur->isArity(PN_TERNARY));
            if (!resolve(cur->pn_kid1, prefix))
                return false;
            if (cur->pn_kid2) {
                if (!resolve(cur->pn_kid2, prefix))
                    return false;
            }
            if (!resolve(cur->pn_kid3, prefix))
                return false;
            break;

          // Nodes with arbitrary-expression children.
          case PNK_OR:
          case PNK_AND:
          case PNK_BITOR:
          case PNK_BITXOR:
          case PNK_BITAND:
          case PNK_STRICTEQ:
          case PNK_EQ:
          case PNK_STRICTNE:
          case PNK_NE:
          case PNK_LT:
          case PNK_LE:
          case PNK_GT:
          case PNK_GE:
          case PNK_INSTANCEOF:
          case PNK_IN:
          case PNK_LSH:
          case PNK_RSH:
          case PNK_URSH:
          case PNK_ADD:
          case PNK_SUB:
          case PNK_STAR:
          case PNK_DIV:
          case PNK_MOD:
          case PNK_POW:
          case PNK_COMMA:
          case PNK_NEW:
          case PNK_CALL:
          case PNK_SUPERCALL:
          case PNK_GENEXP:
          case PNK_ARRAY:
          case PNK_STATEMENTLIST:
          case PNK_ARGSBODY:
          // Initializers for individual variables, and computed property names
          // within destructuring patterns, may contain unnamed functions.
          case PNK_VAR:
          case PNK_CONST:
          case PNK_LET:
            MOZ_ASSERT(cur->isArity(PN_LIST));
            for (ParseNode* element = cur->pn_head; element; element = element->pn_next) {
                if (!resolve(element, prefix))
                    return false;
            }
            break;

          // Array comprehension nodes are lists with a single child:
          // PNK_COMPREHENSIONFOR for comprehensions, PNK_LEXICALSCOPE for
          // legacy comprehensions.  Probably this should be a non-list
          // eventually.
          case PNK_ARRAYCOMP:
            MOZ_ASSERT(cur->isArity(PN_LIST));
            MOZ_ASSERT(cur->pn_count == 1);
            MOZ_ASSERT(cur->pn_head->isKind(PNK_LEXICALSCOPE) ||
                       cur->pn_head->isKind(PNK_COMPREHENSIONFOR));
            if (!resolve(cur->pn_head, prefix))
                return false;
            break;

          case PNK_OBJECT:
          case PNK_CLASSMETHODLIST:
            MOZ_ASSERT(cur->isArity(PN_LIST));
            for (ParseNode* element = cur->pn_head; element; element = element->pn_next) {
                if (!resolve(element, prefix))
                    return false;
            }
            break;

          // A template string list's contents alternate raw template string
          // contents with expressions interpolated into the overall literal.
          case PNK_TEMPLATE_STRING_LIST:
            MOZ_ASSERT(cur->isArity(PN_LIST));
            if (!resolveTemplateLiteral(cur, prefix))
                return false;
            break;

          case PNK_TAGGED_TEMPLATE:
            MOZ_ASSERT(cur->isArity(PN_LIST));
            if (!resolveTaggedTemplate(cur, prefix))
                return false;
            break;

          // Import/export spec lists contain import/export specs containing
          // only pairs of names. Alternatively, an export spec lists may
          // contain a single export batch specifier.
          case PNK_IMPORT_SPEC_LIST: {
          case PNK_EXPORT_SPEC_LIST:
            MOZ_ASSERT(cur->isArity(PN_LIST));
#ifdef DEBUG
            bool isImport = cur->isKind(PNK_IMPORT_SPEC_LIST);
            ParseNode* item = cur->pn_head;
            if (!isImport && item && item->isKind(PNK_EXPORT_BATCH_SPEC)) {
                MOZ_ASSERT(item->isArity(PN_NULLARY));
                break;
            }
            for (; item; item = item->pn_next) {
                MOZ_ASSERT(item->isKind(isImport ? PNK_IMPORT_SPEC : PNK_EXPORT_SPEC));
                MOZ_ASSERT(item->isArity(PN_BINARY));
                MOZ_ASSERT(item->pn_left->isKind(PNK_NAME));
                MOZ_ASSERT(!item->pn_left->maybeExpr());
                MOZ_ASSERT(item->pn_right->isKind(PNK_NAME));
                MOZ_ASSERT(!item->pn_right->maybeExpr());
            }
#endif
            break;
          }

          case PNK_CATCHLIST: {
            MOZ_ASSERT(cur->isArity(PN_LIST));
            for (ParseNode* catchNode = cur->pn_head; catchNode; catchNode = catchNode->pn_next) {
                MOZ_ASSERT(catchNode->isKind(PNK_LEXICALSCOPE));
                MOZ_ASSERT(catchNode->expr()->isKind(PNK_CATCH));
                MOZ_ASSERT(catchNode->expr()->isArity(PN_TERNARY));
                if (!resolve(catchNode->expr(), prefix))
                    return false;
            }
            break;
          }

          case PNK_DOT:
            MOZ_ASSERT(cur->isArity(PN_NAME));

            // Super prop nodes do not have a meaningful LHS
            if (cur->as<PropertyAccess>().isSuper())
                break;
            if (!resolve(cur->expr(), prefix))
                return false;
            break;

          case PNK_LABEL:
            MOZ_ASSERT(cur->isArity(PN_NAME));
            if (!resolve(cur->expr(), prefix))
                return false;
            break;

          case PNK_LEXICALSCOPE:
          case PNK_NAME:
            MOZ_ASSERT(cur->isArity(PN_NAME));
            if (!resolve(cur->maybeExpr(), prefix))
                return false;
            break;

          case PNK_FUNCTION:
          case PNK_MODULE:
            MOZ_ASSERT(cur->isArity(PN_CODE));
            if (!resolve(cur->pn_body, prefix))
                return false;
            break;

          // Kinds that should be handled by parent node resolution.

          case PNK_IMPORT_SPEC: // by PNK_IMPORT_SPEC_LIST
          case PNK_EXPORT_SPEC: // by PNK_EXPORT_SPEC_LIST
          case PNK_CALLSITEOBJ: // by PNK_TAGGED_TEMPLATE
          case PNK_CLASSNAMES:  // by PNK_CLASS
            MOZ_CRASH("should have been handled by a parent node");

          case PNK_LIMIT: // invalid sentinel value
            MOZ_CRASH("invalid node kind");
        }

        nparents--;
        return true;
    }
};

} /* anonymous namespace */

bool
frontend::NameFunctions(ExclusiveContext* cx, ParseNode* pn)
{
    NameResolver nr(cx);
    return nr.resolve(pn);
}
