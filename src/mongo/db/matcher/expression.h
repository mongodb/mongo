// expression.h

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


#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/matcher/match_details.h"
#include "mongo/db/matcher/matchable.h"
#include "mongo/stdx/memory.h"

namespace mongo {

class CollatorInterface;
class MatchExpression;
class TreeMatchExpression;

typedef StatusWith<std::unique_ptr<MatchExpression>> StatusWithMatchExpression;

class MatchExpression {
    MONGO_DISALLOW_COPYING(MatchExpression);

public:
    enum MatchType {
        // tree types
        AND,
        OR,

        // array types
        ELEM_MATCH_OBJECT,
        ELEM_MATCH_VALUE,
        SIZE,

        // leaf types
        EQ,
        LTE,
        LT,
        GT,
        GTE,
        REGEX,
        MOD,
        EXISTS,
        MATCH_IN,
        BITS_ALL_SET,
        BITS_ALL_CLEAR,
        BITS_ANY_SET,
        BITS_ANY_CLEAR,

        // Negations.
        NOT,
        NOR,

        // special types
        TYPE_OPERATOR,
        GEO,
        WHERE,

        // things that maybe shouldn't even be nodes
        ALWAYS_FALSE,

        // Things that we parse but cannot be answered without an index.
        GEO_NEAR,
        TEXT,

        // Expressions that are only created internally
        INTERNAL_2DSPHERE_KEY_IN_REGION,
        INTERNAL_2D_KEY_IN_REGION,
        INTERNAL_2D_POINT_IN_ANNULUS
    };

    MatchExpression(MatchType type);
    virtual ~MatchExpression() {}

    //
    // Structural/AST information
    //

    /**
     * What type is the node?  See MatchType above.
     */
    MatchType matchType() const {
        return _matchType;
    }

    /**
     * How many children does the node have?  Most nodes are leaves so the default impl. is for
     * a leaf.
     */
    virtual size_t numChildren() const {
        return 0;
    }

    /**
     * Get the i-th child.
     */
    virtual MatchExpression* getChild(size_t i) const {
        return NULL;
    }

    /**
     * Get all the children of a node
     */
    virtual std::vector<MatchExpression*>* getChildVector() {
        return NULL;
    }

    /**
     * Get the path of the leaf.  Returns StringData() if there is no path (node is logical).
     */
    virtual const StringData path() const {
        return StringData();
    }

    /**
     * Notes on structure:
     * isLogical, isArray, and isLeaf define three partitions of all possible operators.
     *
     * isLogical can have children and its children can be arbitrary operators.
     *
     * isArray can have children and its children are predicates over one field.
     *
     * isLeaf is a predicate over one field.
     */

    /**
     * Is this node a logical operator?  All of these inherit from ListOfMatchExpression.
     * AND, OR, NOT, NOR.
     */
    bool isLogical() const {
        return AND == _matchType || OR == _matchType || NOT == _matchType || NOR == _matchType;
    }

    /**
     * Is this node an array operator?  Array operators have multiple clauses but operate on one
     * field.
     *
     * ELEM_MATCH_VALUE, ELEM_MATCH_OBJECT, SIZE (ArrayMatchingMatchExpression)
     */
    bool isArray() const {
        return SIZE == _matchType || ELEM_MATCH_VALUE == _matchType ||
            ELEM_MATCH_OBJECT == _matchType;
    }

    /**
     * Not-internal nodes, predicates over one field.  Almost all of these inherit from
     * LeafMatchExpression.
     *
     * Exceptions: WHERE, which doesn't have a field.
     *             TYPE_OPERATOR, which inherits from MatchExpression due to unique array
     *                            semantics.
     */
    bool isLeaf() const {
        return !isArray() && !isLogical();
    }

    // XXX: document
    virtual std::unique_ptr<MatchExpression> shallowClone() const = 0;

    // XXX document
    virtual bool equivalent(const MatchExpression* other) const = 0;

    /**
    * Determine if a document satisfies the tree-predicate.
    *
    * The caller may optionally provide a non-null MatchDetails as an out-parameter. For matching
    *documents, the MatchDetails provide further info on how the document was
    *matched---specifically, which array element matched an array predicate.
    *
    * The caller must check that the MatchDetails is valid via the isValid() method before using.
    */
    virtual bool matches(const MatchableDocument* doc, MatchDetails* details = 0) const = 0;
    virtual bool matchesBSON(const BSONObj& doc, MatchDetails* details = 0) const;

    /**
     * Determines if the element satisfies the tree-predicate.
     * Not valid for all expressions (e.g. $where); in those cases, returns false.
     */
    virtual bool matchesSingleElement(const BSONElement& e) const = 0;

    //
    // Tagging mechanism: Hang data off of the tree for retrieval later.
    //

    class TagData {
    public:
        virtual ~TagData() {}
        virtual void debugString(StringBuilder* builder) const = 0;
        virtual TagData* clone() const = 0;
    };

    /**
     * Takes ownership
     */
    void setTag(TagData* data) {
        _tagData.reset(data);
    }
    TagData* getTag() const {
        return _tagData.get();
    }
    virtual void resetTag() {
        setTag(NULL);
        for (size_t i = 0; i < numChildren(); ++i) {
            getChild(i)->resetTag();
        }
    }

    /**
     * Set the collator 'collator' on this match expression and all its children.
     *
     * 'collator' must outlive the match expression.
     */
    void setCollator(const CollatorInterface* collator);

    /**
     * Serialize the MatchExpression to BSON, appending to 'out'. Output of this method is expected
     * to be a valid query object, that, when parsed, produces a logically equivalent
     * MatchExpression.
     */
    virtual void serialize(BSONObjBuilder* out) const = 0;

    //
    // Debug information
    //
    virtual std::string toString() const;
    virtual void debugString(StringBuilder& debug, int level = 0) const = 0;

protected:
    /**
     * Subclasses that are collation-aware must implement this method in order to capture changes
     * to the collator that occur after initialization time.
     */
    virtual void _doSetCollator(const CollatorInterface* collator){};

    void _debugAddSpace(StringBuilder& debug, int level) const;

private:
    MatchType _matchType;
    std::unique_ptr<TagData> _tagData;
};

class FalseMatchExpression : public MatchExpression {
public:
    FalseMatchExpression(StringData path) : MatchExpression(ALWAYS_FALSE) {
        _path = path;
    }

    virtual bool matches(const MatchableDocument* doc, MatchDetails* details = 0) const {
        return false;
    }

    virtual bool matchesSingleElement(const BSONElement& e) const {
        return false;
    }

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        return stdx::make_unique<FalseMatchExpression>(_path);
    }

    virtual void debugString(StringBuilder& debug, int level = 0) const;

    virtual void serialize(BSONObjBuilder* out) const;

    virtual bool equivalent(const MatchExpression* other) const {
        return other->matchType() == ALWAYS_FALSE;
    }

private:
    StringData _path;
};
}
