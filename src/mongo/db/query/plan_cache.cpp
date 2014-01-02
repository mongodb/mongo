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

namespace {

    using std::auto_ptr;
    using std::ostream;
    using std::string;
    using std::stringstream;
    using namespace mongo;

    void encodePlanCacheKeyTree(const MatchExpression* tree, ostream* os);

    /**
     * Comparator for MatchExpression nodes. nodes by:
     * 1) operator type (MatchExpression::MatchType)
     * 2) path name (MatchExpression::path())
     * 3) cache key of the subtree
     *
     * The third item is needed to break ties, thus ensuring that
     * match expression trees which should have the same cache key
     * always sort the same way. If you're wondering when the tuple
     * (operator type, path name) could ever be equal, consider this
     * query:
     *
     * {$and:[{$or:[{a:1},{a:2}]},{$or:[{b:1},{b:2}]}]}
     *
     * The two OR nodes would compare as equal in this case were it
     * not for tuple item #3 (cache key of the subtree).
     */
    bool OperatorAndFieldNameComparison(const MatchExpression* lhs, const MatchExpression* rhs) {
        // First compare by MatchType
        MatchExpression::MatchType lhsMatchType = lhs->matchType();
        MatchExpression::MatchType rhsMatchType = rhs->matchType();
        if (lhsMatchType != rhsMatchType) {
            return lhsMatchType < rhsMatchType;
        }
        // Second, path.
        StringData lhsPath = lhs->path();
        StringData rhsPath = rhs->path();
        if (lhsPath != rhsPath) {
            return lhsPath < rhsPath;
        }
        // Third, cache key.
        stringstream ssLeft, ssRight;
        encodePlanCacheKeyTree(lhs, &ssLeft);
        encodePlanCacheKeyTree(rhs, &ssRight);
        return ssLeft.str() < ssRight.str();
    }

    /**
     * 2-character encoding of MatchExpression::MatchType.
     */
    const char* encodeMatchType(MatchExpression::MatchType mt) {
        switch(mt) {
        case MatchExpression::AND: return "an"; break;
        case MatchExpression::OR: return "or"; break;
        case MatchExpression::NOR: return "nr"; break;
        case MatchExpression::NOT: return "nt"; break;
        case MatchExpression::ALL: return "al"; break;
        case MatchExpression::ELEM_MATCH_OBJECT: return "eo"; break;
        case MatchExpression::ELEM_MATCH_VALUE: return "ev"; break;
        case MatchExpression::SIZE: return "sz"; break;
        case MatchExpression::LTE: return "le"; break;
        case MatchExpression::LT: return "lt"; break;
        case MatchExpression::EQ: return "eq"; break;
        case MatchExpression::GT: return "gt"; break;
        case MatchExpression::GTE: return "ge"; break;
        case MatchExpression::REGEX: return "re"; break;
        case MatchExpression::MOD: return "mo"; break;
        case MatchExpression::EXISTS: return "ex"; break;
        case MatchExpression::MATCH_IN: return "in"; break;
        case MatchExpression::NIN: return "ni"; break;
        case MatchExpression::TYPE_OPERATOR: return "ty"; break;
        case MatchExpression::GEO: return "go"; break;
        case MatchExpression::WHERE: return "wh"; break;
        case MatchExpression::ATOMIC: return "at"; break;
        case MatchExpression::ALWAYS_FALSE: return "af"; break;
        case MatchExpression::GEO_NEAR: return "gn"; break;
        case MatchExpression::TEXT: return "te"; break;
        }
        // Unreachable code.
        // All MatchType values have been handled in switch().
        verify(0);
        return "";
    }

    /**
     * Traverses expression tree pre-order.
     * Appends an encoding of each node's match type and path name
     * to the output stream.
     */
    void encodePlanCacheKeyTree(const MatchExpression* tree, ostream* os) {
        // Encode match type and path.
        *os << encodeMatchType(tree->matchType()) << tree->path();
        // Traverse child nodes.
        for (size_t i = 0; i < tree->numChildren(); ++i) {
            encodePlanCacheKeyTree(tree->getChild(i), os);
        }
    }

    /**
     * Encodes sort order into cache key.
     * Sort order is normalized because it provided by
     * LiteParsedQuery.
     */
    void encodePlanCacheKeySort(const BSONObj& sortObj, ostream* os) {
        BSONObjIterator it(sortObj);
        while (it.more()) {
            BSONElement elt = it.next();
            // $meta text score
            if (LiteParsedQuery::isTextScoreMeta(elt)) {
                *os << "t";
            }
            // Ascending
            else if (elt.numberInt() == 1) {
                *os << "a";
            }
            // Descending
            else {
                *os << "d";
            }
            *os << elt.fieldName();
        }
    }

} // namespace

namespace mongo {

