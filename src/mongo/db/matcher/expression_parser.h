// expression_parser.h

/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/stdx/functional.h"

namespace mongo {

class OperationContext;

typedef StatusWith<std::unique_ptr<MatchExpression>> StatusWithMatchExpression;

class MatchExpressionParser {
public:
    /**
     * In general, expression parsing and matching should not require context, but the $where
     * clause is an exception in that it needs to read the sys.js collection.
     *
     * The default behaviour is to return an error status that $where context is not present.
     *
     * Do not use this class to pass-in generic context as it should only be used for $where.
     */
    class ExtensionsCallback {
    public:
        virtual StatusWithMatchExpression parseWhere(const BSONElement& where) const;

        virtual ~ExtensionsCallback() {}
    };

    /**
     * caller has to maintain ownership obj
     * the tree has views (BSONElement) into obj
     */
    static StatusWithMatchExpression parse(
        const BSONObj& obj, const ExtensionsCallback& extensionsCallback = ExtensionsCallback()) {
        // The 0 initializes the match expression tree depth.
        return MatchExpressionParser(&extensionsCallback)._parse(obj, 0);
    }

private:
    explicit MatchExpressionParser(const ExtensionsCallback* extensionsCallback)
        : _extensionsCallback(extensionsCallback) {}

    /**
     * 5 = false
     * { a : 5 } = false
     * { $lt : 5 } = true
     * { $ref: "s", $id: "x" } = false
     * { $ref: "s", $id: "x", $db: "mydb" } = false
     * { $ref : "s" } = false (if incomplete DBRef is allowed)
     * { $id : "x" } = false (if incomplete DBRef is allowed)
     * { $db : "mydb" } = false (if incomplete DBRef is allowed)
     */
    bool _isExpressionDocument(const BSONElement& e, bool allowIncompleteDBRef);

    /**
     * { $ref: "s", $id: "x" } = true
     * { $ref : "s" } = true (if incomplete DBRef is allowed)
     * { $id : "x" } = true (if incomplete DBRef is allowed)
     * { $db : "x" } = true (if incomplete DBRef is allowed)
     */
    bool _isDBRefDocument(const BSONObj& obj, bool allowIncompleteDBRef);

    /**
     * Parse 'obj' and return either a MatchExpression or an error.
     *
     * 'level' tracks the current depth of the tree across recursive calls to this
     * function. Used in order to apply special logic at the top-level and to return an
     * error if the tree exceeds the maximum allowed depth.
     */
    StatusWithMatchExpression _parse(const BSONObj& obj, int level);

    /**
     * parses a field in a sub expression
     * if the query is { x : { $gt : 5, $lt : 8 } }
     * e is { $gt : 5, $lt : 8 }
     */
    Status _parseSub(const char* name, const BSONObj& obj, AndMatchExpression* root, int level);

    /**
     * parses a single field in a sub expression
     * if the query is { x : { $gt : 5, $lt : 8 } }
     * e is $gt : 5
     */
    StatusWithMatchExpression _parseSubField(const BSONObj& context,
                                             const AndMatchExpression* andSoFar,
                                             const char* name,
                                             const BSONElement& e,
                                             int level);

    StatusWithMatchExpression _parseComparison(const char* name,
                                               ComparisonMatchExpression* cmp,
                                               const BSONElement& e);

    StatusWithMatchExpression _parseMOD(const char* name, const BSONElement& e);

    StatusWithMatchExpression _parseRegexElement(const char* name, const BSONElement& e);

    StatusWithMatchExpression _parseRegexDocument(const char* name, const BSONObj& doc);


    Status _parseArrayFilterEntries(ArrayFilterEntries* entries, const BSONObj& theArray);

    StatusWithMatchExpression _parseType(const char* name, const BSONElement& elt);

    // arrays

    StatusWithMatchExpression _parseElemMatch(const char* name, const BSONElement& e, int level);

    StatusWithMatchExpression _parseAll(const char* name, const BSONElement& e, int level);

    // tree

    Status _parseTreeList(const BSONObj& arr, ListOfMatchExpression* out, int level);

    StatusWithMatchExpression _parseNot(const char* name, const BSONElement& e, int level);

    /**
     * Parses 'e' into a BitTestMatchExpression.
     */
    template <class T>
    StatusWithMatchExpression _parseBitTest(const char* name, const BSONElement& e);

    /**
     * Converts 'theArray', a BSONArray of integers, into a std::vector of integers.
     */
    StatusWith<std::vector<uint32_t>> _parseBitPositionsArray(const BSONObj& theArray);

    // The maximum allowed depth of a query tree. Just to guard against stack overflow.
    static const int kMaximumTreeDepth;

    // Performs parsing for the $where clause. We do not own this pointer - it has to live
    // as long as the parser is active.
    const ExtensionsCallback* _extensionsCallback;
};

/**
 * This implementation is used for the server-side code.
 */
class ExtensionsCallbackReal : public MatchExpressionParser::ExtensionsCallback {
public:
    /**
     * The OperationContext passed here is not owned, but just referenced. It gets assigned to
     * any $where parsers, which this callback generates. Therefore, the op context must only
     * be destroyed after these parsers and their clones (shallowClone) have been destroyed.
     */
    ExtensionsCallbackReal(OperationContext* txn, StringData dbName);

    virtual StatusWithMatchExpression parseWhere(const BSONElement& where) const;

private:
    // Not owned here
    OperationContext* const _txn;
    const StringData _dbName;
};

/**
 * This is just a pass-through implementation, used by sharding only.
 */
class ExtensionsCallbackNoop : public MatchExpressionParser::ExtensionsCallback {
public:
    ExtensionsCallbackNoop();

    virtual StatusWithMatchExpression parseWhere(const BSONElement& where) const;
};


typedef stdx::function<StatusWithMatchExpression(
    const char* name, int type, const BSONObj& section)> MatchExpressionParserGeoCallback;
extern MatchExpressionParserGeoCallback expressionParserGeoCallback;

typedef stdx::function<StatusWithMatchExpression(const BSONObj& queryObj)>
    MatchExpressionParserTextCallback;
extern MatchExpressionParserTextCallback expressionParserTextCallback;
}
