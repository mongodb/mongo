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
#include <sstream>
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/qlog.h"

namespace {

    using std::ostream;
    using std::string;
    using std::stringstream;
    using namespace mongo;

    /**
     * Comparator for MatchExpression nodes. nodes by:
     * 1) operator type (MatchExpression::MatchType)
     * 2) path name (MatchExpression::path())
     *
     * XXX: Do we need to a third item in the tuple to break ties?
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
        return lhsPath < rhsPath;
    }

    /**
     * Traverses expression tree post-order.
     * Sorts children at each non-leaf node by (MatchType, path())
     */
    void sortTree(MatchExpression* tree) {
        for (size_t i = 0; i < tree->numChildren(); ++i) {
            sortTree(tree->getChild(i));
        }
        std::vector<MatchExpression*>* children = tree->getChildVector();
        if (NULL != children) {
            std::sort(children->begin(), children->end(), OperatorAndFieldNameComparison);
        }
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
        sortTree(queryOut->root());
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
