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

#include "mongo/db/query/canonical_query.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/util/log.h"


namespace {

    using boost::shared_ptr;
    using std::auto_ptr;
    using std::string;
    using namespace mongo;

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
    void encodeUserString(StringData s, mongoutils::str::stream* os) {
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

    void encodePlanCacheKeyTree(const MatchExpression* tree, mongoutils::str::stream* os);

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
        mongoutils::str::stream ssLeft, ssRight;
        encodePlanCacheKeyTree(lhs, &ssLeft);
        encodePlanCacheKeyTree(rhs, &ssRight);
        string strLeft(ssLeft);
        string strRight(ssRight);
        return strLeft < strRight;
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
    void encodeGeoMatchExpression(const GeoMatchExpression* tree, mongoutils::str::stream* os) {
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
                                      mongoutils::str::stream* os) {
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
    void encodePlanCacheKeyTree(const MatchExpression* tree, mongoutils::str::stream* os) {
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
    void encodePlanCacheKeySort(const BSONObj& sortObj, mongoutils::str::stream* os) {
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
    void encodePlanCacheKeyProj(const BSONObj& projObj, mongoutils::str::stream* os) {
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

} // namespace

namespace mongo {

    //
    // These all punt to the many-argumented canonicalize below.
    //

    // static
    Status CanonicalQuery::canonicalize(const string& ns,
                                        const BSONObj& query,
                                        CanonicalQuery** out,
                                        const MatchExpressionParser::WhereCallback& whereCallback) {
        const BSONObj emptyObj;
        return CanonicalQuery::canonicalize(
                                    ns, query, emptyObj, emptyObj, 0, 0, out, whereCallback);
    }

    // static
    Status CanonicalQuery::canonicalize(const std::string& ns,
                                        const BSONObj& query,
                                        bool explain,
                                        CanonicalQuery** out,
                                        const MatchExpressionParser::WhereCallback& whereCallback) {
        const BSONObj emptyObj;
        return CanonicalQuery::canonicalize(ns,
                                            query,
                                            emptyObj, // sort
                                            emptyObj, // projection
                                            0, // skip
                                            0, // limit
                                            emptyObj, // hint
                                            emptyObj, // min
                                            emptyObj, // max
                                            false, // snapshot
                                            explain,
                                            out,
                                            whereCallback);
    }

    // static
    Status CanonicalQuery::canonicalize(const string& ns,
                                        const BSONObj& query,
                                        long long skip,
                                        long long limit,
                                        CanonicalQuery** out,
                                        const MatchExpressionParser::WhereCallback& whereCallback) {
        const BSONObj emptyObj;
        return CanonicalQuery::canonicalize(ns, 
                                            query, 
                                            emptyObj, 
                                            emptyObj, 
                                            skip, 
                                            limit, 
                                            out,
                                            whereCallback);
    }

    // static
    Status CanonicalQuery::canonicalize(const string& ns,
                                        const BSONObj& query,
                                        const BSONObj& sort,
                                        const BSONObj& proj,
                                        CanonicalQuery** out,
                                        const MatchExpressionParser::WhereCallback& whereCallback) {
        return CanonicalQuery::canonicalize(ns, query, sort, proj, 0, 0, out, whereCallback);
    }

    // static
    Status CanonicalQuery::canonicalize(const string& ns,
                                        const BSONObj& query,
                                        const BSONObj& sort,
                                        const BSONObj& proj,
                                        long long skip,
                                        long long limit,
                                        CanonicalQuery** out,
                                        const MatchExpressionParser::WhereCallback& whereCallback) {
        const BSONObj emptyObj;
        return CanonicalQuery::canonicalize(
                            ns, query, sort, proj, skip, limit, emptyObj, out, whereCallback);
    }

    // static
    Status CanonicalQuery::canonicalize(const string& ns,
                                        const BSONObj& query,
                                        const BSONObj& sort,
                                        const BSONObj& proj,
                                        long long skip,
                                        long long limit,
                                        const BSONObj& hint,
                                        CanonicalQuery** out,
                                        const MatchExpressionParser::WhereCallback& whereCallback) {
        const BSONObj emptyObj;
        return CanonicalQuery::canonicalize(ns, query, sort, proj, skip, limit, hint,
                                            emptyObj, emptyObj,
                                            false, // snapshot
                                            false, // explain
                                            out,
                                            whereCallback);
    }

    //
    // These actually call init() on the CQ.
    //

    // static
    Status CanonicalQuery::canonicalize(const QueryMessage& qm,
                                        CanonicalQuery** out,
                                        const MatchExpressionParser::WhereCallback& whereCallback) {
        // Make LiteParsedQuery.
        LiteParsedQuery* lpq;
        Status parseStatus = LiteParsedQuery::make(qm, &lpq);
        if (!parseStatus.isOK()) { return parseStatus; }

        return CanonicalQuery::canonicalize(lpq, out, whereCallback);
    }

    // static
    Status CanonicalQuery::canonicalize(LiteParsedQuery* lpq,
                                        CanonicalQuery** out,
                                        const MatchExpressionParser::WhereCallback& whereCallback) {
        auto_ptr<LiteParsedQuery> autoLpq(lpq);

        // Make MatchExpression.
        StatusWithMatchExpression swme = MatchExpressionParser::parse(autoLpq->getFilter(),
                                                                      whereCallback);
        if (!swme.isOK()) {
            return swme.getStatus();
        }

        // Make the CQ we'll hopefully return.
        auto_ptr<CanonicalQuery> cq(new CanonicalQuery());
        // Takes ownership of lpq and the MatchExpression* in swme.
        Status initStatus = cq->init(autoLpq.release(), whereCallback, swme.getValue());

        if (!initStatus.isOK()) { return initStatus; }
        *out = cq.release();
        return Status::OK();
    }

    // static
    Status CanonicalQuery::canonicalize(const CanonicalQuery& baseQuery,
                                        MatchExpression* root,
                                        CanonicalQuery** out,
                                        const MatchExpressionParser::WhereCallback& whereCallback) {

        LiteParsedQuery* lpq;

        // Pass empty sort and projection.
        BSONObj emptyObj;
        // 0, 0, 0 is 'ntoskip', 'ntoreturn', and 'queryoptions'
        // false, false is 'snapshot' and 'explain'
        Status parseStatus = LiteParsedQuery::make(baseQuery.ns(),
                                                   0, 0, 0,
                                                   baseQuery.getParsed().getFilter(),
                                                   baseQuery.getParsed().getProj(),
                                                   baseQuery.getParsed().getSort(),
                                                   emptyObj, emptyObj, emptyObj,
                                                   false, false, &lpq);
        if (!parseStatus.isOK()) {
            return parseStatus;
        }

        // Make the CQ we'll hopefully return.
        auto_ptr<CanonicalQuery> cq(new CanonicalQuery());
        Status initStatus = cq->init(lpq, whereCallback, root->shallowClone());

        if (!initStatus.isOK()) { return initStatus; }
        *out = cq.release();
        return Status::OK();
    }

    // static
    Status CanonicalQuery::canonicalize(const string& ns,
                                        const BSONObj& query,
                                        const BSONObj& sort,
                                        const BSONObj& proj,
                                        long long skip,
                                        long long limit,
                                        const BSONObj& hint,
                                        const BSONObj& minObj,
                                        const BSONObj& maxObj,
                                        bool snapshot,
                                        bool explain,
                                        CanonicalQuery** out,
                                        const MatchExpressionParser::WhereCallback& whereCallback) {
        LiteParsedQuery* lpqRaw;
        // Pass empty sort and projection.
        BSONObj emptyObj;
        Status parseStatus = LiteParsedQuery::make(ns, skip, limit, 0, query, proj, sort,
                                                   hint, minObj, maxObj, snapshot, explain,
                                                   &lpqRaw);
        if (!parseStatus.isOK()) {
            return parseStatus;
        }
        auto_ptr<LiteParsedQuery> lpq(lpqRaw);

        // Build a parse tree from the BSONObj in the parsed query.
        StatusWithMatchExpression swme = 
                            MatchExpressionParser::parse(lpq->getFilter(), whereCallback);
        if (!swme.isOK()) {
            return swme.getStatus();
        }

        // Make the CQ we'll hopefully return.
        auto_ptr<CanonicalQuery> cq(new CanonicalQuery());
        // Takes ownership of lpq and the MatchExpression* in swme.
        Status initStatus = cq->init(lpq.release(), whereCallback, swme.getValue());

        if (!initStatus.isOK()) { return initStatus; }
        *out = cq.release();
        return Status::OK();
    }

    Status CanonicalQuery::init(LiteParsedQuery* lpq,
                                const MatchExpressionParser::WhereCallback& whereCallback,
                                MatchExpression* root) {
        _isForWrite = false;
        _pq.reset(lpq);

        // Normalize, sort and validate tree.
        root = normalizeTree(root);

        sortTree(root);
        _root.reset(root);
        Status validStatus = isValid(root, *_pq);
        if (!validStatus.isOK()) {
            return validStatus;
        }

        this->generateCacheKey();

        // Validate the projection if there is one.
        if (!_pq->getProj().isEmpty()) {
            ParsedProjection* pp;
            Status projStatus = 
                ParsedProjection::make(_pq->getProj(), _root.get(), &pp, whereCallback);
            if (!projStatus.isOK()) {
                return projStatus;
            }
            _proj.reset(pp);
        }

        return Status::OK();
    }


    // static
    bool CanonicalQuery::isSimpleIdQuery(const BSONObj& query) {
        bool hasID = false;

        BSONObjIterator it(query);
        while (it.more()) {
            BSONElement elt = it.next();
            if (mongoutils::str::equals("_id", elt.fieldName() ) ) {
                // Verify that the query on _id is a simple equality.
                hasID = true;

                if (elt.type() == Object) {
                    // If the value is an object, it can't have a query operator
                    // (must be a literal object match).
                    if (elt.Obj().firstElementFieldName()[0] == '$') {
                        return false;
                    }
                }
                else if (!elt.isSimpleType() && BinData != elt.type()) {
                    // The _id fild cannot be something like { _id : { $gt : ...
                    // But it can be BinData.
                    return false;
                }
            }
            else if (elt.fieldName()[0] == '$' &&
                     (mongoutils::str::equals("$isolated", elt.fieldName())||
                      mongoutils::str::equals("$atomic", elt.fieldName()))) {
                // ok, passthrough
            }
            else {
                // If the field is not _id, it must be $isolated/$atomic.
                return false;
            }
        }

        return hasID;
    }

    const PlanCacheKey& CanonicalQuery::getPlanCacheKey() const {
        return _cacheKey;
    }

    void CanonicalQuery::generateCacheKey(void) {
        mongoutils::str::stream ss;
        encodePlanCacheKeyTree(_root.get(), &ss);
        encodePlanCacheKeySort(_pq->getSort(), &ss);
        encodePlanCacheKeyProj(_pq->getProj(), &ss);
        _cacheKey = ss;
    }

    // static
    MatchExpression* CanonicalQuery::normalizeTree(MatchExpression* root) {
        // root->isLogical() is true now.  We care about AND, OR, and NOT. NOR currently scares us.
        if (MatchExpression::AND == root->matchType() || MatchExpression::OR == root->matchType()) {
            // We could have AND of AND of AND.  Make sure we clean up our children before merging
            // them.
            // UNITTEST 11738048
            for (size_t i = 0; i < root->getChildVector()->size(); ++i) {
                (*root->getChildVector())[i] = normalizeTree(root->getChild(i));
            }

            // If any of our children are of the same logical operator that we are, we remove the
            // child's children and append them to ourselves after we examine all children.
            vector<MatchExpression*> absorbedChildren;

            for (size_t i = 0; i < root->numChildren();) {
                MatchExpression* child = root->getChild(i);
                if (child->matchType() == root->matchType()) {
                    // AND of an AND or OR of an OR.  Absorb child's children into ourself.
                    for (size_t j = 0; j < child->numChildren(); ++j) {
                        absorbedChildren.push_back(child->getChild(j));
                    }
                    // TODO(opt): this is possibly n^2-ish
                    root->getChildVector()->erase(root->getChildVector()->begin() + i);
                    child->getChildVector()->clear();
                    // Note that this only works because we cleared the child's children
                    delete child;
                    // Don't increment 'i' as the current child 'i' used to be child 'i+1'
                }
                else {
                    ++i;
                }
            }

            root->getChildVector()->insert(root->getChildVector()->end(),
                                           absorbedChildren.begin(),
                                           absorbedChildren.end());

            // AND of 1 thing is the thing, OR of 1 thing is the thing.
            if (1 == root->numChildren()) {
                MatchExpression* ret = root->getChild(0);
                root->getChildVector()->clear();
                delete root;
                return ret;
            }
        }
        else if (MatchExpression::NOT == root->matchType()) {
            // Normalize the rest of the tree hanging off this NOT node.
            NotMatchExpression* nme = static_cast<NotMatchExpression*>(root);
            MatchExpression* child = nme->releaseChild();
            // normalizeTree(...) takes ownership of 'child', and then
            // transfers ownership of its return value to 'nme'.
            nme->resetChild(normalizeTree(child));
        }
        else if (MatchExpression::ELEM_MATCH_VALUE == root->matchType()) {
            // Just normalize our children.
            for (size_t i = 0; i < root->getChildVector()->size(); ++i) {
                (*root->getChildVector())[i] = normalizeTree(root->getChild(i));
            }
        }

        return root;
    }

    // static
    void CanonicalQuery::sortTree(MatchExpression* tree) {
        for (size_t i = 0; i < tree->numChildren(); ++i) {
            sortTree(tree->getChild(i));
        }
        std::vector<MatchExpression*>* children = tree->getChildVector();
        if (NULL != children) {
            std::sort(children->begin(), children->end(), OperatorAndFieldNameComparison);
        }
    }

    // static
    size_t CanonicalQuery::countNodes(const MatchExpression* root,
                                      MatchExpression::MatchType type) {
        size_t sum = 0;
        if (type == root->matchType()) {
            sum = 1;
        }
        for (size_t i = 0; i < root->numChildren(); ++i) {
            sum += countNodes(root->getChild(i), type);
        }
        return sum;
    }

    /**
     * Does 'root' have a subtree of type 'subtreeType' with a node of type 'childType' inside?
     */
    bool hasNodeInSubtree(MatchExpression* root, MatchExpression::MatchType childType,
                          MatchExpression::MatchType subtreeType) {
        if (subtreeType == root->matchType()) {
            return QueryPlannerCommon::hasNode(root, childType);
        }
        for (size_t i = 0; i < root->numChildren(); ++i) {
            if (hasNodeInSubtree(root->getChild(i), childType, subtreeType)) {
                return true;
            }
        }
        return false;
    }

    // static
    Status CanonicalQuery::isValid(MatchExpression* root, const LiteParsedQuery& parsed) {
        // Analysis below should be done after squashing the tree to make it clearer.

        // There can only be one TEXT.  If there is a TEXT, it cannot appear inside a NOR.
        //
        // Note that the query grammar (as enforced by the MatchExpression parser) forbids TEXT
        // inside of value-expression clauses like NOT, so we don't check those here.
        size_t numText = countNodes(root, MatchExpression::TEXT);
        if (numText > 1) {
            return Status(ErrorCodes::BadValue, "Too many text expressions");
        }
        else if (1 == numText) {
            if (hasNodeInSubtree(root, MatchExpression::TEXT, MatchExpression::NOR)) {
                return Status(ErrorCodes::BadValue, "text expression not allowed in nor");
            }
        }

        // There can only be one NEAR.  If there is a NEAR, it must be either the root or the root
        // must be an AND and its child must be a NEAR.
        size_t numGeoNear = countNodes(root, MatchExpression::GEO_NEAR);
        if (numGeoNear > 1) {
            return Status(ErrorCodes::BadValue, "Too many geoNear expressions");
        }
        else if (1 == numGeoNear) {
            bool topLevel = false;
            if (MatchExpression::GEO_NEAR == root->matchType()) {
                topLevel = true;
            }
            else if (MatchExpression::AND == root->matchType()) {
                for (size_t i = 0; i < root->numChildren(); ++i) {
                    if (MatchExpression::GEO_NEAR == root->getChild(i)->matchType()) {
                        topLevel = true;
                        break;
                    }
                }
            }
            if (!topLevel) {
                return Status(ErrorCodes::BadValue, "geoNear must be top-level expr");
            }
        }

        // NEAR cannot have a $natural sort or $natural hint.
        if (numGeoNear > 0) {
            BSONObj sortObj = parsed.getSort();
            if (!sortObj["$natural"].eoo()) {
                return Status(ErrorCodes::BadValue,
                              "geoNear expression not allowed with $natural sort order");
            }

            BSONObj hintObj = parsed.getHint();
            if (!hintObj["$natural"].eoo()) {
                return Status(ErrorCodes::BadValue,
                              "geoNear expression not allowed with $natural hint");
            }
        }

        // TEXT and NEAR cannot both be in the query.
        if (numText > 0 && numGeoNear > 0) {
            return Status(ErrorCodes::BadValue, "text and geoNear not allowed in same query");
        }

        // TEXT and {$natural: ...} sort order cannot both be in the query.
        if (numText > 0) {
            const BSONObj& sortObj = parsed.getSort();
            BSONObjIterator it(sortObj);
            while (it.more()) {
                BSONElement elt = it.next();
                if (mongoutils::str::equals("$natural", elt.fieldName())) {
                    return Status(ErrorCodes::BadValue,
                                  "text expression not allowed with $natural sort order");
                }
            }
        }

        // TEXT and hint cannot both be in the query.
        if (numText > 0 && !parsed.getHint().isEmpty()) {
            return Status(ErrorCodes::BadValue, "text and hint not allowed in same query");
        }

        // TEXT and snapshot cannot both be in the query.
        if (numText > 0 && parsed.isSnapshot()) {
            return Status(ErrorCodes::BadValue, "text and snapshot not allowed in same query");
        }

        return Status::OK();
    }

    // static
    // XXX TODO: This does not belong here at all.
    MatchExpression* CanonicalQuery::logicalRewrite(MatchExpression* tree) {
        // Only thing we do is pull an OR up at the root.
        if (MatchExpression::AND != tree->matchType()) {
            return tree;
        }

        // We want to bail out ASAP if we have nothing to do here.
        size_t numOrs = 0;
        for (size_t i = 0; i < tree->numChildren(); ++i) {
            if (MatchExpression::OR == tree->getChild(i)->matchType()) {
                ++numOrs;
            }
        }

        // Only do this for one OR right now.
        if (1 != numOrs) {
            return tree;
        }

        // Detach the OR from the root.
        invariant(NULL != tree->getChildVector());
        vector<MatchExpression*>& rootChildren = *tree->getChildVector();
        MatchExpression* orChild = NULL;
        for (size_t i = 0; i < rootChildren.size(); ++i) {
            if (MatchExpression::OR == rootChildren[i]->matchType()) {
                orChild = rootChildren[i];
                rootChildren.erase(rootChildren.begin() + i);
                break;
            }
        }

        // AND the existing root with each or child.
        invariant(NULL != orChild);
        invariant(NULL != orChild->getChildVector());
        vector<MatchExpression*>& orChildren = *orChild->getChildVector();
        for (size_t i = 0; i < orChildren.size(); ++i) {
            AndMatchExpression* ama = new AndMatchExpression();
            ama->add(orChildren[i]);
            ama->add(tree->shallowClone());
            orChildren[i] = ama;
        }
        delete tree;

        // Clean up any consequences from this tomfoolery.
        return normalizeTree(orChild);
    }

    std::string CanonicalQuery::toString() const {
        mongoutils::str::stream ss;
        ss << "ns=" << _pq->ns() << " limit=" << _pq->getNumToReturn()
           << " skip=" << _pq->getSkip() << '\n';
        // The expression tree puts an endl on for us.
        ss << "Tree: " << _root->toString();
        ss << "Sort: " << _pq->getSort().toString() << '\n';
        ss << "Proj: " << _pq->getProj().toString() << '\n';
        return ss;
    }

    std::string CanonicalQuery::toStringShort() const {
        mongoutils::str::stream ss;
        ss << "query: " << _pq->getFilter().toString()
           << " sort: " << _pq->getSort().toString()
           << " projection: " << _pq->getProj().toString()
           << " skip: " << _pq->getSkip()
           << " limit: " << _pq->getNumToReturn();
        return ss;
    }

}  // namespace mongo
