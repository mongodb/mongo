/**
 *    Copyright (C) 2011 10gen Inc.
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

#include "mongo/db/jsobj.h"

namespace mongo {
    
    class QueryPlan;
    
    /**
     * An interface for policies overriding the query optimizer's default behavior for selecting
     * query plans and creating cursors.
     */
    class QueryPlanSelectionPolicy {
    public:
        virtual ~QueryPlanSelectionPolicy() {}
        virtual string name() const = 0;
        virtual bool permitOptimalNaturalPlan() const { return true; }
        virtual bool permitOptimalIdPlan() const { return true; }
        virtual bool permitPlan( const QueryPlan& plan ) const { return true; }
        virtual BSONObj planHint( const StringData& ns ) const { return BSONObj(); }

        /**
         * @return true to request that a created Cursor provide a matcher().  If false, the
         * Cursor's matcher() may be NULL if the Cursor can perform accurate query matching
         * internally using a non Matcher mechanism.  One case where a Matcher might be requested
         * even though not strictly necessary to select matching documents is if metadata about
         * matches may be requested using MatchDetails.  NOTE This is a hint that the Cursor use a
         * Matcher, but the hint may be ignored.  In some cases the Cursor may not provide
         * a Matcher even if 'requestMatcher' is true.
         */
        virtual bool requestMatcher() const { return true; }

        /**
         * @return true to request creating an IntervalBtreeCursor rather than a BtreeCursor when
         * possible.  An IntervalBtreeCursor is optimized for counting the number of documents
         * between two endpoints in a btree.  NOTE This is a hint to create an interval cursor, but
         * the hint may be ignored.  In some cases a different cursor type may be created even if
         * 'requestIntervalCursor' is true.
         */
        virtual bool requestIntervalCursor() const { return false; }
        
        /** Allow any query plan selection, permitting the query optimizer's default behavior. */
        static const QueryPlanSelectionPolicy& any();

        /** Prevent unindexed collection scans. */
        static const QueryPlanSelectionPolicy& indexOnly();

        /**
         * Generally hints to use the _id plan, falling back to the $natural plan.  However, the
         * $natural plan will always be used if optimal for the query.
         */
        static const QueryPlanSelectionPolicy& idElseNatural();
        
    private:
        class Any;
        static Any __any;
        class IndexOnly;
        static IndexOnly __indexOnly;
        class IdElseNatural;
        static IdElseNatural __idElseNatural;
    };

    class QueryPlanSelectionPolicy::Any : public QueryPlanSelectionPolicy {
    public:
        virtual string name() const { return "any"; }
    };
    
    class QueryPlanSelectionPolicy::IndexOnly : public QueryPlanSelectionPolicy {
    public:
        virtual string name() const { return "indexOnly"; }
        virtual bool permitOptimalNaturalPlan() const { return false; }
        virtual bool permitPlan( const QueryPlan& plan ) const;
    };

    class QueryPlanSelectionPolicy::IdElseNatural : public QueryPlanSelectionPolicy {
    public:
        virtual string name() const { return "idElseNatural"; }
        virtual bool permitPlan( const QueryPlan& plan ) const;
        virtual BSONObj planHint( const StringData& ns ) const;
    };
    
} // namespace mongo
