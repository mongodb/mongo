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
#include <math.h>
#include <memory>
#include "boost/thread/locks.hpp"
#include "mongo/base/owned_pointer_vector.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/qlog.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    const int PlanCache::kPlanCacheMaxWriteOperations = 1000;

    const int PlanCache::kMaxCacheSize = 200;

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
          key(key),
          query(entry.query.getOwned()),
          sort(entry.sort.getOwned()),
          projection(entry.projection.getOwned()) {
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

    //
    // PlanCacheEntry
    //

    PlanCacheEntry::PlanCacheEntry(const std::vector<QuerySolution*>& solutions,
                                   PlanRankingDecision* why)
        : plannerData(solutions.size()),
          decision(why) {
        invariant(why);

        // The caller of this constructor is responsible for ensuring
        // that the QuerySolution 's' has valid cacheData. If there's no
        // data to cache you shouldn't be trying to construct a PlanCacheEntry.

        // Copy the solution's cache data into the plan cache entry.
        for (size_t i = 0; i < solutions.size(); ++i) {
            invariant(solutions[i]->cacheData.get());
            plannerData[i] = solutions[i]->cacheData->clone();
        }
    }

    PlanCacheEntry::~PlanCacheEntry() {
        for (size_t i = 0; i < feedback.size(); ++i) {
            delete feedback[i];
        }
        for (size_t i = 0; i < plannerData.size(); ++i) {
            delete plannerData[i];
        }
    }

    PlanCacheEntry* PlanCacheEntry::clone() const {
        OwnedPointerVector<QuerySolution> solutions;
        for (size_t i = 0; i < plannerData.size(); ++i) {
            QuerySolution* qs = new QuerySolution();
            qs->cacheData.reset(plannerData[i]->clone());
            solutions.mutableVector().push_back(qs);
        }
        PlanCacheEntry* entry = new PlanCacheEntry(solutions.vector(), decision->clone());

        entry->backupSoln = backupSoln;

        // Copy query shape.
        entry->query = query.getOwned();
        entry->sort = sort.getOwned();
        entry->projection = projection.getOwned();

        // Copy performance stats.
        for (size_t i = 0; i < feedback.size(); ++i) {
            PlanCacheEntryFeedback* fb = new PlanCacheEntryFeedback();
            fb->stats.reset(feedback[i]->stats->clone());
            fb->score = feedback[i]->score;
            entry->feedback.push_back(fb);
        }
        entry->averageScore = averageScore;
        entry->stddevScore = stddevScore;
        return entry;
    }

    string PlanCacheEntry::toString() const {
        mongoutils::str::stream ss;
        ss << "(query: " << query.toString()
           << ";sort: " << sort.toString()
           << ";projection: " << projection.toString()
           << ";solutions: " << plannerData.size()
           << ")";
        return ss;
    }

    string CachedSolution::toString() const {
        mongoutils::str::stream ss;
        ss << "key: " << key << '\n';
        return ss;
    }

    // static
    const size_t PlanCacheEntry::kMaxFeedback = 20;

    // static
    const double PlanCacheEntry::kStdDevThreshold = 2.0;

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
        mongoutils::str::stream ss;
        if (!children.empty()) {
            ss << string(3 * indents, '-') << "Node\n";
            int newIndent = indents + 1;
            for (vector<PlanCacheIndexTree*>::const_iterator it = children.begin();
                    it != children.end(); ++it) {
                ss << (*it)->toString(newIndent);
            }
            return ss;
        }
        else {
            ss << string(3 * indents, '-') << "Leaf ";
            if (NULL != entry.get()) {
                ss << entry->keyPattern.toString() << ", pos: " << index_pos;
            }
            ss << '\n';
        }
        return ss;
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
        other->adminHintApplied = this->adminHintApplied;
        return other;
    }

    std::string SolutionCacheData::toString() const {
        mongoutils::str::stream ss;
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
        return ss;
    }

    //
    // PlanCache
    //

    PlanCache::PlanCache() : _cache(kMaxCacheSize) { }

    PlanCache::PlanCache(const std::string& ns) : _cache(kMaxCacheSize), _ns(ns) { }

    PlanCache::~PlanCache() { }

    Status PlanCache::add(const CanonicalQuery& query, const std::vector<QuerySolution*>& solns,
                          PlanRankingDecision* why) {
        invariant(why);

        if (solns.empty()) {
            return Status(ErrorCodes::BadValue, "no solutions provided");
        }

        if (why->stats.size() != solns.size()) {
            return Status(ErrorCodes::BadValue,
                          "number of stats in decision must match solutions");
        }

        if (why->scores.size() != solns.size()) {
            return Status(ErrorCodes::BadValue,
                          "number of scores in decision must match solutions");
        }

        if (why->candidateOrder.size() != solns.size()) {
            return Status(ErrorCodes::BadValue,
                          "candidate ordering entries in decision must match solutions");
        }

        PlanCacheEntry* entry = new PlanCacheEntry(solns, why);
        const LiteParsedQuery& pq = query.getParsed();
        entry->query = pq.getFilter().getOwned();
        entry->sort = pq.getSort().getOwned();
        entry->projection = pq.getProj().getOwned();

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
        std::auto_ptr<PlanCacheEntry> evictedEntry = _cache.add(query.getPlanCacheKey(), entry);

        if (NULL != evictedEntry.get()) {
            LOG(1) << _ns << ": plan cache maximum size exceeded - "
                   << "removed least recently used entry "
                   << evictedEntry->toString();
        }

        return Status::OK();
    }

    Status PlanCache::get(const CanonicalQuery& query, CachedSolution** crOut) const {
        const PlanCacheKey& key = query.getPlanCacheKey();
        verify(crOut);

        boost::lock_guard<boost::mutex> cacheLock(_cacheMutex);
        PlanCacheEntry* entry;
        Status cacheStatus = _cache.get(key, &entry);
        if (!cacheStatus.isOK()) {
            return cacheStatus;
        }
        invariant(entry);

        *crOut = new CachedSolution(key, *entry);

        return Status::OK();
    }

    // XXX: Figure out what the right policy is here for determining if
    // the cached solution is bad.
    static bool hasCachedPlanPerformanceDegraded(PlanCacheEntry* entry,
                                                 PlanCacheEntryFeedback* latestFeedback) {

        if (!entry->averageScore) {
            // We haven't computed baseline performance stats for this cached plan yet.
            // Let's do that now.

            // Compute mean score.
            double sum = 0;
            for (size_t i = 0; i < entry->feedback.size(); ++i) {
                sum += entry->feedback[i]->score;
            }
            double mean = sum / entry->feedback.size();

            // Compute std deviation of scores.
            double sum_of_squares = 0;
            for (size_t i = 0; i < entry->feedback.size(); ++i) {
                double iscore = entry->feedback[i]->score;
                sum_of_squares += (iscore - mean) * (iscore - mean);
            }
            double stddev = sqrt(sum_of_squares / (entry->feedback.size() - 1));

            // If the score has gotten more than a standard deviation lower than
            // its initial value, we should uncache the entry.
            double initialScore = entry->decision->scores[0];
            if ((initialScore - mean) > (PlanCacheEntry::kStdDevThreshold * stddev)) {
                return true;
            }

            entry->averageScore.reset(mean);
            entry->stddevScore.reset(stddev);
        }

        // If the latest use of this plan cache entry is too far from the expected
        // performance, then we should uncache the entry.
        if ((*entry->averageScore - latestFeedback->score)
             > (PlanCacheEntry::kStdDevThreshold * (*entry->stddevScore))) {
            return true;
        }

        return false;
    }

    Status PlanCache::feedback(const CanonicalQuery& cq, PlanCacheEntryFeedback* feedback) {
        if (NULL == feedback) {
            return Status(ErrorCodes::BadValue, "feedback is NULL");
        }
        std::auto_ptr<PlanCacheEntryFeedback> autoFeedback(feedback);
        const PlanCacheKey& ck = cq.getPlanCacheKey();

        boost::lock_guard<boost::mutex> cacheLock(_cacheMutex);
        PlanCacheEntry* entry;
        Status cacheStatus = _cache.get(ck, &entry);
        if (!cacheStatus.isOK()) {
            return cacheStatus;
        }
        invariant(entry);

        if (entry->feedback.size() >= PlanCacheEntry::kMaxFeedback) {
            // If we have enough feedback, then use it to determine whether
            // we should get rid of the cached solution.
            if (hasCachedPlanPerformanceDegraded(entry, autoFeedback.get())) {
                LOG(1) << _ns << ": removing plan cache entry " << entry->toString()
                       << " - detected degradation in performance of cached solution.";
                _cache.remove(ck);
            }
        }
        else {
            // We don't have enough feedback yet---just store it and move on.
            entry->feedback.push_back(autoFeedback.release());
        }

        return Status::OK();
    }

    Status PlanCache::remove(const CanonicalQuery& canonicalQuery) {
        boost::lock_guard<boost::mutex> cacheLock(_cacheMutex);
        return _cache.remove(canonicalQuery.getPlanCacheKey());
    }

    void PlanCache::clear() {
        boost::lock_guard<boost::mutex> cacheLock(_cacheMutex);
        _cache.clear();
        _writeOperations.store(0);
    }

    Status PlanCache::getEntry(const CanonicalQuery& query, PlanCacheEntry** entryOut) const {
        const PlanCacheKey& key = query.getPlanCacheKey();
        verify(entryOut);

        boost::lock_guard<boost::mutex> cacheLock(_cacheMutex);
        PlanCacheEntry* entry;
        Status cacheStatus = _cache.get(key, &entry);
        if (!cacheStatus.isOK()) {
            return cacheStatus;
        }
        invariant(entry);

        *entryOut = entry->clone();

        return Status::OK();
    }

    std::vector<PlanCacheEntry*> PlanCache::getAllEntries() const {
        boost::lock_guard<boost::mutex> cacheLock(_cacheMutex);
        std::vector<PlanCacheEntry*> entries;
        typedef std::list< std::pair<PlanCacheKey, PlanCacheEntry*> >::const_iterator ConstIterator;
        for (ConstIterator i = _cache.begin(); i != _cache.end(); i++) {
            PlanCacheEntry* entry = i->second;
            entries.push_back(entry->clone());
        }

        return entries;
    }

    size_t PlanCache::size() const {
        boost::lock_guard<boost::mutex> cacheLock(_cacheMutex);
        return _cache.size();
    }

    void PlanCache::notifyOfWriteOp() {
        // It's fine to clear the cache multiple times if multiple threads
        // increment the counter to kPlanCacheMaxWriteOperations or greater.
        if (_writeOperations.addAndFetch(1) < kPlanCacheMaxWriteOperations) {
            return;
        }
        LOG(1) << _ns << ": clearing collection plan cache - " << kPlanCacheMaxWriteOperations
               << " write operations on detected since last refresh.";
        clear();
    }

}  // namespace mongo
