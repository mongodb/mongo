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

#include <algorithm>
#include <memory>
#include <sstream>
#include "boost/thread/locks.hpp"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/qlog.h"

namespace mongo {

    const int PlanCache::kPlanCacheMaxWriteOperations = 1000;

    //
    // Cache-related functions for CanonicalQuery
    //

    bool PlanCache::shouldCacheQuery(const CanonicalQuery& query) {
        const LiteParsedQuery& lpq = query.getParsed();
        const MatchExpression* expr = query.root();

        // Collection scan
        // No sort order requested
        if (lpq.getSort().isEmpty() &&
            expr->matchType() == MatchExpression::AND && expr->numChildren() == 0) {
            return false;
        }

        // Hint provided
        if (!lpq.getHint().isEmpty()) {
            return false;
        }

        // Min provided
        // Min queries are a special case of hinted queries.
        if (!lpq.getMin().isEmpty()) {
            return false;
        }

        // Max provided
        // Similar to min, max queries are a special case of hinted queries.
        if (!lpq.getMax().isEmpty()) {
            return false;
        }

        return true;
    }

    //
    // CachedSolution
    //

    CachedSolution::CachedSolution(const PlanCacheKey& key, const PlanCacheEntry& entry)
        : plannerData(entry.plannerData.size()),
          backupSoln(entry.backupSoln),
          pinned(entry.pinned),
          pinnedIndex(entry.pinnedIndex),
          shunnedIndexes(entry.shunnedIndexes),
          key(key),
          query(entry.query.copy()),
          sort(entry.sort.copy()),
          projection(entry.projection.copy()) {
        // CachedSolution should not having any references into
        // cache entry. All relevant data should be cloned/copied.
        for (size_t i = 0; i < entry.plannerData.size(); ++i) {
            verify(entry.plannerData[i]);
            plannerData[i] = entry.plannerData[i]->clone();
        }
    }

    CachedSolution::~CachedSolution() {
        for (std::vector<SolutionCacheData*>::const_iterator i = plannerData.begin();
             i != plannerData.end(); ++i) {
            SolutionCacheData* scd = *i;
            delete scd;
        }
    }

    size_t CachedSolution::getWinnerIndex() const {
        // Choose pinned plan always (even if shunned).
        if (pinned) {
            verify(pinnedIndex < plannerData.size());
            return pinnedIndex;
        }

        // Cached solution should contain at least one unshunned plan.
        verify(shunnedIndexes.size() < plannerData.size());
        for (size_t i = 0; i < plannerData.size(); ++i) {
            if (shunnedIndexes.find(i) == shunnedIndexes.end()) {
                return i;
            }
        }

        // Unreachable code.
        verify(0);
        return 0;
    }

    //
    // PlanCacheEntry
    //

    PlanCacheEntry::PlanCacheEntry(const std::vector<QuerySolution*>& solutions,
                                   PlanRankingDecision* d)
        : plannerData(solutions.size()) {
        // The caller of this constructor is responsible for ensuring
        // that the QuerySolution 's' has valid cacheData. If there's no
        // data to cache you shouldn't be trying to construct a PlanCacheEntry.

        // Copy the solution's cache data into the plan cache entry.
        for (size_t i = 0; i < solutions.size(); ++i) {
            verify(solutions[i]->cacheData.get());
            plannerData[i] = solutions[i]->cacheData->clone();
        }

        decision.reset(d);
        pinned = false;
        pinnedIndex = 0;
    }