    //
    // Cache-related functions for CanonicalQuery
    //

    bool shouldCacheQuery(const CanonicalQuery& query) {
        const LiteParsedQuery& lpq = query.getParsed();
        const MatchExpression* expr = query.root();

        // Collection scan
        if (expr->matchType() == MatchExpression::AND && expr->numChildren() == 0) {
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

    /**
     * For every non-leaf node, sorts child nodes by (MatchType, path name).
     */
    void normalizeQueryForCache(CanonicalQuery* queryOut) {
        // Sorting is the only normalization for now.
        PlanCache::sortTree(queryOut->root());
    }

    /**
     * Cache key is a string-ified combination of the query and sort obfuscated
     * for minimal user comprehension.
     */
    PlanCacheKey getPlanCacheKey(const CanonicalQuery& query) {
        stringstream ss;
        encodePlanCacheKeyTree(query.root(), &ss);
        encodePlanCacheKeySort(query.getParsed().getSort(), &ss);
        PlanCacheKey key(ss.str());
        return key;
    }

    //
    // PlanCacheEntry
    //

    PlanCacheEntry::PlanCacheEntry(const QuerySolution& s, PlanRankingDecision* d) {
        // The caller of this constructor is responsible for ensuring
        // that the QuerySolution 's' has valid cacheData. If there's no
        // data to cache you shouldn't be trying to construct a PlanCacheEntry.
        verify(NULL != s.cacheData.get());

        decision.reset(d);
        pinned = false;
        numPlans = 0;

        // Copy the solution's cache data into the plan cache entry.
        plannerData.reset(s.cacheData->clone());
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
        return this->tree->toString();
    }

    //
    // PlanCache
    //

    PlanCache::~PlanCache() {
        _clear();
    }

    Status PlanCache::add(const CanonicalQuery& query, const QuerySolution& soln,
                          PlanRankingDecision* why) {
        verify(why);

        if (NULL == soln.cacheData.get()) {
            return Status(ErrorCodes::BadValue, "invalid solution - no cache data");
        }

        PlanCacheKey key = getPlanCacheKey(query);
        PlanCacheEntry* entry = new PlanCacheEntry(soln, why);
        // XXX: fix later
        entry->numPlans = why->onlyOneSolution ? 1 : 2;
        const LiteParsedQuery& pq = query.getParsed();
        entry->query = pq.getFilter().copy();
        entry->sort = pq.getSort().copy();
        entry->projection = pq.getProj().copy();

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
        PlanCacheKey key = getPlanCacheKey(query);
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

        auto_ptr<CachedSolution> cr(new CachedSolution());
        cr->key = key;
        cr->numPlans = entry->numPlans;
        cr->query = entry->query;
        cr->sort = entry->sort;
        cr->projection = entry->projection;
        cr->plannerData.reset(entry->plannerData->clone());

        *crOut = cr.release();
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
    }

    void PlanCache::getKeys(std::vector<PlanCacheKey>* keysOut) const {
        verify(keysOut);

        boost::lock_guard<boost::mutex> cacheLock(_cacheMutex);
        typedef unordered_map<PlanCacheKey, PlanCacheEntry*>::const_iterator ConstIterator;
        for (ConstIterator i = _cache.begin(); i != _cache.end(); i++) {
            const PlanCacheKey& key = i->first;
            keysOut->push_back(key);
        }
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
        for (size_t i = 0; i < entry->numPlans; i++) {
            stringstream ss;
            ss << "plan" << i;
            PlanID currentPlan(ss.str());
            if (currentPlan == plan) {
                entry->pinned = true;
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
        ss << "plan" << entry->numPlans;
        PlanID plan(ss.str());

        entry->numPlans++;

        *planOut = plan;
        return Status::OK();
    }

    // static
    void PlanCache::sortTree(MatchExpression* tree) {
        for (size_t i = 0; i < tree->numChildren(); ++i) {
            sortTree(tree->getChild(i));
        }
        std::vector<MatchExpression*>* children = tree->getChildVector();
        if (NULL != children) {
            std::sort(children->begin(), children->end(), OperatorAndFieldNameComparison);
        }
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

        // Do not proceed if there's only one plan for this query.
        if (1 == entry->numPlans) {
            return Status(ErrorCodes::BadValue, "cannot shun only plan for query");
        }
       
        if (i == _cache.end()) {
            return Status(ErrorCodes::BadValue, "no such key in cache");
        }
        // search for plan
        // XXX: remove when we have real cached plans
        for (size_t i = 0; i < entry->numPlans; i++) {
            stringstream ss;
            ss << "plan" << i;
            PlanID currentPlan(ss.str());
            if (currentPlan == plan) {
                entry->numPlans--;
                return Status::OK();
            }
        }

        return Status(ErrorCodes::BadValue, "no such plan in cache");
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
