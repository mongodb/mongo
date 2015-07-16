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

#include "mongo/db/exec/working_set.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/matchable.h"

namespace mongo {

/**
 * The MatchExpression uses the MatchableDocument interface to see if a document satisfies the
 * expression.  This wraps a WorkingSetMember in the MatchableDocument interface so that any of
 * the WorkingSetMember's various types can be tested to see if they satisfy an expression.
 */
class WorkingSetMatchableDocument : public MatchableDocument {
public:
    WorkingSetMatchableDocument(WorkingSetMember* wsm) : _wsm(wsm) {}

    // This is only called by a $where query.  The query system must be smart enough to realize
    // that it should do a fetch beforehand.
    BSONObj toBSON() const {
        invariant(_wsm->hasObj());
        return _wsm->obj.value();
    }

    ElementIterator* allocateIterator(const ElementPath* path) const final {
        // BSONElementIterator does some interesting things with arrays that I don't think
        // SimpleArrayElementIterator does.
        if (_wsm->hasObj()) {
            return new BSONElementIterator(path, _wsm->obj.value());
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
                invariant(keyDataIt.more());
                BSONElement keyDataElt = keyDataIt.next();

                if (path->fieldRef().equalsDottedField(keyPatternElt.fieldName())) {
                    if (Array == keyDataElt.type()) {
                        return new SimpleArrayElementIterator(keyDataElt, true);
                    } else {
                        return new SingleElementElementIterator(keyDataElt);
                    }
                }
            }
        }

        // This should not happen.
        massert(16920,
                "trying to match on unknown field: " + path->fieldRef().dottedField().toString(),
                0);

        return new SingleElementElementIterator(BSONElement());
    }

    void releaseIterator(ElementIterator* iterator) const final {
        delete iterator;
    }

private:
    WorkingSetMember* _wsm;
};

class IndexKeyMatchableDocument : public MatchableDocument {
public:
    IndexKeyMatchableDocument(const BSONObj& key, const BSONObj& keyPattern)
        : _keyPattern(keyPattern), _key(key) {}

    BSONObj toBSON() const {
        return _key;
    }

    ElementIterator* allocateIterator(const ElementPath* path) const final {
        BSONObjIterator keyPatternIt(_keyPattern);
        BSONObjIterator keyDataIt(_key);

        while (keyPatternIt.more()) {
            BSONElement keyPatternElt = keyPatternIt.next();
            invariant(keyDataIt.more());
            BSONElement keyDataElt = keyDataIt.next();

            if (path->fieldRef().equalsDottedField(keyPatternElt.fieldName())) {
                if (Array == keyDataElt.type()) {
                    return new SimpleArrayElementIterator(keyDataElt, true);
                } else {
                    return new SingleElementElementIterator(keyDataElt);
                }
            }
        }

        // Planning should not let this happen.
        massert(17409,
                "trying to match on unknown field: " + path->fieldRef().dottedField().toString(),
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
        if (NULL == filter) {
            return true;
        }
        WorkingSetMatchableDocument doc(wsm);
        return filter->matches(&doc, NULL);
    }

    static bool passes(const BSONObj& keyData,
                       const BSONObj& keyPattern,
                       const MatchExpression* filter) {
        if (NULL == filter) {
            return true;
        }
        IndexKeyMatchableDocument doc(keyData, keyPattern);
        return filter->matches(&doc, NULL);
    }
};

}  // namespace mongo
