// matcher.h

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
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/match_details.h"

namespace mongo {

    class Matcher2 {
        MONGO_DISALLOW_COPYING( Matcher2 );

        struct IndexSpliceInfo {
            IndexSpliceInfo() {
                hasNullEquality = false;
            }

            bool hasNullEquality;
        };

    public:
        explicit Matcher2( const BSONObj& pattern, bool nested=false /* do not use */ );

        /**
         * Generate a matcher for the provided index key format using the
         * provided full doc matcher.
         * THIS API WILL GO AWAY EVENTUALLY
         */
        explicit Matcher2( const Matcher2 &docMatcher, const BSONObj &constrainIndexKey );


        bool matches(const BSONObj& doc, MatchDetails* details = NULL ) const;

        bool atomic() const;

        bool hasExistsFalse() const;

        /*
         * this is from old mature
         * criteria is 1 equality expression, nothing else
         */
        bool singleSimpleCriterion() const;

        const BSONObj* getQuery() const { return &_pattern; };
        std::string toString() const { return _pattern.toString(); }

        /**
         * @return true if this key matcher will return the same true/false
         * value as the provided doc matcher.
         */
        bool keyMatch( const Matcher2 &docMatcher ) const;

        static MatchExpression* spliceForIndex( const BSONObj& key,
                                                const MatchExpression* full,
                                                IndexSpliceInfo* spliceInfo );

    private:
        BSONObj _pattern;
        BSONObj _indexKey;

        boost::scoped_ptr<MatchExpression> _expression;

        IndexSpliceInfo _spliceInfo;

        static MatchExpression* _spliceForIndex( const set<std::string>& keys,
                                                 const MatchExpression* full,
                                                 IndexSpliceInfo* spliceInfo );

    };

}
