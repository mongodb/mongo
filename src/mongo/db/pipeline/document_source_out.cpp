/**
 * Copyright 2011 (c) 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "pch.h"

#include "db/pipeline/document_source.h"


namespace mongo {

    const char DocumentSourceOut::outName[] = "$out";

    DocumentSourceOut::~DocumentSourceOut() {
    }

    bool DocumentSourceOut::eof() {
        return pSource->eof();
    }

    bool DocumentSourceOut::advance() {
        return pSource->advance();
    }

    boost::intrusive_ptr<Document> DocumentSourceOut::getCurrent() {
        return pSource->getCurrent();
    }

    DocumentSourceOut::DocumentSourceOut(BSONElement *pBsonElement) {
        assert(false && "unimplemented");
    }

    intrusive_ptr<DocumentSourceOut> DocumentSourceOut::createFromBson(
        BSONElement *pBsonElement) {
        intrusive_ptr<DocumentSourceOut> pSource(
            new DocumentSourceOut(pBsonElement));

        return pSource;
    }

    void DocumentSourceOut::sourceToBson(BSONObjBuilder *pBuilder) const {
        assert(false); // CW TODO
    }
}
