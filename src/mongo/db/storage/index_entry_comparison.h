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

#pragma once

namespace mongo {

    /**
     * This represents a single item in an index. This is intended to (possibly) be used by 
     * implementations of the SortedDataInterface interface. An index item simply consists of a key
     *  and a disk location, but this could be subclassed to perform more complex tasks.
     */
    struct IndexKeyEntry {
        IndexKeyEntry(const BSONObj& key, DiskLoc loc) : _key(key), _loc(loc) {}
        virtual ~IndexKeyEntry() { }
        
        virtual const BSONObj& key() const { return _key; }
        virtual DiskLoc loc() const { return _loc; }

    protected:
        BSONObj _key;
        DiskLoc _loc;

    }; // struct IndexKeyEntry

    /**
     * As the name suggests, this is used to compare two different IndexKeyEntry instances. The 
     * existense of compound indexes necessitates some complicated logic. This is meant to
     * support the implementation of the SortedDataInterface::customLocate() and
     * SortedDataInterface::advanceTo() methods, which require fine-grained control over whether the
     * ranges of various keys comprising a compound index are inclusive or exclusive.
     */
    struct IndexEntryComparison {
        IndexEntryComparison(Ordering order) : order(order) {}

        bool operator() (const IndexKeyEntry& lhs, const IndexKeyEntry& rhs) const {
            // implementing in memcmp style to ease reuse of this code.
            return comparison(lhs, rhs) < 0;
        }

        // Due to the limitations in the std::set API we need to use the same type (IndexKeyEntry)
        // for both the stored data and the "query". We cheat and encode extra information in the
        // first byte of the field names in the query. This works because all stored objects should
        // have all field names empty, so their first bytes are '\0'.
        enum BehaviorIfFieldIsEqual {
            normal = '\0',
            less = 'l',
            greater = 'g',
        };

        // This should behave the same as customBSONCmp from btree_logic.cpp.
        int comparison(const IndexKeyEntry& lhs, const IndexKeyEntry& rhs) const {
            BSONObjIterator lhsIt(lhs.key());
            BSONObjIterator rhsIt(rhs.key());
            
            for (unsigned mask = 1; lhsIt.more(); mask <<= 1) {
                invariant(rhsIt.more());

                const BSONElement l = lhsIt.next();
                const BSONElement r = rhsIt.next();

                if (int cmp = l.woCompare(r, /*compareFieldNames=*/false)) {
                    invariant(cmp != std::numeric_limits<int>::min()); // can't be negated
                    return order.descending(mask)
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
            invariant(!rhsIt.more());

            // This means just look at the key, not the loc.
            if (lhs.loc().isNull() || rhs.loc().isNull())
                return 0;

            return lhs.loc().compare(rhs.loc()); // is supposed to ignore ordering
        }
        
        /**
         * Preps a query for compare(). Strips fieldNames if there are any.
         */
        static BSONObj makeQueryObject(const BSONObj& keyPrefix,
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

        const Ordering order;
    }; // struct IndexEntryComparison

} // namespace mongo
