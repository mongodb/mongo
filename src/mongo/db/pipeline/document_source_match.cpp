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
*/

#include "mongo/pch.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/matcher.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"

namespace mongo {

    const char DocumentSourceMatch::matchName[] = "$match";

    DocumentSourceMatch::~DocumentSourceMatch() {
    }

    const char *DocumentSourceMatch::getSourceName() const {
        return matchName;
    }

    void DocumentSourceMatch::sourceToBson(
        BSONObjBuilder *pBuilder, bool explain) const {
        const BSONObj *pQuery = matcher.getQuery();
        pBuilder->append(matchName, *pQuery);
    }

    bool DocumentSourceMatch::accept(const Document& pDocument) const {

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
        BSONObjBuilder objBuilder;
        pDocument->toBson(&objBuilder);
        BSONObj obj(objBuilder.done());

        return matcher.matches(obj);
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
