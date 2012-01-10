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
#include "db/pipeline/document.h"
#include "db/pipeline/expression.h"
#include "db/pipeline/expression_context.h"
#include "db/pipeline/value.h"

namespace mongo {
    const char DocumentSourceSkip::skipName[] = "$skip";

    DocumentSourceSkip::DocumentSourceSkip(const intrusive_ptr<ExpressionContext> &pTheCtx):
        skip(0),
        count(0),
        pCtx(pTheCtx) {
    }

    DocumentSourceSkip::~DocumentSourceSkip() {
    }

    void DocumentSourceSkip::skipper() {
        if (count == 0) {
            while (!pSource->eof() && count++ < skip) {
                pSource->advance();
            }
        }

        if (pSource->eof()) {
            pCurrent.reset();
            return;
        }

        pCurrent = pSource->getCurrent();
    }

    bool DocumentSourceSkip::eof() {
        skipper();
        return pSource->eof();
    }

    bool DocumentSourceSkip::advance() {
        if (eof()) {
            pCurrent.reset();
            return false;
        }

        pCurrent = pSource->getCurrent();
        return pSource->advance();
    }

    intrusive_ptr<Document> DocumentSourceSkip::getCurrent() {
        skipper();
        return pCurrent;
    }

    void DocumentSourceSkip::sourceToBson(BSONObjBuilder *pBuilder) const {
        pBuilder->append("$skip", skip);
    }

    intrusive_ptr<DocumentSourceSkip> DocumentSourceSkip::create(
        const intrusive_ptr<ExpressionContext> &pCtx) {
        intrusive_ptr<DocumentSourceSkip> pSource(
            new DocumentSourceSkip(pCtx));
        return pSource;
    }

    intrusive_ptr<DocumentSource> DocumentSourceSkip::createFromBson(
        BSONElement *pBsonElement,
        const intrusive_ptr<ExpressionContext> &pCtx) {
        uassert(15972, str::stream() << "the value to " <<
                skipName << " must be a number", pBsonElement->isNumber());

        intrusive_ptr<DocumentSourceSkip> pSkip(
            DocumentSourceSkip::create(pCtx));

        pSkip->skip = (int)pBsonElement->numberLong();
        assert(pSkip->skip > 0); // CW TODO error code

        return pSkip;
    }
}
