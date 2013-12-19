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

#include "mongo/db/query/canonical_query.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_parser.h"

namespace mongo {

    // static
    Status CanonicalQuery::canonicalize(const QueryMessage& qm, CanonicalQuery** out) {
        LiteParsedQuery* lpq;
        Status parseStatus = LiteParsedQuery::make(qm, &lpq);
        if (!parseStatus.isOK()) { return parseStatus; }

        auto_ptr<CanonicalQuery> cq(new CanonicalQuery());
        Status initStatus = cq->init(lpq);
        if (!initStatus.isOK()) { return initStatus; }

        *out = cq.release();
        return Status::OK();
    }

    // static
    Status CanonicalQuery::canonicalize(const string& ns, const BSONObj& query,
                                        CanonicalQuery** out) {
        BSONObj emptyObj;
        return CanonicalQuery::canonicalize(ns, query, emptyObj, emptyObj, 0, 0, out);
    }

    // static
    Status CanonicalQuery::canonicalize(const string& ns, const BSONObj& query,
                                        long long skip, long long limit,
                                        CanonicalQuery** out) {
        BSONObj emptyObj;
        return CanonicalQuery::canonicalize(ns, query, emptyObj, emptyObj, skip, limit, out);
    }

    // static
    Status CanonicalQuery::canonicalize(const string& ns, const BSONObj& query,
                                        const BSONObj& sort, const BSONObj& proj,
                                        CanonicalQuery** out) {
        return CanonicalQuery::canonicalize(ns, query, sort, proj, 0, 0, out);
    }

    // static
    Status CanonicalQuery::canonicalize(const string& ns, const BSONObj& query,
                                        const BSONObj& sort, const BSONObj& proj,
                                        long long skip, long long limit,
                                        CanonicalQuery** out) {
        BSONObj emptyObj;
        return CanonicalQuery::canonicalize(ns, query, sort, proj, skip, limit, emptyObj, out);
    }

    // static
    Status CanonicalQuery::canonicalize(const string& ns, const BSONObj& query,
                                        const BSONObj& sort, const BSONObj& proj,
                                        long long skip, long long limit,
                                        const BSONObj& hint,
                                        CanonicalQuery** out) {
        BSONObj emptyObj;
        return CanonicalQuery::canonicalize(ns, query, sort, proj, skip, limit, hint,
                                            emptyObj, emptyObj, false, out);
    }

    // static
    Status CanonicalQuery::canonicalize(const string& ns, const BSONObj& query,
                                        const BSONObj& sort, const BSONObj& proj,
                                        long long skip, long long limit,
                                        const BSONObj& hint,
                                        const BSONObj& minObj, const BSONObj& maxObj,
                                        bool snapshot, CanonicalQuery** out) {
        LiteParsedQuery* lpq;
        // Pass empty sort and projection.
        BSONObj emptyObj;
        Status parseStatus = LiteParsedQuery::make(ns, skip, limit, 0, query, proj, sort,
                                                   hint, minObj, maxObj, snapshot, &lpq);
        if (!parseStatus.isOK()) { return parseStatus; }

        auto_ptr<CanonicalQuery> cq(new CanonicalQuery());
        Status initStatus = cq->init(lpq);
        if (!initStatus.isOK()) { return initStatus; }

        *out = cq.release();
        return Status::OK();
    }

    /**
     * Returns the normalized version of the subtree rooted at 'root'.
     */
    MatchExpression* normalizeTree(MatchExpression* root) {
        // root->isLogical() is true now.  We care about AND and OR.  Negations currently scare us.
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

        return root;
    }

    // TODO: Should this really live in the parsing?  Or elsewhere?
    Status argsValid(MatchExpression* root) {
        MatchExpression::MatchType type = root->matchType();

        if (MatchExpression::GT == type || MatchExpression::GTE == type
            || MatchExpression::LT == type || MatchExpression::LTE == type) {

            ComparisonMatchExpression* cme = static_cast<ComparisonMatchExpression*>(root);
            BSONElement data = cme->getData();
            if (RegEx == data.type()) {
                return Status(ErrorCodes::BadValue,
                              "Can't have RegEx as arg to pred " + cme->toString());
            }
        }

        for (size_t i = 0; i < root->numChildren(); ++i) {
            Status s = argsValid(root->getChild(i));
            if (!s.isOK()) {
                return s;
            }
        }

        return Status::OK();
    }

    size_t countNodes(MatchExpression* root, MatchExpression::MatchType type) {
        size_t sum = 0;
        if (type == root->matchType()) {
            sum = 1;
        }
        for (size_t i = 0; i < root->numChildren(); ++i) {
            sum += countNodes(root->getChild(i), type);
        }
        return sum;
    }

    // TODO: Move this to query_validator.cpp
    Status isValid(MatchExpression* root) {
        // TODO: This should really be done as part of type checking in the parser.
        Status argStatus = argsValid(root);
        if (!argStatus.isOK()) {
            return argStatus;
        }

        // Analysis below should be done after squashing the tree to make it clearer.

        // TODO: We want $text in root level or within rooted AND for consistent text score
        // availability.

        // There can only be one NEAR.  If there is a NEAR, it must be either the root or the root
        // must be an AND and its child must be a NEAR.
        size_t numGeoNear = countNodes(root, MatchExpression::GEO_NEAR);

        if (0 == numGeoNear) {
            return Status::OK();
        }

        if (numGeoNear > 1) {
            return Status(ErrorCodes::BadValue, "Too many geoNear expressions");
        }

        if (MatchExpression::GEO_NEAR == root->matchType()) {
            return Status::OK();
        }
        else if (MatchExpression::AND == root->matchType()) {
            for (size_t i = 0; i < root->numChildren(); ++i) {
                if (MatchExpression::GEO_NEAR == root->getChild(i)->matchType()) {
                    return Status::OK();
                }
            }
        }

        return Status(ErrorCodes::BadValue, "geoNear must be top-level expr");
    }

    Status CanonicalQuery::init(LiteParsedQuery* lpq) {
        _pq.reset(lpq);

        // Build a parse tree from the BSONObj in the parsed query.
        StatusWithMatchExpression swme = MatchExpressionParser::parse(_pq->getFilter());
        if (!swme.isOK()) { return swme.getStatus(); }

        MatchExpression* root = swme.getValue();
        root = normalizeTree(root);
        Status validStatus = isValid(root);
        if (!validStatus.isOK()) {
            return validStatus;
        }

        _root.reset(root);

        // Validate the projection if there is one.
        if (!_pq->getProj().isEmpty()) {
            ParsedProjection* pp;
            Status projStatus = ParsedProjection::make(_pq->getProj(), root, &pp);
            if (!projStatus.isOK()) {
                return projStatus;
            }
            _proj.reset(pp);
        }

        return Status::OK();
    }

    string CanonicalQuery::toString() const {
        stringstream ss;
        ss << "ns=" << _pq->ns() << " limit=" << _pq->getNumToReturn()
           << " skip=" << _pq->getSkip() << endl;
        // The expression tree puts an endl on for us.
        ss << "Tree: " << _root->toString();
        ss << "Sort: " << _pq->getSort().toString() << endl;
        ss << "Proj: " << _pq->getProj().toString() << endl;
        return ss.str();
    }

}  // namespace mongo
