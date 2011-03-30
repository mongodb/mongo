/**
 * Copyright (c) 2011 10gen Inc.
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
#include "db/jsobj.h"
#include "db/pipeline/document.h"
#include "db/pipeline/value.h"

namespace mongo {
    shared_ptr<Document> Document::createFromBsonObj(BSONObj *pBsonObj) {
        shared_ptr<Document> pDocument(new Document(pBsonObj));
        return pDocument;
    }

    Document::Document(BSONObj *pBsonObj):
        vFieldName(),
        vpValue() {
        BSONObjIterator bsonIterator(pBsonObj->begin());
        while(bsonIterator.more()) {
            BSONElement bsonElement(bsonIterator.next());
            string fieldName(bsonElement.fieldName());
            shared_ptr<const Value> pValue(
                Value::createFromBsonElement(&bsonElement));

            vFieldName.push_back(fieldName);
            vpValue.push_back(pValue);
        }
    }

    void Document::toBson(BSONObjBuilder *pBuilder) {
        const size_t n = vFieldName.size();
        for(size_t i = 0; i < n; ++i)
            vpValue[i]->addToBsonObj(pBuilder, vFieldName[i]);
    }

    shared_ptr<Document> Document::create(size_t sizeHint) {
        shared_ptr<Document> pDocument(new Document(sizeHint));
        return pDocument;
    }

    Document::Document(size_t sizeHint):
        vFieldName(),
        vpValue() {
        if (sizeHint) {
            vFieldName.reserve(sizeHint);
            vpValue.reserve(sizeHint);
        }
    }

    shared_ptr<Document> Document::clone() {
        const size_t n = vFieldName.size();
        shared_ptr<Document> pNew(Document::create(n));
        for(size_t i = 0; i < n; ++i)
            pNew->addField(vFieldName[i], vpValue[i]);

        return pNew;
    }

    Document::~Document() {
    }

    FieldIterator *Document::createFieldIterator() {
        return new FieldIterator(shared_from_this());
    }

    shared_ptr<const Value> Document::getValue(string fieldName) {
        /*
          For now, assume the number of fields is small enough that iteration
          is ok.  Later, if this gets large, we can create a map into the
          vector for these lookups.

          Note that because of the schema-less nature of this data, we always
          have to look, and can't assume that the requested field is always
          in a particular place as we would with a statically compilable
          reference.
        */
        const size_t n = vFieldName.size();
        for(size_t i = 0; i < n; ++i) {
            if (strcmp(vFieldName[i].c_str(), fieldName.c_str()) == 0)
                return vpValue[i];
        }

        return(shared_ptr<const Value>());
    }

    void Document::addField(string fieldName, shared_ptr<const Value> pValue) {
        vFieldName.push_back(fieldName);
        vpValue.push_back(pValue);
    }

    void Document::setField(size_t index,
                            string fieldName, shared_ptr<const Value> pValue) {
        vFieldName[index] = fieldName;
        vpValue[index] = pValue;
    }

    int Document::compare(const shared_ptr<Document> &rL,
                          const shared_ptr<Document> &rR) {
        const size_t lSize = rL->vFieldName.size();
        const size_t rSize = rR->vFieldName.size();

        for(size_t i = 0; true; ++i) {
            if (i >= lSize) {
                if (i >= rSize)
                    return 0; // documents are the same length

                return-1; // left document is shorter
            }

            if (i >= rSize)
                return 1; // right document is shorter

            const int nameCmp = rL->vFieldName[i].compare(rR->vFieldName[i]);
            if (nameCmp)
                return nameCmp; // field names are unequal

            const int valueCmp = Value::compare(rL->vpValue[i], rR->vpValue[i]);
            if (valueCmp)
                return valueCmp; // fields are unequal
        }

        /* NOTREACHED */
        assert(false); // CW TODO
        return 0;
    }

    /* ----------------------- FieldIterator ------------------------------- */

    FieldIterator::FieldIterator(shared_ptr<Document> pTheDocument):
        pDocument(pTheDocument),
        index(0) {
    }

    bool FieldIterator::more() const {
        return (index < pDocument->vFieldName.size());
    }

    pair<string, shared_ptr<const Value>> FieldIterator::next() {
        assert(more());
        pair<string, shared_ptr<const Value>> result(
            pDocument->vFieldName[index], pDocument->vpValue[index]);
        ++index;
        return result;
    }
}
