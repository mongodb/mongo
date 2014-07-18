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

#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/diskloc.h"

namespace mongo {

    /**
     * This represents a single item in an index. This is intended to (possibly) be used by 
     * implementations of the SortedDataInterface interface. An index item simply consists of a key
     * and a disk location, but this could be subclassed to perform more complex tasks.
     */
    struct IndexKeyEntry {
    public:
        IndexKeyEntry(const BSONObj& key, DiskLoc loc) : _key(key), _loc(loc) {}
        virtual ~IndexKeyEntry() { }
        
        virtual const BSONObj& key() const { return _key; }
        virtual DiskLoc loc() const { return _loc; }

    protected:
        BSONObj _key;
        DiskLoc _loc;

    }; // struct IndexKeyEntry

    /**
     * As the name suggests, this class is used to compare two different IndexKeyEntry instances.
     * The existense of compound indexes necessitates some complicated logic. This is meant to
     * support the implementation of the SortedDataInterface::customLocate() and
     * SortedDataInterface::advanceTo() methods, which require fine-grained control over whether the
     * ranges of various keys comprising a compound index are inclusive or exclusive.
     */
    class IndexEntryComparison {
    public:
        IndexEntryComparison(Ordering order) : _order(order) {}

        bool operator() (const IndexKeyEntry& lhs, const IndexKeyEntry& rhs) const;

        /**
         * This function requires quite a bit of explanation:
         * 
         * This functino compares two IndexKeyEntry objects which have been stripped of their field
         * names. Either lhs or rhs represents the lower bound of a query, meaning that either lhs
         * or rhs must be the result of a call to makeQueryObject(). 
         *
         * Ex: lhs's key is {"": 5, "": "foo"}, and it represents the lower bound of a range query.
         * If rhs's key is {"": 4, "": "foo"}, then the function will return :w
         */
        int comparison(const IndexKeyEntry& lhs, const IndexKeyEntry& rhs) const;
        
        /**
         * Preps a query for compare(). Strips fieldNames if there are any.
         */
        static BSONObj makeQueryObject(const BSONObj& keyPrefix,
                                       int prefixLen,
                                       bool prefixExclusive,
                                       const vector<const BSONElement*>& keySuffix,
                                       const vector<bool>& suffixInclusive,
                                       const int cursorDirection);
    private:
        // Due to the limitations in the std::set API we need to use the same type (IndexKeyEntry)
        // for both the stored data and the "query". We cheat and encode extra information in the
        // first byte of the field names in the query. This works because all stored objects should
        // have all field names empty, so their first bytes are '\0'.
        enum BehaviorIfFieldIsEqual {
            normal = '\0',
            less = 'l',
            greater = 'g',
        };

        const Ordering _order;
    }; // struct IndexEntryComparison

} // namespace mongo
