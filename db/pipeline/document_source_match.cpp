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

#include "pch.h"

#include "db/pipeline/document_source.h"

#include "db/jsobj.h"
#include "db/matcher.h"
#include "db/pipeline/document.h"
#include "db/pipeline/expression.h"

namespace mongo {

    const char DocumentSourceMatch::matchName[] = "$match";

    DocumentSourceMatch::~DocumentSourceMatch() {
    }

    void DocumentSourceMatch::sourceToBson(BSONObjBuilder *pBuilder) const {
	const BSONObj *pQuery = matcher.getQuery();
	pBuilder->append(matchName, *pQuery);
    }

    bool DocumentSourceMatch::accept(
	const shared_ptr<Document> &pDocument) const {

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

    shared_ptr<DocumentSource> DocumentSourceMatch::createFromBson(
	BSONElement *pBsonElement,
	const intrusive_ptr<ExpressionContext> &pCtx) {
        assert(pBsonElement->type() == Object);
        // CW TODO error: expression object must be an object

        boost::shared_ptr<DocumentSourceMatch> pMatcher(
	    new DocumentSourceMatch(pBsonElement->Obj()));

        return pMatcher;
    }

    void DocumentSourceMatch::toMatcherBson(BSONObjBuilder *pBuilder) const {
	const BSONObj *pQuery = matcher.getQuery();
	pBuilder->appendElements(*pQuery);
    }

    DocumentSourceMatch::DocumentSourceMatch(const BSONObj &query):
	DocumentSourceFilterBase(),
        matcher(query) {
    }
}
