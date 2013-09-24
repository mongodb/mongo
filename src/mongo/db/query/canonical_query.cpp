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
#include "mongo/db/query/projection_parser.h"

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
        LiteParsedQuery* lpq;
        // Pass empty sort and projection.
        BSONObj emptyObj;
        Status parseStatus = LiteParsedQuery::make(ns, 0, 0, 0, query, emptyObj, emptyObj, &lpq);
        if (!parseStatus.isOK()) { return parseStatus; }

        auto_ptr<CanonicalQuery> cq(new CanonicalQuery());
        Status initStatus = cq->init(lpq);
        if (!initStatus.isOK()) { return initStatus; }

        *out = cq.release();
        return Status::OK();
    }

    // static
    Status CanonicalQuery::canonicalize(const string& ns, const BSONObj& query,
                                        const BSONObj& sort, const BSONObj& proj,
                                        CanonicalQuery** out) {
        LiteParsedQuery* lpq;
        // Pass empty sort and projection.
        BSONObj emptyObj;
        Status parseStatus = LiteParsedQuery::make(ns, 0, 0, 0, query, proj, sort, &lpq);
        if (!parseStatus.isOK()) { return parseStatus; }

        auto_ptr<CanonicalQuery> cq(new CanonicalQuery());
        Status initStatus = cq->init(lpq);
        if (!initStatus.isOK()) { return initStatus; }

        *out = cq.release();
        return Status::OK();
    }

    void normalizeTree(MatchExpression* root) {
        // root->isLogical() is true now.  We care about AND and OR.  Negations currently scare us.
        if (MatchExpression::AND == root->matchType() || MatchExpression::OR == root->matchType()) {
            // We could have AND of AND of AND.  Make sure we clean up our children before merging
            // them.
            // UNITTEST 11738048
            for (size_t i = 0; i < root->numChildren(); ++i) {
                normalizeTree(root->getChild(i));
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

            root->getChildVector()->insert(root->getChildVector()->end(), absorbedChildren.begin(), absorbedChildren.end());
        }
    }


    Status CanonicalQuery::init(LiteParsedQuery* lpq) {
        _pq.reset(lpq);

        // Build a parse tree from the BSONObj in the parsed query.
        StatusWithMatchExpression swme = MatchExpressionParser::parse(_pq->getFilter());
        if (!swme.isOK()) { return swme.getStatus(); }

        MatchExpression* root = swme.getValue();
        normalizeTree(root);
        _root.reset(root);

        if (!_pq->getProj().isEmpty()) {
            ParsedProjection* proj;
            Status projStatus = ProjectionParser::parseFindSyntax(_pq->getProj(), &proj);
            if (!projStatus.isOK()) {
                return projStatus;
            }
            _proj.reset(proj);
        }

        return Status::OK();
    }

    string CanonicalQuery::toString() const {
        return "ns = " + _pq->ns()
               + "\nTree: " + _root->toString()
               + "\nSort: " + _pq->getSort().toString();
    }

}  // namespace mongo
