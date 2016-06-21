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

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/client/dbclientinterface.h"  // For QueryOption_foobar
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include <algorithm>
#include <math.h>
#include <memory>

namespace mongo {
namespace {

// Delimiters for cache key encoding.
const char kEncodeDiscriminatorsBegin = '<';
const char kEncodeDiscriminatorsEnd = '>';
const char kEncodeChildrenBegin = '[';
const char kEncodeChildrenEnd = ']';
const char kEncodeChildrenSeparator = ',';
const char kEncodeSortSection = '~';
const char kEncodeProjectionSection = '|';
const char kEncodeCollationSection = '#';

/**
 * Encode user-provided string. Cache key delimiters seen in the
 * user string are escaped with a backslash.
 */
void encodeUserString(StringData s, StringBuilder* keyBuilder) {
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        switch (c) {
            case kEncodeDiscriminatorsBegin:
            case kEncodeDiscriminatorsEnd:
            case kEncodeChildrenBegin:
            case kEncodeChildrenEnd:
            case kEncodeChildrenSeparator:
            case kEncodeSortSection:
            case kEncodeProjectionSection:
            case kEncodeCollationSection:
            case '\\':
                *keyBuilder << '\\';
            // Fall through to default case.
            default:
                *keyBuilder << c;
        }
    }
}

/**
 * 2-character encoding of MatchExpression::MatchType.
 */
const char* encodeMatchType(MatchExpression::MatchType mt) {
    switch (mt) {
        case MatchExpression::AND:
            return "an";
            break;
        case MatchExpression::OR:
            return "or";
            break;
        case MatchExpression::NOR:
            return "nr";
            break;
        case MatchExpression::NOT:
            return "nt";
            break;
        case MatchExpression::ELEM_MATCH_OBJECT:
            return "eo";
            break;
        case MatchExpression::ELEM_MATCH_VALUE:
            return "ev";
            break;
        case MatchExpression::SIZE:
            return "sz";
            break;
        case MatchExpression::LTE:
            return "le";
            break;
        case MatchExpression::LT:
            return "lt";
            break;
        case MatchExpression::EQ:
            return "eq";
            break;
        case MatchExpression::GT:
            return "gt";
            break;
        case MatchExpression::GTE:
            return "ge";
            break;
        case MatchExpression::REGEX:
            return "re";
            break;
        case MatchExpression::MOD:
            return "mo";
            break;
        case MatchExpression::EXISTS:
            return "ex";
            break;
        case MatchExpression::MATCH_IN:
            return "in";
            break;
        case MatchExpression::TYPE_OPERATOR:
            return "ty";
            break;
        case MatchExpression::GEO:
            return "go";
            break;
        case MatchExpression::WHERE:
            return "wh";
            break;
        case MatchExpression::ALWAYS_FALSE:
            return "af";
            break;
        case MatchExpression::GEO_NEAR:
            return "gn";
            break;
        case MatchExpression::TEXT:
            return "te";
            break;
        case MatchExpression::BITS_ALL_SET:
            return "ls";
            break;
        case MatchExpression::BITS_ALL_CLEAR:
            return "lc";
            break;
        case MatchExpression::BITS_ANY_SET:
            return "ys";
            break;
        case MatchExpression::BITS_ANY_CLEAR:
            return "yc";
            break;
        default:
            verify(0);
            return "";
    }
}

/**
 * Encodes GEO match expression.
 * Encoding includes:
 * - type of geo query (within/intersect/near)
 * - geometry type
 * - CRS (flat or spherical)
 */
void encodeGeoMatchExpression(const GeoMatchExpression* tree, StringBuilder* keyBuilder) {
    const GeoExpression& geoQuery = tree->getGeoExpression();

    // Type of geo query.
    switch (geoQuery.getPred()) {
        case GeoExpression::WITHIN:
            *keyBuilder << "wi";
            break;
        case GeoExpression::INTERSECT:
            *keyBuilder << "in";
            break;
        case GeoExpression::INVALID:
            *keyBuilder << "id";
            break;
    }

    // Geometry type.
    // Only one of the shared_ptrs in GeoContainer may be non-NULL.
    *keyBuilder << geoQuery.getGeometry().getDebugType();

    // CRS (flat or spherical)
    if (FLAT == geoQuery.getGeometry().getNativeCRS()) {
        *keyBuilder << "fl";
    } else if (SPHERE == geoQuery.getGeometry().getNativeCRS()) {
        *keyBuilder << "sp";
    } else if (STRICT_SPHERE == geoQuery.getGeometry().getNativeCRS()) {
        *keyBuilder << "ss";
    } else {
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
void encodeGeoNearMatchExpression(const GeoNearMatchExpression* tree, StringBuilder* keyBuilder) {
    const GeoNearExpression& nearQuery = tree->getData();

    // isNearSphere
    *keyBuilder << (nearQuery.isNearSphere ? "ns" : "nr");

    // CRS (flat or spherical or strict-winding spherical)
    switch (nearQuery.centroid->crs) {
        case FLAT:
            *keyBuilder << "fl";
            break;
        case SPHERE:
            *keyBuilder << "sp";
            break;
        case STRICT_SPHERE:
            *keyBuilder << "ss";
            break;
        case UNSET:
            error() << "unknown CRS type " << (int)nearQuery.centroid->crs
                    << " in point geometry for near query";
            invariant(false);
            break;
    }
}

}  // namespace

//
// Cache-related functions for CanonicalQuery
//

bool PlanCache::shouldCacheQuery(const CanonicalQuery& query) {
    const QueryRequest& qr = query.getQueryRequest();
    const MatchExpression* expr = query.root();

    // Collection scan
    // No sort order requested
    if (qr.getSort().isEmpty() && expr->matchType() == MatchExpression::AND &&
        expr->numChildren() == 0) {
        return false;
    }

    // Hint provided
    if (!qr.getHint().isEmpty()) {
        return false;
    }

    // Min provided
    // Min queries are a special case of hinted queries.
    if (!qr.getMin().isEmpty()) {
        return false;
    }

    // Max provided
    // Similar to min, max queries are a special case of hinted queries.
    if (!qr.getMax().isEmpty()) {
        return false;
    }

    // Explain queries are not-cacheable. This is primarily because of
    // the need to generate current and accurate information in allPlans.
    // If the explain report is generated by the cached plan runner using
    // stale information from the cache for the losing plans, allPlans would
    // simply be wrong.
    if (qr.isExplain()) {
        return false;
    }

    // Tailable cursors won't get cached, just turn into collscans.
    if (query.getQueryRequest().isTailable()) {
        return false;
    }

    // Snapshot is really a hint.
    if (query.getQueryRequest().isSnapshot()) {
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
      collation(entry.collation.getOwned()),
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
         i != plannerData.end();
         ++i) {
        SolutionCacheData* scd = *i;
        delete scd;
    }
}

//
// PlanCacheEntry
//

PlanCacheEntry::PlanCacheEntry(const std::vector<QuerySolution*>& solutions,
                               PlanRankingDecision* why)
    : plannerData(solutions.size()), decision(why) {
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
    entry->collation = collation.getOwned();

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
    return str::stream() << "(query: " << query.toString() << ";sort: " << sort.toString()
                         << ";projection: " << projection.toString()
                         << ";collation: " << collation.toString()
                         << ";solutions: " << plannerData.size() << ")";
}

std::string CachedSolution::toString() const {
    return str::stream() << "key: " << key << '\n';
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
        root->canCombineBounds = canCombineBounds;
    }

    for (std::vector<PlanCacheIndexTree*>::const_iterator it = children.begin();
         it != children.end();
         ++it) {
        PlanCacheIndexTree* clonedChild = (*it)->clone();
        root->children.push_back(clonedChild);
    }
    return root;
}

std::string PlanCacheIndexTree::toString(int indents) const {
    StringBuilder result;
    if (!children.empty()) {
        result << std::string(3 * indents, '-') << "Node\n";
        int newIndent = indents + 1;
        for (std::vector<PlanCacheIndexTree*>::const_iterator it = children.begin();
             it != children.end();
             ++it) {
            result << (*it)->toString(newIndent);
        }
        return result.str();
    } else {
        result << std::string(3 * indents, '-') << "Leaf ";
        if (NULL != entry.get()) {
            result << entry->keyPattern.toString() << ", pos: " << index_pos << ", can combine? "
                   << canCombineBounds;
        }
        result << '\n';
    }
    return result.str();
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
    switch (this->solnType) {
        case WHOLE_IXSCAN_SOLN:
            verify(this->tree.get());
            return str::stream() << "(whole index scan solution: "
                                 << "dir=" << this->wholeIXSolnDir << "; "
                                 << "tree=" << this->tree->toString() << ")";
        case COLLSCAN_SOLN:
            return "(collection scan)";
        case USE_INDEX_TAGS_SOLN:
            verify(this->tree.get());
            return str::stream() << "(index-tagged expression tree: "
                                 << "tree=" << this->tree->toString() << ")";
    }
    MONGO_UNREACHABLE;
}

//
// PlanCache
//

PlanCache::PlanCache() : _cache(internalQueryCacheSize) {}

PlanCache::PlanCache(const std::string& ns) : _cache(internalQueryCacheSize), _ns(ns) {}

PlanCache::~PlanCache() {}

/**
 * Traverses expression tree pre-order.
 * Appends an encoding of each node's match type and path name
 * to the output stream.
 */
void PlanCache::encodeKeyForMatch(const MatchExpression* tree, StringBuilder* keyBuilder) const {
    // Encode match type and path.
    *keyBuilder << encodeMatchType(tree->matchType());

    encodeUserString(tree->path(), keyBuilder);

    // GEO and GEO_NEAR require additional encoding.
    if (MatchExpression::GEO == tree->matchType()) {
        encodeGeoMatchExpression(static_cast<const GeoMatchExpression*>(tree), keyBuilder);
    } else if (MatchExpression::GEO_NEAR == tree->matchType()) {
        encodeGeoNearMatchExpression(static_cast<const GeoNearMatchExpression*>(tree), keyBuilder);
    }

    // Encode indexability.
    const IndexToDiscriminatorMap& discriminators =
        _indexabilityState.getDiscriminators(tree->path());
    if (!discriminators.empty()) {
        *keyBuilder << kEncodeDiscriminatorsBegin;
        // For each discriminator on this path, append the character '0' or '1'.
        for (auto&& indexAndDiscriminatorPair : discriminators) {
            *keyBuilder << indexAndDiscriminatorPair.second.isMatchCompatibleWithIndex(tree);
        }
        *keyBuilder << kEncodeDiscriminatorsEnd;
    }

    // Traverse child nodes.
    // Enclose children in [].
    if (tree->numChildren() > 0) {
        *keyBuilder << kEncodeChildrenBegin;
    }
    // Use comma to separate children encoding.
    for (size_t i = 0; i < tree->numChildren(); ++i) {
        if (i > 0) {
            *keyBuilder << kEncodeChildrenSeparator;
        }
        encodeKeyForMatch(tree->getChild(i), keyBuilder);
    }
    if (tree->numChildren() > 0) {
        *keyBuilder << kEncodeChildrenEnd;
    }
}

/**
 * Encodes sort order into cache key.
 * Sort order is normalized because it provided by
 * QueryRequest.
 */
void PlanCache::encodeKeyForSort(const BSONObj& sortObj, StringBuilder* keyBuilder) const {
    if (sortObj.isEmpty()) {
        return;
    }

    *keyBuilder << kEncodeSortSection;

    BSONObjIterator it(sortObj);
    while (it.more()) {
        BSONElement elt = it.next();
        // $meta text score
        if (QueryRequest::isTextScoreMeta(elt)) {
            *keyBuilder << "t";
        }
        // Ascending
        else if (elt.numberInt() == 1) {
            *keyBuilder << "a";
        }
        // Descending
        else {
            *keyBuilder << "d";
        }
        encodeUserString(elt.fieldName(), keyBuilder);

        // Sort argument separator
        if (it.more()) {
            *keyBuilder << ",";
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
void PlanCache::encodeKeyForProj(const BSONObj& projObj, StringBuilder* keyBuilder) const {
    // Sorts the BSON elements by field name using a map.
    std::map<StringData, BSONElement> elements;

    BSONObjIterator it(projObj);
    while (it.more()) {
        BSONElement elt = it.next();
        StringData fieldName = elt.fieldNameStringData();

        // Internal callers may add $-prefixed fields to the projection. These are not part of a
        // user query, and therefore are not considered part of the cache key.
        if (fieldName[0] == '$') {
            continue;
        }

        elements[fieldName] = elt;
    }

    if (!elements.empty()) {
        *keyBuilder << kEncodeProjectionSection;
    }

    // Read elements in order of field name
    for (std::map<StringData, BSONElement>::const_iterator i = elements.begin();
         i != elements.end();
         ++i) {
        const BSONElement& elt = (*i).second;

        if (elt.isSimpleType()) {
            // For inclusion/exclusion projections, we encode as "i" or "e".
            *keyBuilder << (elt.trueValue() ? "i" : "e");
        } else {
            // For projection operators, we use the verbatim string encoding of the element.
            encodeUserString(elt.toString(false,   // includeFieldName
                                          false),  // full
                             keyBuilder);
        }

        encodeUserString(elt.fieldName(), keyBuilder);
    }
}

Status PlanCache::add(const CanonicalQuery& query,
                      const std::vector<QuerySolution*>& solns,
                      PlanRankingDecision* why) {
    invariant(why);

    if (solns.empty()) {
        return Status(ErrorCodes::BadValue, "no solutions provided");
    }

    if (why->stats.size() != solns.size()) {
        return Status(ErrorCodes::BadValue, "number of stats in decision must match solutions");
    }

    if (why->scores.size() != solns.size()) {
        return Status(ErrorCodes::BadValue, "number of scores in decision must match solutions");
    }

    if (why->candidateOrder.size() != solns.size()) {
        return Status(ErrorCodes::BadValue,
                      "candidate ordering entries in decision must match solutions");
    }

    PlanCacheEntry* entry = new PlanCacheEntry(solns, why);
    const QueryRequest& qr = query.getQueryRequest();
    entry->query = qr.getFilter().getOwned();
    entry->sort = qr.getSort().getOwned();
    if (query.getCollator()) {
        entry->collation = query.getCollator()->getSpec().toBSON();
    }

    // Strip projections on $-prefixed fields, as these are added by internal callers of the query
    // system and are not considered part of the user projection.
    BSONObjBuilder projBuilder;
    for (auto elem : qr.getProj()) {
        if (elem.fieldName()[0] == '$') {
            continue;
        }
        projBuilder.append(elem);
    }
    entry->projection = projBuilder.obj();

    stdx::lock_guard<stdx::mutex> cacheLock(_cacheMutex);
    std::unique_ptr<PlanCacheEntry> evictedEntry = _cache.add(computeKey(query), entry);

    if (NULL != evictedEntry.get()) {
        LOG(1) << _ns << ": plan cache maximum size exceeded - "
               << "removed least recently used entry " << evictedEntry->toString();
    }

    return Status::OK();
}

Status PlanCache::get(const CanonicalQuery& query, CachedSolution** crOut) const {
    PlanCacheKey key = computeKey(query);
    verify(crOut);

    stdx::lock_guard<stdx::mutex> cacheLock(_cacheMutex);
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
    std::unique_ptr<PlanCacheEntryFeedback> autoFeedback(feedback);
    PlanCacheKey ck = computeKey(cq);

    stdx::lock_guard<stdx::mutex> cacheLock(_cacheMutex);
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
    stdx::lock_guard<stdx::mutex> cacheLock(_cacheMutex);
    return _cache.remove(computeKey(canonicalQuery));
}

void PlanCache::clear() {
    stdx::lock_guard<stdx::mutex> cacheLock(_cacheMutex);
    _cache.clear();
    _writeOperations.store(0);
}

PlanCacheKey PlanCache::computeKey(const CanonicalQuery& cq) const {
    StringBuilder keyBuilder;
    encodeKeyForMatch(cq.root(), &keyBuilder);
    encodeKeyForSort(cq.getQueryRequest().getSort(), &keyBuilder);
    encodeKeyForProj(cq.getQueryRequest().getProj(), &keyBuilder);
    return keyBuilder.str();
}

Status PlanCache::getEntry(const CanonicalQuery& query, PlanCacheEntry** entryOut) const {
    PlanCacheKey key = computeKey(query);
    verify(entryOut);

    stdx::lock_guard<stdx::mutex> cacheLock(_cacheMutex);
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
    stdx::lock_guard<stdx::mutex> cacheLock(_cacheMutex);
    std::vector<PlanCacheEntry*> entries;
    typedef std::list<std::pair<PlanCacheKey, PlanCacheEntry*>>::const_iterator ConstIterator;
    for (ConstIterator i = _cache.begin(); i != _cache.end(); i++) {
        PlanCacheEntry* entry = i->second;
        entries.push_back(entry->clone());
    }

    return entries;
}

bool PlanCache::contains(const CanonicalQuery& cq) const {
    stdx::lock_guard<stdx::mutex> cacheLock(_cacheMutex);
    return _cache.hasKey(computeKey(cq));
}

size_t PlanCache::size() const {
    stdx::lock_guard<stdx::mutex> cacheLock(_cacheMutex);
    return _cache.size();
}

void PlanCache::notifyOfIndexEntries(const std::vector<IndexEntry>& indexEntries) {
    _indexabilityState.updateDiscriminators(indexEntries);
}

}  // namespace mongo
