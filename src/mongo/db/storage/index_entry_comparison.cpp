/**
 *    Copyright (C) 2014 MongoDB Inc.
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
#include "mongo/platform/basic.h"

#include "mongo/db/storage/index_entry_comparison.h"

namespace mongo {
    bool IndexEntryComparison::operator() (const IndexKeyEntry& lhs, const IndexKeyEntry& rhs)
       const {
        // implementing in memcmp style to ease reuse of this code.
        return comparison(lhs, rhs) < 0;
    }

    // This should behave the same as customBSONCmp from btree_logic.cpp.
    //
    // Reading the comment in the .h file is HIGHLY recommended if you need to understand what this
    // function is doing
    int IndexEntryComparison::comparison(const IndexKeyEntry& lhs, const IndexKeyEntry& rhs) const {
        BSONObjIterator lhsIt(lhs.key());
        BSONObjIterator rhsIt(rhs.key());

        for (unsigned mask = 1; lhsIt.more(); mask <<= 1) {
            // XXX: commented this out since we found cases where lhs and rhs are not of the
            //      same length. THis seems to be allowed in mmap
            // invariant(rhsIt.more());

            const BSONElement l = lhsIt.next();
            const BSONElement r = rhsIt.next();

            if (int cmp = l.woCompare(r, /*compareFieldNames=*/false)) {
                invariant(cmp != std::numeric_limits<int>::min()); // can't be negated
                return _order.descending(mask)
                    ? -cmp
                    : cmp;
            }

            // Here is where the weirdness begins. We sometimes want to fudge the comparison
            // when a key == the query to implement exclusive ranges.
            BehaviorIfFieldIsEqual lEqBehavior = BehaviorIfFieldIsEqual(l.fieldName()[0]);
            BehaviorIfFieldIsEqual rEqBehavior = BehaviorIfFieldIsEqual(r.fieldName()[0]);

            if (lEqBehavior) {
                // lhs is the query, rhs is the stored data
                invariant(rEqBehavior == normal);
                return lEqBehavior == less ? -1 : 1;
            }

            if (rEqBehavior) {
                // rhs is the query, lhs is the stored data, so reverse the returns
                invariant(lEqBehavior == normal);
                return lEqBehavior == less ? 1 : -1;
            }

        }
        // invariant(!rhsIt.more());
        if (rhsIt.more())
            return 1;

        // This means just look at the key, not the loc.
        if (lhs.loc().isNull() || rhs.loc().isNull())
            return 0;

        return lhs.loc().compare(rhs.loc()); // is supposed to ignore ordering
    }

    // Reading the comment in the .h file is HIGHLY recommended if you need to understand what this
    // function is doing
    BSONObj IndexEntryComparison::makeQueryObject(const BSONObj& keyPrefix,
                                                        int prefixLen,
                                                        bool prefixExclusive,
                                                        const vector<const BSONElement*>& keySuffix,
                                                        const vector<bool>& suffixInclusive,
                                                        const int cursorDirection) {

        // See comment above for why this is done.
        const char exclusiveByte = (cursorDirection == 1
                ? IndexEntryComparison::greater
                : IndexEntryComparison::less);
        const StringData exclusiveFieldName(&exclusiveByte, 1);

        BSONObjBuilder bb;

        // handle the prefix
        if (prefixLen > 0) {
            BSONObjIterator it(keyPrefix);
            for (int i = 0; i < prefixLen; i++) {
                invariant(it.more());
                const BSONElement e = it.next();

                if (prefixExclusive && i == prefixLen - 1) {
                    bb.appendAs(e, exclusiveFieldName);
                }
                else {
                    bb.appendAs(e, StringData());
                }
            }
        }

        // Handle the suffix. Note that the useful parts of the suffix start at index prefixLen
        // rather than at 0.
        invariant(keySuffix.size() == suffixInclusive.size());
        for (size_t i = prefixLen; i < keySuffix.size(); i++) {
            if (suffixInclusive[i]) {
                bb.appendAs(*keySuffix[i], StringData());
            }
            else {
                bb.appendAs(*keySuffix[i], exclusiveFieldName);
            }
        }

        return bb.obj();
    }

} // namespace mongo
