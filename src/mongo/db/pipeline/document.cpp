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
#include <boost/functional/hash.hpp>
#include "db/jsobj.h"
#include "db/pipeline/dependency_tracker.h"
#include "db/pipeline/document.h"
#include "db/pipeline/value.h"
#include "util/mongoutils/str.h"

namespace mongo {
    using namespace mongoutils;

    string Document::idName("_id");

    intrusive_ptr<Document> Document::createFromBsonObj(
        BSONObj *pBsonObj, const DependencyTracker *pDependencies) {
        intrusive_ptr<Document> pDocument(
            new Document(pBsonObj, pDependencies));
        return pDocument;
    }

    Document::Document(BSONObj *pBsonObj,
                       const DependencyTracker *pDependencies):
        vFieldName(),
        vpValue() {
        BSONObjIterator bsonIterator(pBsonObj->begin());
        while(bsonIterator.more()) {
            BSONElement bsonElement(bsonIterator.next());
            string fieldName(bsonElement.fieldName());

            // LATER check pDependencies
            // LATER grovel through structures???
            intrusive_ptr<const Value> pValue(
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

    intrusive_ptr<Document> Document::create(size_t sizeHint) {
        intrusive_ptr<Document> pDocument(new Document(sizeHint));
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

    intrusive_ptr<Document> Document::clone() {
        const size_t n = vFieldName.size();
        intrusive_ptr<Document> pNew(Document::create(n));
        for(size_t i = 0; i < n; ++i)
            pNew->addField(vFieldName[i], vpValue[i]);

        return pNew;
    }

    Document::~Document() {
    }

    FieldIterator *Document::createFieldIterator() {
        return new FieldIterator(intrusive_ptr<Document>(this));
    }

    intrusive_ptr<const Value> Document::getValue(const string &fieldName) {
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
            if (fieldName.compare(vFieldName[i]) == 0)
                return vpValue[i];
        }

        return(intrusive_ptr<const Value>());
    }

    void Document::addField(const string &fieldName,
                            const intrusive_ptr<const Value> &pValue) {
        uassert(15945, str::stream() << "cannot add undefined field " <<
                fieldName << " to document", pValue->getType() != Undefined);

        vFieldName.push_back(fieldName);
        vpValue.push_back(pValue);
    }

    void Document::setField(size_t index,
                            const string &fieldName,
                            const intrusive_ptr<const Value> &pValue) {
        /* special case:  should this field be removed? */
        if (!pValue.get()) {
            vFieldName.erase(vFieldName.begin() + index);
            vpValue.erase(vpValue.begin() + index);
            return;
        }

        /* make sure we have a valid value */
        uassert(15968, str::stream() << "cannot set undefined field " <<
                fieldName << " to document", pValue->getType() != Undefined);

        /* set the indicated field */
        vFieldName[index] = fieldName;
        vpValue[index] = pValue;
    }

    intrusive_ptr<const Value> Document::getField(const string &fieldName) const {
        const size_t n = vFieldName.size();
        for(size_t i = 0; i < n; ++i) {
            if (fieldName.compare(vFieldName[i]) == 0)
                return vpValue[i];
        }

        /* if we got here, there's no such field */
        return intrusive_ptr<const Value>();
    }

    size_t Document::getApproximateSize() const {
        size_t size = sizeof(Document);
        const size_t n = vpValue.size();
        for(size_t i = 0; i < n; ++i)
            size += vpValue[i]->getApproximateSize();

        return size;
    }

    size_t Document::getFieldIndex(const string &fieldName) const {
        const size_t n = vFieldName.size();
        size_t i = 0;
        for(; i < n; ++i) {
            if (fieldName.compare(vFieldName[i]) == 0)
                break;
        }

        return i;
    }

    void Document::hash_combine(size_t &seed) const {
        const size_t n = vFieldName.size();
        for(size_t i = 0; i < n; ++i) {
            boost::hash_combine(seed, vFieldName[i]);
            vpValue[i]->hash_combine(seed);
        }
    }

    int Document::compare(const intrusive_ptr<Document> &rL,
                          const intrusive_ptr<Document> &rR) {
        const size_t lSize = rL->vFieldName.size();
        const size_t rSize = rR->vFieldName.size();

        for(size_t i = 0; true; ++i) {
            if (i >= lSize) {
                if (i >= rSize)
                    return 0; // documents are the same length

                return -1; // left document is shorter
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
        verify(false);
        return 0;
    }

    /* ----------------------- FieldIterator ------------------------------- */

    FieldIterator::FieldIterator(const intrusive_ptr<Document> &pTheDocument):
        pDocument(pTheDocument),
        index(0) {
    }

    bool FieldIterator::more() const {
        return (index < pDocument->vFieldName.size());
    }

    pair<string, intrusive_ptr<const Value> > FieldIterator::next() {
        verify(more());
        pair<string, intrusive_ptr<const Value> > result(
            pDocument->vFieldName[index], pDocument->vpValue[index]);
        ++index;
        return result;
    }
}
