// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/matcher/path.h"
#include "mongo/util/modules.h"

#include <cstddef>

namespace mongo {

/**
 * TODO SERVER-114832 Break audit dependency on this class.
 */
class [[MONGO_MOD_UNFORTUNATELY_OPEN]] MatchableDocument {
public:
    // Inlining to allow subclasses to see that this is a no-op and avoid a function call.
    // Speeds up query execution measurably.
    virtual ~MatchableDocument() {}

    virtual BSONObj toBSON() const = 0;

    /**
     * The newly returned ElementIterator is allowed to keep a pointer to path.
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
    ~BSONMatchableDocument() override;

    BSONObj toBSON() const override {
        return _obj;
    }

    ElementIterator* allocateIterator(const ElementPath* path) const override {
        if (_iteratorUsed)
            return new BSONElementIterator(path, _obj);
        _iteratorUsed = true;
        _iterator.reset(path, _obj);
        return &_iterator;
    }

    void releaseIterator(ElementIterator* iterator) const override {
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

/**
 * A MatchableDocument interface for viewing a BSONElement as if it were wrapped in the top-level
 * field of any given path. For example, given the object obj={a: [5]}, we can create a view over
 * the element obj["a"]["0"]. An iterator over this view with path "i" would behave identically to
 * an iterator over {i: 5} with path "i".
 */
class BSONElementViewMatchableDocument : public MatchableDocument {
public:
    BSONElementViewMatchableDocument(BSONElement elem) : _elem(elem), _iteratorUsed(false) {}

    BSONObj toBSON() const override {
        return BSON("" << _elem);
    }

    /**
     * Creates an iterator over '_elem' as if '_elem' were wrapped in the first field of 'path'.
     * 'path' must have at least one field.
     */
    ElementIterator* allocateIterator(const ElementPath* path) const override {
        // Skip the first field in the path when traversing the document.
        const size_t suffixIndex = 1;

        if (_iteratorUsed) {
            return new BSONElementIterator(path, suffixIndex, _elem);
        }
        _iteratorUsed = true;
        _iterator.reset(path, suffixIndex, _elem);
        return &_iterator;
    }

    void releaseIterator(ElementIterator* iterator) const override {
        if (iterator == &_iterator) {
            _iteratorUsed = false;
        } else {
            delete iterator;
        }
    }

private:
    BSONElement _elem;
    mutable BSONElementIterator _iterator;
    mutable bool _iteratorUsed;
};
}  // namespace mongo
