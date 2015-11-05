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
#include "mongo/db/matcher/expression_text_base.h"
#include "mongo/db/matcher/expression_where_base.h"
#include "mongo/stdx/functional.h"

namespace mongo {

class OperationContext;

typedef StatusWith<std::unique_ptr<MatchExpression>> StatusWithMatchExpression;

class MatchExpressionParser {
public:
    /**
     * Certain match clauses (the "extension" clauses, namely $text and $where) require context in
     * order to perform parsing. This context is captured inside of an ExtensionsCallback object.
     *
     * The default implementations of parseText() and parseWhere() simply return an error Status.
     * Instead of constructing an ExtensionsCallback object directly, an instance of one of the
     * derived classes (ExtensionsCallbackReal or ExtensionsCallbackNoop) should generally be used
     * instead.
     */
    class ExtensionsCallback {
    public:
        virtual StatusWithMatchExpression parseText(BSONElement text) const;

        virtual StatusWithMatchExpression parseWhere(BSONElement where) const;

        virtual ~ExtensionsCallback() {}

    protected:
        /**
         * Helper method which extracts parameters from the given $text element.
         */
        static StatusWith<TextMatchExpressionBase::TextParams> extractTextMatchExpressionParams(
            BSONElement text);

        /**
         * Helper method which extracts parameters from the given $where element.
         */
        static StatusWith<WhereMatchExpressionBase::WhereParams> extractWhereMatchExpressionParams(
            BSONElement where);
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

    // Performs parsing for the match extensions. We do not own this pointer - it has to live
    // as long as the parser is active.
    const ExtensionsCallback* _extensionsCallback;
};

typedef stdx::function<StatusWithMatchExpression(
    const char* name, int type, const BSONObj& section)> MatchExpressionParserGeoCallback;
extern MatchExpressionParserGeoCallback expressionParserGeoCallback;
}