    PlanCacheEntry::~PlanCacheEntry() {
        for (size_t i = 0; i < feedback.size(); ++i) {
            delete feedback[i];
        }
        for (size_t i = 0; i < plannerData.size(); ++i) {
            delete plannerData[i];
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
    // PlanCacheIndexTree
    //

    void PlanCacheIndexTree::setIndexEntry(const IndexEntry& ie) {
        entry.reset(new IndexEntry(ie));
    }

    PlanCacheIndexTree* PlanCacheIndexTree::clone() const {
        PlanCacheIndexTree* root = new PlanCacheIndexTree();
        if (NULL != entry.get()) {
            root->index_pos = index_pos;
            root->setIndexEntry(*entry.get());
        }

        for (vector<PlanCacheIndexTree*>::const_iterator it = children.begin();
                it != children.end(); ++it) {
            PlanCacheIndexTree* clonedChild = (*it)->clone();
            root->children.push_back(clonedChild);
        }
        return root;
    }

    std::string PlanCacheIndexTree::toString(int indents) const {
        std::stringstream ss;
        if (!children.empty()) {
            ss << string(3 * indents, '-') << "Node" << std::endl;
            int newIndent = indents + 1;
            for (vector<PlanCacheIndexTree*>::const_iterator it = children.begin();
                    it != children.end(); ++it) {
                ss << (*it)->toString(newIndent);
            }
            return ss.str();
        }
        else {
            ss << string(3 * indents, '-') << "Leaf ";
            if (NULL != entry.get()) {
                ss << entry->keyPattern.toString() << ", pos: " << index_pos;
            }
            ss << std::endl;
        }
        return ss.str();
    }

    //
    // SolutionCacheData
    //

    SolutionCacheData* SolutionCacheData::clone() const {
        SolutionCacheData* other = new SolutionCacheData();
        if (NULL != this->tree.get()) {
            // 'tree' could be NULL if the cached solution
            // is a collection scan.
            other->tree.reset(this->tree->clone());
        }
        other->solnType = this->solnType;
        other->wholeIXSolnDir = this->wholeIXSolnDir;
        return other;
    }

    std::string SolutionCacheData::toString() const {
        stringstream ss;
        switch (this->solnType) {
        case WHOLE_IXSCAN_SOLN:
            verify(this->tree.get());
            ss << "(whole index scan solution: "
               << "dir=" << this->wholeIXSolnDir << "; "
               << "tree=" << this->tree->toString()
               << ")";
            break;
        case COLLSCAN_SOLN:
            ss << "(collection scan)";
            break;
        case USE_INDEX_TAGS_SOLN:
            verify(this->tree.get());
            ss << "(index-tagged expression tree: "
               << "tree=" << this->tree->toString()
               << ")";
        }
        return ss.str();
    }

    //
    // PlanCache
    //

    PlanCache::~PlanCache() {
        _clear();
    }

    Status PlanCache::add(const CanonicalQuery& query, const std::vector<QuerySolution*>& solns,
                          PlanRankingDecision* why) {
        verify(why);

        if (solns.empty()) {
            return Status(ErrorCodes::BadValue, "no solutions provided");
        }

        PlanCacheKey key = query.getPlanCacheKey();
        PlanCacheEntry* entry = new PlanCacheEntry(solns, why);
        const LiteParsedQuery& pq = query.getParsed();
        entry->query = pq.getFilter().copy();
        entry->sort = pq.getSort().copy();
        entry->projection = pq.getProj().copy();

        // If the winning solution uses a blocking sort, then try and
        // find a fallback solution that has no blocking sort.
        if (solns[0]->hasSortStage) {
            for (size_t i = 1; i < solns.size(); ++i) {
                if (!solns[i]->hasSortStage) {
                    entry->backupSoln.reset(i);
                    break;
                }
            }
        }

        boost::lock_guard<boost::mutex> cacheLock(_cacheMutex);
        // XXX: Replacing existing entry - revisit when we have real cached solutions.
        // Delete previous entry
        typedef unordered_map<PlanCacheKey, PlanCacheEntry*>::const_iterator ConstIterator;
        ConstIterator i = _cache.find(key);
        if (i != _cache.end()) {
            PlanCacheEntry* previousEntry = i->second;
            delete previousEntry;
        }
        _cache[key] = entry;

        return Status::OK();
    }

    Status PlanCache::get(const CanonicalQuery& query, CachedSolution** crOut) const{
        PlanCacheKey key = query.getPlanCacheKey();
        return get(key, crOut);
    }

    Status PlanCache::get(const PlanCacheKey& key, CachedSolution** crOut) const {
        verify(crOut);

        boost::lock_guard<boost::mutex> cacheLock(_cacheMutex);
        typedef unordered_map<PlanCacheKey, PlanCacheEntry*>::const_iterator ConstIterator;
        ConstIterator i = _cache.find(key);
        if (i == _cache.end()) {
            return Status(ErrorCodes::BadValue, "no such key in cache");
        }
        PlanCacheEntry* entry = i->second;
        verify(entry);

        *crOut = new CachedSolution(key, *entry);

        return Status::OK();
    }

    Status PlanCache::feedback(const PlanCacheKey& ck, PlanCacheEntryFeedback* feedback) {
        boost::lock_guard<boost::mutex> cacheLock(_cacheMutex);
        return Status(ErrorCodes::BadValue, "not implemented yet");
    }

    Status PlanCache::remove(const PlanCacheKey& ck) {
        boost::lock_guard<boost::mutex> cacheLock(_cacheMutex);
        typedef unordered_map<PlanCacheKey, PlanCacheEntry*>::const_iterator ConstIterator;
        ConstIterator i = _cache.find(ck);
        if (i == _cache.end()) {
            return Status(ErrorCodes::BadValue, "no such key in cache");
        }
        PlanCacheEntry* entry = i->second;
        verify(entry);
        _cache.erase(i);
        delete entry;
        return Status::OK();
    }

    void PlanCache::clear() {
        boost::lock_guard<boost::mutex> cacheLock(_cacheMutex);
        _clear();
        _writeOperations.store(0);
    }

    void PlanCache::getKeys(std::vector<PlanCacheKey>* keysOut) const {
        verify(keysOut);

        boost::lock_guard<boost::mutex> cacheLock(_cacheMutex);
        std::vector<PlanCacheKey> keys;
        typedef unordered_map<PlanCacheKey, PlanCacheEntry*>::const_iterator ConstIterator;
        for (ConstIterator i = _cache.begin(); i != _cache.end(); i++) {
            const PlanCacheKey& key = i->first;
            keys.push_back(key);
        }

        // Replace contents of output vector provided in keysOut.
        keys.swap(*keysOut);
    }

    Status PlanCache::pin(const PlanCacheKey& key, const PlanID& plan) {
        boost::lock_guard<boost::mutex> cacheLock(_cacheMutex);
        typedef unordered_map<PlanCacheKey, PlanCacheEntry*>::const_iterator ConstIterator;
        ConstIterator i = _cache.find(key);
        if (i == _cache.end()) {
            return Status(ErrorCodes::BadValue, "no such key in cache");
        }
        PlanCacheEntry* entry = i->second;
        verify(entry);

        // search for plan
        // XXX: remove when we have real cached plans
        for (size_t i = 0; i < entry->plannerData.size(); i++) {
            stringstream ss;
            ss << "plan" << i;
            PlanID currentPlan(ss.str());
            if (currentPlan == plan) {
                entry->pinned = true;
                entry->pinnedIndex = i;
                return Status::OK();
            }
        }

        return Status(ErrorCodes::BadValue, "no such plan in cache");
    }

    Status PlanCache::unpin(const PlanCacheKey& key) {
        boost::lock_guard<boost::mutex> cacheLock(_cacheMutex);
        typedef unordered_map<PlanCacheKey, PlanCacheEntry*>::const_iterator ConstIterator;
        ConstIterator i = _cache.find(key);
        if (i == _cache.end()) {
            return Status(ErrorCodes::BadValue, "no such key in cache");
        }
        PlanCacheEntry* entry = i->second;
        verify(entry);

        entry->pinned = false;

        return Status::OK();
    }

    Status PlanCache::addPlan(const PlanCacheKey& key, const BSONObj& details, PlanID* planOut) {
        boost::lock_guard<boost::mutex> cacheLock(_cacheMutex);
        typedef unordered_map<PlanCacheKey, PlanCacheEntry*>::const_iterator ConstIterator;
        ConstIterator i = _cache.find(key);
        if (i == _cache.end()) {
            return Status(ErrorCodes::BadValue, "no such key in cache");
        }
        PlanCacheEntry* entry = i->second;
        verify(entry);

        // XXX: Generate fake plan ID
        stringstream ss;
        ss << "plan" << entry->plannerData.size();
        PlanID plan(ss.str());

        SolutionCacheData* scd = new SolutionCacheData();
        scd->tree.reset(new PlanCacheIndexTree());
        entry->plannerData.push_back(scd);

        *planOut = plan;
        return Status::OK();
    }

    Status PlanCache::shunPlan(const PlanCacheKey& key, const PlanID& plan) {
        boost::lock_guard<boost::mutex> cacheLock(_cacheMutex);
        typedef unordered_map<PlanCacheKey, PlanCacheEntry*>::const_iterator ConstIterator;
        ConstIterator i = _cache.find(key);
        if (i == _cache.end()) {
            return Status(ErrorCodes::BadValue, "no such key in cache");
        }
        PlanCacheEntry* entry = i->second;
        verify(entry);

        if (i == _cache.end()) {
            return Status(ErrorCodes::BadValue, "no such key in cache");
        }

        // search for plan
        for (size_t i = 0; i < entry->plannerData.size(); i++) {
            stringstream ss;
            ss << "plan" << i;
            PlanID currentPlan(ss.str());
            if (currentPlan == plan) {
                // Do nothing if plan is already shunned.
                if (entry->shunnedIndexes.find(i) != entry->shunnedIndexes.end()) {
                    return Status::OK();
                }

                // Do not proceed if this is the last unshunned plan.
                if ((entry->shunnedIndexes.size() + 1U) == entry->plannerData.size()) {
                    return Status(ErrorCodes::BadValue, "query must have at least one unshunned plan");
                }

                entry->shunnedIndexes.insert(i);
                return Status::OK();
            }
        }

        return Status(ErrorCodes::BadValue, "no such plan in cache");
    }

    void PlanCache::notifyOfWriteOp() {
        // It's fine to clear the cache multiple times if multiple threads
        // increment the counter to kPlanCacheMaxWriteOperations or greater.
        if (_writeOperations.addAndFetch(1) < kPlanCacheMaxWriteOperations) {
            return;
        }
        clear();
    }

    void PlanCache::_clear() {
        typedef unordered_map<PlanCacheKey, PlanCacheEntry*>::const_iterator ConstIterator;
        for (ConstIterator i = _cache.begin(); i != _cache.end(); i++) {
            PlanCacheEntry* entry = i->second;
            delete entry;
        }
        _cache.clear();
    }

}  // namespace mongo
