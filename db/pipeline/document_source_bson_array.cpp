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

#include "db/pipeline/document.h"

namespace mongo {

    DocumentSourceBsonArray::~DocumentSourceBsonArray() {
    }

    bool DocumentSourceBsonArray::eof() {
	return !arrayIterator.more();
    }

    bool DocumentSourceBsonArray::advance() {
	if (eof())
	    return false;

	currentElement = arrayIterator.next();
	return true;
    }

    shared_ptr<Document> DocumentSourceBsonArray::getCurrent() {
        BSONObj documentObj(currentElement.Obj());
        shared_ptr<Document> pDocument(
            Document::createFromBsonObj(&documentObj));
        return pDocument;
    }

    void DocumentSourceBsonArray::setSource(
	shared_ptr<DocumentSource> pSource) {
	/* this doesn't take a source */
	assert(false);
    }

    DocumentSourceBsonArray::DocumentSourceBsonArray(
	BSONElement *pBsonElement):
        embeddedObject(pBsonElement->embeddedObject()),
        arrayIterator(embeddedObject) {
	if (arrayIterator.more())
	    currentElement = arrayIterator.next();
    }

    shared_ptr<DocumentSourceBsonArray> DocumentSourceBsonArray::create(
	BSONElement *pBsonElement) {

	assert(pBsonElement->type() == Array);
	shared_ptr<DocumentSourceBsonArray> pSource(
	    new DocumentSourceBsonArray(pBsonElement));

	return pSource;
    }

    void DocumentSourceBsonArray::sourceToBson(BSONObjBuilder *pBuilder) const {
	assert(false); // this has no analog in the BSON world
    }
}
