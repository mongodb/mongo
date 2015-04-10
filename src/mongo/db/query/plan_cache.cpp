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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/query/plan_cache.h"

#include <algorithm>
#include <math.h>
#include <memory>
#include "boost/thread/locks.hpp"
#include "mongo/base/owned_pointer_vector.h"
#include "mongo/client/dbclientinterface.h"   // For QueryOption_foobar
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

    // Delimiters for cache key encoding.
    const char kEncodeChildrenBegin = '[';
    const char kEncodeChildrenEnd = ']';
    const char kEncodeChildrenSeparator = ',';
    const char kEncodeSortSection = '~';
    const char kEncodeProjectionSection = '|';

    /**
     * Encode user-provided string. Cache key delimiters seen in the
     * user string are escaped with a backslash.
     */
    void encodeUserString(StringData s, str::stream* os) {
        for (size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            switch (c) {
            case kEncodeChildrenBegin:
            case kEncodeChildrenEnd:
            case kEncodeChildrenSeparator:
            case kEncodeSortSection:
            case kEncodeProjectionSection:
            case '\\':
                  *os << '\\';
                // Fall through to default case.
            default:
                *os << c;
            }
        }
    }

    void encodePlanCacheKeyTree(const MatchExpression* tree, str::stream* os);

    /**
     * 2-character encoding of MatchExpression::MatchType.
     */
    const char* encodeMatchType(MatchExpression::MatchType mt) {
        switch(mt) {
        case MatchExpression::AND: return "an"; break;
        case MatchExpression::OR: return "or"; break;
        case MatchExpression::NOR: return "nr"; break;
        case MatchExpression::NOT: return "nt"; break;
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
        default: verify(0); return "";
        }
    }

    /**
     * Encodes GEO match expression.
     * Encoding includes:
     * - type of geo query (within/intersect/near)
     * - geometry type
     * - CRS (flat or spherical)
     */
    void encodeGeoMatchExpression(const GeoMatchExpression* tree, str::stream* os) {
        const GeoExpression& geoQuery = tree->getGeoExpression();

        // Type of geo query.
        switch (geoQuery.getPred()) {
        case GeoExpression::WITHIN: *os << "wi"; break;
        case GeoExpression::INTERSECT: *os << "in"; break;
        case GeoExpression::INVALID: *os << "id"; break;
        }

        // Geometry type.
        // Only one of the shared_ptrs in GeoContainer may be non-NULL.
        *os << geoQuery.getGeometry().getDebugType();

        // CRS (flat or spherical)
        if (FLAT == geoQuery.getGeometry().getNativeCRS()) {
            *os << "fl";
        }
        else if (SPHERE == geoQuery.getGeometry().getNativeCRS()) {
            *os << "sp";
        }
        else if (STRICT_SPHERE == geoQuery.getGeometry().getNativeCRS()) {
            *os << "ss";
        }
        else {
            error() << "unknown CRS type " << (int)geoQuery.getGeometry().getNativeCRS()
                    << " in geometry of type " << geoQuery.getGeometry().getDebugType();
            invariant(false);
        }
    }

    /**
     * Encodes GEO_NEAR match expression.
     * Encode:
     * - isNearSphere
     * - CRS (flat or spherical)
     */
    void encodeGeoNearMatchExpression(const GeoNearMatchExpression* tree,
                                      str::stream* os) {
        const GeoNearExpression& nearQuery = tree->getData();

        // isNearSphere
        *os << (nearQuery.isNearSphere ? "ns" : "nr");

        // CRS (flat or spherical or strict-winding spherical)
        switch (nearQuery.centroid->crs) {
        case FLAT: *os << "fl"; break;
        case SPHERE: *os << "sp"; break;
        case STRICT_SPHERE: *os << "ss"; break;
        case UNSET:
            error() << "unknown CRS type " << (int)nearQuery.centroid->crs
                    << " in point geometry for near query";
            invariant(false);
            break;
        }
    }

    /**
     * Traverses expression tree pre-order.
     * Appends an encoding of each node's match type and path name
     * to the output stream.
     */
    void encodePlanCacheKeyTree(const MatchExpression* tree, str::stream* os) {
        // Encode match type and path.
        *os << encodeMatchType(tree->matchType());

        encodeUserString(tree->path(), os);

        // GEO and GEO_NEAR require additional encoding.
        if (MatchExpression::GEO == tree->matchType()) {
            encodeGeoMatchExpression(static_cast<const GeoMatchExpression*>(tree), os);
        }
        else if (MatchExpression::GEO_NEAR == tree->matchType()) {
            encodeGeoNearMatchExpression(static_cast<const GeoNearMatchExpression*>(tree), os);
        }

        // Traverse child nodes.
        // Enclose children in [].
        if (tree->numChildren() > 0) {
            *os << kEncodeChildrenBegin;
        }
        // Use comma to separate children encoding.
        for (size_t i = 0; i < tree->numChildren(); ++i) {
            if (i > 0) {
                *os << kEncodeChildrenSeparator;
            }
            encodePlanCacheKeyTree(tree->getChild(i), os);
        }
        if (tree->numChildren() > 0) {
            *os << kEncodeChildrenEnd;
        }
    }

    /**
     * Encodes sort order into cache key.
     * Sort order is normalized because it provided by
     * LiteParsedQuery.
     */
    void encodePlanCacheKeySort(const BSONObj& sortObj, str::stream* os) {
        if (sortObj.isEmpty()) {
            return;
        }

        *os << kEncodeSortSection;

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
            encodeUserString(elt.fieldName(), os);

            // Sort argument separator
            if (it.more()) {
                *os << ",";
            }
        }
    }

    /**
     * Encodes parsed projection into cache key.
     * Does a simple toString() on each projected field
     * in the BSON object.
     * Orders the encoded elements in the projection by field name.
     * This handles all the special projection types ($meta, $elemMatch, etc.)
     */
    void encodePlanCacheKeyProj(const BSONObj& projObj, str::stream* os) {
        if (projObj.isEmpty()) {
            return;
        }

        *os << kEncodeProjectionSection;

        // Sorts the BSON elements by field name using a map.
        std::map<StringData, BSONElement> elements;

        BSONObjIterator it(projObj);
        while (it.more()) {
            BSONElement elt = it.next();
            StringData fieldName = elt.fieldNameStringData();
            elements[fieldName] = elt;
        }

        // Read elements in order of field name
        for (std::map<StringData, BSONElement>::const_iterator i = elements.begin();
             i != elements.end(); ++i) {
            const BSONElement& elt = (*i).second;
            // BSONElement::toString() arguments
            // includeFieldName - skip field name (appending after toString() result). false.
            // full: choose less verbose representation of child/data values. false.
            encodeUserString(elt.toString(false, false), os);
            encodeUserString(elt.fieldName(), os);
        }
    }

}  // namespace

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

        // Explain queries are not-cacheable. This is primarily because of
        // the need to generate current and accurate information in allPlans.
        // If the explain report is generated by the cached plan runner using
        // stale information from the cache for the losing plans, allPlans would
        // simply be wrong.
        if (lpq.isExplain()) {
            return false;
        }

        // Tailable cursors won't get cached, just turn into collscans.
        if (query.getParsed().isTailable()) {
            return false;
        }

        // Snapshot is really a hint.
        if (query.getParsed().isSnapshot()) {
            return false;
        }

        return true;
    }

    //
    // CachedSolution
    //

    CachedSolution::CachedSolution(const PlanCacheKey& key, const PlanCacheEntry& entry)
        : plannerData(entry.plannerData.size()),
          key(key),
          query(entry.query.getOwned()),
          sort(entry.sort.getOwned()),
          projection(entry.projection.getOwned()),
          decisionWorks(entry.decision->stats[0]->common.works) {
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
        return entry;
    }

    std::string PlanCacheEntry::toString() const {
        str::stream ss;
        ss << "(query: " << query.toString()
           << ";sort: " << sort.toString()
           << ";projection: " << projection.toString()
           << ";solutions: " << plannerData.size()
           << ")";
        return ss;
    }

    std::string CachedSolution::toString() const {
        str::stream ss;
        ss << "key: " << key << '\n';
        return ss;
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

        for (std::vector<PlanCacheIndexTree*>::const_iterator it = children.begin();
                it != children.end(); ++it) {
            PlanCacheIndexTree* clonedChild = (*it)->clone();
            root->children.push_back(clonedChild);
        }
        return root;
    }

    std::string PlanCacheIndexTree::toString(int indents) const {
        str::stream ss;
        if (!children.empty()) {
            ss << std::string(3 * indents, '-') << "Node\n";
            int newIndent = indents + 1;
            for (std::vector<PlanCacheIndexTree*>::const_iterator it = children.begin();
                    it != children.end(); ++it) {
                ss << (*it)->toString(newIndent);
            }
            return ss;
        }
        else {
            ss << std::string(3 * indents, '-') << "Leaf ";
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
        other->indexFilterApplied = this->indexFilterApplied;
        return other;
    }

    std::string SolutionCacheData::toString() const {
        str::stream ss;
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

    PlanCache::PlanCache() : _cache(internalQueryCacheSize) { }

    PlanCache::PlanCache(const std::string& ns) : _cache(internalQueryCacheSize), _ns(ns) { }

    PlanCache::~PlanCache() { }

    Status PlanCache::add(const CanonicalQuery& query,
                          const std::vector<QuerySolution*>& solns,
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

        boost::lock_guard<boost::mutex> cacheLock(_cacheMutex);
        std::auto_ptr<PlanCacheEntry> evictedEntry = _cache.add(computeKey(query), entry);

        if (NULL != evictedEntry.get()) {
            LOG(1) << _ns << ": plan cache maximum size exceeded - "
                   << "removed least recently used entry "
                   << evictedEntry->toString();
        }

        return Status::OK();
    }

    Status PlanCache::get(const CanonicalQuery& query, CachedSolution** crOut) const {
        PlanCacheKey key = computeKey(query);
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

    Status PlanCache::feedback(const CanonicalQuery& cq, PlanCacheEntryFeedback* feedback) {
        if (NULL == feedback) {
            return Status(ErrorCodes::BadValue, "feedback is NULL");
        }
        std::auto_ptr<PlanCacheEntryFeedback> autoFeedback(feedback);
        PlanCacheKey ck = computeKey(cq);

        boost::lock_guard<boost::mutex> cacheLock(_cacheMutex);
        PlanCacheEntry* entry;
        Status cacheStatus = _cache.get(ck, &entry);
        if (!cacheStatus.isOK()) {
            return cacheStatus;
        }
        invariant(entry);

        // We store up to a constant number of feedback entries.
        if (entry->feedback.size() < size_t(internalQueryCacheFeedbacksStored)) {
            entry->feedback.push_back(autoFeedback.release());
        }

        return Status::OK();
    }

    Status PlanCache::remove(const CanonicalQuery& canonicalQuery) {
        boost::lock_guard<boost::mutex> cacheLock(_cacheMutex);
        return _cache.remove(computeKey(canonicalQuery));
    }

    void PlanCache::clear() {
        boost::lock_guard<boost::mutex> cacheLock(_cacheMutex);
        _cache.clear();
        _writeOperations.store(0);
    }

    PlanCacheKey PlanCache::computeKey(const CanonicalQuery& cq) const {
        str::stream ss;
        encodePlanCacheKeyTree(cq.root(), &ss);
        encodePlanCacheKeySort(cq.getParsed().getSort(), &ss);
        encodePlanCacheKeyProj(cq.getParsed().getProj(), &ss);
        return ss;
    }

    Status PlanCache::getEntry(const CanonicalQuery& query, PlanCacheEntry** entryOut) const {
        PlanCacheKey key = computeKey(query);
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

    bool PlanCache::contains(const CanonicalQuery& cq) const {
        boost::lock_guard<boost::mutex> cacheLock(_cacheMutex);
        return _cache.hasKey(computeKey(cq));
    }

    size_t PlanCache::size() const {
        boost::lock_guard<boost::mutex> cacheLock(_cacheMutex);
        return _cache.size();
    }

    void PlanCache::notifyOfWriteOp() {
        // It's fine to clear the cache multiple times if multiple threads
        // increment the counter to kPlanCacheMaxWriteOperations or greater.
        if (_writeOperations.addAndFetch(1) < internalQueryCacheWriteOpsBetweenFlush) {
            return;
        }

        LOG(1) << _ns << ": clearing collection plan cache - "
               << internalQueryCacheWriteOpsBetweenFlush
               << " write operations detected since last refresh.";
        clear();
    }

}  // namespace mongo
