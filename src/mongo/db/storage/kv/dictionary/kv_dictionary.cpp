// kv_catalog.cpp

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

#include "mongo/db/storage/kv/dictionary/kv_dictionary.h"
#include "mongo/db/storage/kv/dictionary/kv_dictionary_update.h"

#include "mongo/db/diskloc.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/kv/slice.h"

namespace mongo {

    static const IndexEntryComparison nullIEC(Ordering::make(BSONObj()));

    KVDictionary::Comparator::Comparator(const Slice &serialized)
        : _cmp(serialized.size() == 0
               ? nullIEC
               : serialized.as<IndexEntryComparison>()),
           // An empty serialization means 'use memcmp'. See Comparator::serialize().
          _useMemcmp(serialized.size() == 0)
    {}

    KVDictionary::Comparator KVDictionary::Comparator::useMemcmp() {
        return Comparator(nullIEC, true);
    }

    KVDictionary::Comparator KVDictionary::Comparator::useIndexEntryComparison(const IndexEntryComparison &cmp) {
        return Comparator(cmp, false);
    }

    Slice KVDictionary::Comparator::serialize() const {
        if (_useMemcmp) {
            // Empty representation.
            return Slice();
        } else {
            // We serialize the bytes that represent the IndexEntryComparison
            return Slice::of(_cmp);
        }
    }

    IndexKeyEntry makeIndexKeyEntry(const Slice& slice) {
        const BSONObj key(slice.data());
        const DiskLoc loc = *reinterpret_cast<const DiskLoc*>(slice.data() + key.objsize());
        return IndexKeyEntry(key, loc);
    }

    int KVDictionary::Comparator::operator()(const Slice &a, const Slice &b) const {
        if (_useMemcmp) {
            // first compare by a commen prefix length using memcmp..
            const int cmp_len = std::min(a.size(), b.size());
            const int c = memcmp(a.data(), b.data(), cmp_len);
            if (c != 0) {
                return c;
            } else {
                // ..then compare by length
                if (a.size() < b.size()) {
                    return -1;
                } else if (a.size() > b.size()) { 
                    return 1;
                } else {
                    return 0;
                }
            }
        } else {
            // TODO: necessary?
            if (a.size() == 0 || b.size() == 0) {
                return a.size() == b.size() ? 0 : ((a.size() == 0) ? -1 : 1);
            }
            const IndexKeyEntry lhs = makeIndexKeyEntry(a);
            const IndexKeyEntry rhs = makeIndexKeyEntry(b);
            return _cmp.compare(lhs, rhs);
        }
    }

    // ---------------------------------------------------------------------- //

    // By default, the dictionary implements updates by applying the update
    // message to the old value and writing back the new value.
    //
    // For most dictionary implementations, this overwrite insert will be
    // inefficient. Those implementations will want to override this method.
    Status KVDictionary::update(OperationContext *opCtx, const Slice &key, const Slice &oldValue,
                                const KVUpdateMessage &message) {
        Slice newValue;
        const Status status = message.apply(oldValue, newValue);
        if (!status.isOK()) {
            return status;
        }

        return insert(opCtx, key, newValue);
    }

} // namespace mongo
