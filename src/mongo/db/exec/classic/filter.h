// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/matchable.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * The MatchExpression uses the MatchableDocument interface to see if a document satisfies the
 * expression.  This wraps a WorkingSetMember in the MatchableDocument interface so that any of
 * the WorkingSetMember's various types can be tested to see if they satisfy an expression.
 */
class WorkingSetMatchableDocument : public MatchableDocument {
public:
    WorkingSetMatchableDocument(WorkingSetMember* wsm)
        : _wsm(wsm), _obj(_wsm->doc.value().toBson()) {}

    // This is only called by a $where query.  The query system must be smart enough to realize
    // that it should do a fetch beforehand.
    BSONObj toBSON() const override {
        return _obj;
    }

    ElementIterator* allocateIterator(const ElementPath* path) const final {
        // BSONElementIterator does some interesting things with arrays that I don't think
        // SimpleArrayElementIterator does.
        if (_wsm->hasObj()) {
            return new BSONElementIterator(path, _obj);
        }

        // NOTE: This (kind of) duplicates code in WorkingSetMember::getFieldDotted.
        // Keep in sync w/that.
        // Find the first field in the index key data described by path and return an iterator
        // over it.
        for (size_t i = 0; i < _wsm->keyData.size(); ++i) {
            BSONObjIterator keyPatternIt(_wsm->keyData[i].indexKeyPattern);
            BSONObjIterator keyDataIt(_wsm->keyData[i].keyData);

            while (keyPatternIt.more()) {
                BSONElement keyPatternElt = keyPatternIt.next();
                tassert(11051641,
                        "Expecting indexKeyPattern and keyData objects to store the same number of "
                        "elements",
                        keyDataIt.more());
                BSONElement keyDataElt = keyDataIt.next();

                if (path->fieldRef().equalsDottedField(keyPatternElt.fieldName())) {
                    if (BSONType::array == keyDataElt.type()) {
                        return new SimpleArrayElementIterator(keyDataElt, true);
                    } else {
                        return new SingleElementElementIterator(keyDataElt);
                    }
                }
            }
        }

        // This should not happen.
        massert(16920,
                "trying to match on unknown field: " + std::string{path->fieldRef().dottedField()},
                0);

        return new SingleElementElementIterator(BSONElement());
    }

    void releaseIterator(ElementIterator* iterator) const final {
        delete iterator;
    }

private:
    WorkingSetMember* _wsm;
    BSONObj _obj;
};

class IndexKeyMatchableDocument : public MatchableDocument {
public:
    IndexKeyMatchableDocument(const BSONObj& key, const BSONObj& keyPattern)
        : _keyPattern(keyPattern), _key(key) {}

    BSONObj toBSON() const override {
        return _key;
    }

    ElementIterator* allocateIterator(const ElementPath* path) const final {
        BSONObjIterator keyPatternIt(_keyPattern);
        BSONObjIterator keyDataIt(_key);

        while (keyPatternIt.more()) {
            BSONElement keyPatternElt = keyPatternIt.next();
            tassert(11051640,
                    "Expecting keyPattern and key objects to store the same number of elements",
                    keyDataIt.more());
            BSONElement keyDataElt = keyDataIt.next();

            if (path->fieldRef().equalsDottedField(keyPatternElt.fieldName())) {
                if (BSONType::array == keyDataElt.type()) {
                    return new SimpleArrayElementIterator(keyDataElt, true);
                } else {
                    return new SingleElementElementIterator(keyDataElt);
                }
            }
        }

        // Planning should not let this happen.
        massert(17409,
                "trying to match on unknown field: " + std::string{path->fieldRef().dottedField()},
                0);

        return new SingleElementElementIterator(BSONElement());
    }

    void releaseIterator(ElementIterator* iterator) const final {
        delete iterator;
    }

private:
    BSONObj _keyPattern;
    BSONObj _key;
};

/**
 * Used by every stage with a filter.
 */
class Filter {
public:
    /**
     * Returns true if filter is NULL or if 'wsm' satisfies the filter.
     * Returns false if 'wsm' does not satisfy the filter.
     */
    static bool passes(WorkingSetMember* wsm, const MatchExpression* filter) {
        if (nullptr == filter) {
            return true;
        }
        WorkingSetMatchableDocument doc(wsm);
        return exec::matcher::matches(filter, &doc, nullptr);
    }

    static bool passes(const BSONObj& keyData,
                       const BSONObj& keyPattern,
                       const MatchExpression* filter) {
        if (nullptr == filter) {
            return true;
        }
        IndexKeyMatchableDocument doc(keyData, keyPattern);
        return exec::matcher::matches(filter, &doc, nullptr);
    }
};

}  // namespace mongo
