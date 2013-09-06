/**
*    Copyright (C) 2011 10gen Inc.
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

#include "mongo/pch.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/matcher.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"

namespace mongo {

    const char DocumentSourceMatch::matchName[] = "$match";

    const char *DocumentSourceMatch::getSourceName() const {
        return matchName;
    }

    Value DocumentSourceMatch::serialize(bool explain) const {
        return Value(DOC(getSourceName() << Document(*matcher.getQuery())));
    }

    bool DocumentSourceMatch::accept(const Document& input) const {

        /*
          The matcher only takes BSON documents, so we have to make one.

          LATER
          We could optimize this by making a document with only the
          fields referenced by the Matcher.  We could do this by looking inside
          the Matcher's BSON before it is created, and recording those.  The
          easiest implementation might be to hold onto an ExpressionDocument
          in here, and give that pDocument to create the created subset of
          fields, and then convert that instead.
        */

        return matcher.matches(input.toBson());
    }

    static void uassertNoDisallowedClauses(BSONObj query) {
        BSONForEach(e, query) {
            // can't use the Matcher API because this would segfault the constructor
            uassert(16395, "$where is not allowed inside of a $match aggregation expression",
                    ! str::equals(e.fieldName(), "$where"));
            // geo breaks if it is not the first portion of the pipeline
            uassert(16424, "$near is not allowed inside of a $match aggregation expression",
                    ! str::equals(e.fieldName(), "$near"));
            uassert(16426, "$nearSphere is not allowed inside of a $match aggregation expression",
                    ! str::equals(e.fieldName(), "$nearSphere"));
            if (e.isABSONObj())
                uassertNoDisallowedClauses(e.Obj());
        }
    }

    intrusive_ptr<DocumentSource> DocumentSourceMatch::createFromBson(
        BSONElement *pBsonElement,
        const intrusive_ptr<ExpressionContext> &pExpCtx) {
        uassert(15959, "the match filter must be an expression in an object",
                pBsonElement->type() == Object);

        uassertNoDisallowedClauses(pBsonElement->Obj());

        intrusive_ptr<DocumentSourceMatch> pMatcher(
            new DocumentSourceMatch(pBsonElement->Obj(), pExpCtx));

        return pMatcher;
    }

    void DocumentSourceMatch::toMatcherBson(BSONObjBuilder *pBuilder) const {
        const BSONObj *pQuery = matcher.getQuery();
        pBuilder->appendElements(*pQuery);
    }

    DocumentSourceMatch::DocumentSourceMatch(
        const BSONObj &query,
        const intrusive_ptr<ExpressionContext> &pExpCtx):
        DocumentSourceFilterBase(pExpCtx),
        matcher(query.getOwned()) {
    }
}
