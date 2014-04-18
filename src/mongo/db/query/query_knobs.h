/**
 *    Copyright (C) 2014 MongoDB Inc.
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

namespace mongo {

    //
    // multi-plan ranking
    //

    // Max number of times we call work() on plans before comparing them,
    // for small collections.
    extern int internalQueryPlanEvaluationWorks;

    // For large collections, the number times we work() candidate plans is
    // taken as this fraction of the collection size.
    extern double internalQueryPlanEvaluationCollFraction;

    // Stop working plans once a plan returns this many results.
    extern int internalQueryPlanEvaluationMaxResults;

    // Do we give a big ranking bonus to intersection plans?
    extern bool internalQueryForceIntersectionPlans;

    // Do we have ixisect on at all?
    extern bool internalQueryPlannerEnableIndexIntersection;

    //
    // plan cache
    //

    // How many entries in the cache?
    extern int internalQueryCacheSize;

    // How many feedback entries do we collect before possibly evicting from the cache based on bad
    // performance?
    extern int internalQueryCacheFeedbacksStored;

    // How many stddevs must a feedback be from the 'reference' performance for us to evict the
    // entry from the cache?
    extern double internalQueryCacheStdDeviations;

    // How many write ops should we allow in a collection before tossing all cache entries?
    extern int internalQueryCacheWriteOpsBetweenFlush;

    //
    // Planning and enumeration.
    //

    // How many indexed solutions will QueryPlanner::plan output?
    extern int internalQueryPlannerMaxIndexedSolutions;

    // How many solutions will the enumerator consider at each OR?
    extern int internalQueryEnumerationMaxOrSolutions;

    // How many intersections will the enumerator consider at each AND?
    extern int internalQueryEnumerationMaxIntersectPerAnd;

    // Do we want to plan each child of the OR independently?
    extern bool internalQueryPlanOrChildrenIndependently;

}  // namespace mongo
