// rocks_index_entry_comparator.h

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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

#include <rocksdb/comparator.h>

#include "mongo/db/storage/rocks/rocks_sorted_data_impl.h"

namespace mongo {

    class RocksIndexEntryComparator : public rocksdb::Comparator {
    public:
        RocksIndexEntryComparator(Ordering order): _indexComparator(order) { }
        virtual ~RocksIndexEntryComparator() { } 

        virtual int Compare(const rocksdb::Slice& a, const rocksdb::Slice& b) const;

        virtual const char* Name() const { return "mongodb.RocksIndexEntryComparator"; }

        /**
         * If *start < limit, changes *start to a short string in [start,limit).
         * Simple comparator implementations may return with *start unchanged,
         * i.e., an implementation of this method that does nothing is correct.
         */
        virtual void FindShortestSeparator( std::string* start, const rocksdb::Slice& limit) const{}

        /**
         * Changes *key to a short string >= *key.
         * Simple comparator implementations may return with *key unchanged,
         * i.e., an implementation of this method that does nothing is correct.
         */
        virtual void FindShortSuccessor(std::string* key) const { }

    private:
        IndexEntryComparison _indexComparator;
    };

} // namespace mongo
