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

#include "mongo/db/query/plan_cache.h"

#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/qlog.h"

namespace mongo {

    //
    // PlanCacheEntry
    //

    PlanCacheEntry::PlanCacheEntry(const QuerySolution& s, PlanRankingDecision* d) {
        decision.reset(d);
        pinned = false;
        // XXX: pull things out of 's' that we need to inorder to recreate the same soln
    }

    PlanCacheEntry::~PlanCacheEntry() {
        for (size_t i = 0; i < feedback.size(); ++i) {
            delete feedback[i];
        }
    }

    string PlanCacheEntry::toString() const {
        stringstream ss;
        ss << "pinned?: " << pinned << endl;
        return ss.str();
    }

    string CachedSolution::toString() const {
        stringstream ss;
        ss << "key: " << key << endl;
        return ss.str();
    }

    //
    // PlanCache
    //

    PlanCache::~PlanCache() { clear(); }

    Status PlanCache::add(const CanonicalQuery& query, const QuerySolution& soln,
                          PlanRankingDecision* why) {
        return Status(ErrorCodes::BadValue, "not implemented yet");
    }

    Status PlanCache::get(const CanonicalQuery& query, CachedSolution** crOut) {
        return Status(ErrorCodes::BadValue, "not implemented yet");
    }

    Status PlanCache::feedback(const PlanCacheKey& ck, PlanCacheEntryFeedback* feedback) {
        return Status(ErrorCodes::BadValue, "not implemented yet");
    }

    Status PlanCache::remove(const PlanCacheKey& ck) {
        return Status(ErrorCodes::BadValue, "not implemented yet");
    }

    void PlanCache::clear() {
        // XXX: implement
    }

}  // namespace mongo
