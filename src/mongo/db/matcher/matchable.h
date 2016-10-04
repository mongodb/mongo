// matchable.h

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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/matcher/path.h"

namespace mongo {

class MatchableDocument {
public:
    // Inlining to allow subclasses to see that this is a no-op and avoid a function call.
    // Speeds up query execution measurably.
    virtual ~MatchableDocument() {}

    virtual BSONObj toBSON() const = 0;

    /**
     * The neewly returned ElementIterator is allowed to keep a pointer to path.
     * So the caller of this function should make sure path is in scope until
     * the ElementIterator is deallocated
    */
    virtual ElementIterator* allocateIterator(const ElementPath* path) const = 0;

    virtual void releaseIterator(ElementIterator* iterator) const = 0;

    class IteratorHolder {
    public:
        IteratorHolder(const MatchableDocument* doc, const ElementPath* path) {
            _doc = doc;
            _iterator = _doc->allocateIterator(path);
        }

        ~IteratorHolder() {
            _doc->releaseIterator(_iterator);
        }

        ElementIterator* operator->() const {
            return _iterator;
        }

    private:
        const MatchableDocument* _doc;
        ElementIterator* _iterator;
    };
};

class BSONMatchableDocument : public MatchableDocument {
public:
    BSONMatchableDocument(const BSONObj& obj);
    virtual ~BSONMatchableDocument();

    virtual BSONObj toBSON() const {
        return _obj;
    }

    virtual ElementIterator* allocateIterator(const ElementPath* path) const {
        if (_iteratorUsed)
            return new BSONElementIterator(path, _obj);
        _iteratorUsed = true;
        _iterator.reset(path, _obj);
        return &_iterator;
    }

    virtual void releaseIterator(ElementIterator* iterator) const {
        if (iterator == &_iterator) {
            _iteratorUsed = false;
        } else {
            delete iterator;
        }
    }

private:
    BSONObj _obj;
    mutable BSONElementIterator _iterator;
    mutable bool _iteratorUsed;
};
}
