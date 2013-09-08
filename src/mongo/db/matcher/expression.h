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

#include <boost/scoped_ptr.hpp>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/matcher/matchable.h"
#include "mongo/db/matcher/match_details.h"

namespace mongo {

    class TreeMatchExpression;

    class MatchExpression {
        MONGO_DISALLOW_COPYING( MatchExpression );
    public:
        enum MatchType {
            // tree types
            AND, OR, NOR, NOT,

            // array types
            ALL, ELEM_MATCH_OBJECT, ELEM_MATCH_VALUE, SIZE,

            // leaf types
            LTE, LT, EQ, GT, GTE, REGEX, MOD, EXISTS, MATCH_IN, NIN,

            // special types
            TYPE_OPERATOR, GEO, WHERE,

            // things that maybe shouldn't even be nodes
            ATOMIC, ALWAYS_FALSE,

            // Things that we parse but cannot be answered without an index.
            // TODO: Text goes here eventually.
            GEO_NEAR,
        };

        MatchExpression( MatchType type );
        virtual ~MatchExpression(){}

        //
        // Structural/AST information
        //

        /**
         * What type is the node?  See MatchType above.
         */
        MatchType matchType() const { return _matchType; }

        /**
         * How many children does the node have?  Most nodes are leaves so the default impl. is for
         * a leaf.
         */
        virtual size_t numChildren() const { return 0; }

        /**
         * Get the i-th child.
         */
        virtual MatchExpression* getChild( size_t i ) const { return NULL; }

        /**
         * Get the path of the leaf.  Returns StringData() if there is no path (node is logical).
         */
        virtual const StringData path() const { return StringData(); }

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
         * ALL (AllElemMatchOp)
         * ELEM_MATCH_VALUE, ELEM_MATCH_OBJECT, SIZE (ArrayMatchingMatchExpression)
         */
        bool isArray() const {
            return SIZE == _matchType || ALL == _matchType || ELEM_MATCH_VALUE == _matchType
                   || ELEM_MATCH_OBJECT == _matchType;
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
        virtual MatchExpression* shallowClone() const = 0;

        //
        // Determine if a document satisfies the tree-predicate.
        //

        virtual bool matches( const MatchableDocument* doc, MatchDetails* details = 0 ) const = 0;

        virtual bool matchesBSON( const BSONObj& doc, MatchDetails* details = 0 ) const;

        /**
         * Determines if the element satisfies the tree-predicate.
         * Not valid for all expressions (e.g. $where); in those cases, returns false.
         */
        virtual bool matchesSingleElement( const BSONElement& e ) const = 0;

        //
        // Tagging mechanism: Hang data off of the tree for retrieval later.
        //

        class TagData {
        public:
            virtual ~TagData() { }
            virtual void debugString(StringBuilder* builder) const = 0;
            virtual TagData* clone() const = 0;
        };

        /**
         * Takes ownership
         */
        void setTag(TagData* data) { _tagData.reset(data); }
        TagData* getTag() const { return _tagData.get(); }
        virtual void resetTag() = 0;

        //
        // Debug information
        //
        virtual string toString() const;
        virtual void debugString( StringBuilder& debug, int level = 0 ) const = 0;

        virtual bool equivalent( const MatchExpression* other ) const = 0;
    protected:
        void _debugAddSpace( StringBuilder& debug, int level ) const;

    private:
        MatchType _matchType;
        boost::scoped_ptr<TagData> _tagData;
    };

    /**
     * this isn't really an expression, but a hint to other things
     * not sure where to put it in the end
     */
    class AtomicMatchExpression : public MatchExpression {
    public:
        AtomicMatchExpression() : MatchExpression( ATOMIC ){}

        virtual bool matches( const MatchableDocument* doc, MatchDetails* details = 0 ) const {
            return true;
        }

        virtual bool matchesSingleElement( const BSONElement& e ) const {
            return true;
        }

        virtual MatchExpression* shallowClone() const {
            return new AtomicMatchExpression();
        }

        virtual void debugString( StringBuilder& debug, int level = 0 ) const;

        virtual bool equivalent( const MatchExpression* other ) const {
            return other->matchType() == ATOMIC;
        }

        virtual void resetTag() { setTag(NULL); }
    };

    class FalseMatchExpression : public MatchExpression {
    public:
        FalseMatchExpression() : MatchExpression( ALWAYS_FALSE ){}

        virtual bool matches( const MatchableDocument* doc, MatchDetails* details = 0 ) const {
            return false;
        }

        virtual bool matchesSingleElement( const BSONElement& e ) const {
            return false;
        }

        virtual MatchExpression* shallowClone() const {
            return new FalseMatchExpression();
        }

        virtual void debugString( StringBuilder& debug, int level = 0 ) const;

        virtual bool equivalent( const MatchExpression* other ) const {
            return other->matchType() == ALWAYS_FALSE;
        }

        virtual void resetTag() { setTag(NULL); }
    };

}
