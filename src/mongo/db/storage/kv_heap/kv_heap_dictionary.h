// kv_heap_dictionary.h

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

#include <map>

#include "mongo/base/status.h"
#include "mongo/db/storage/kv/dictionary/kv_dictionary.h"
#include "mongo/db/storage/kv/slice.h"

namespace mongo {

    class KVHeapDictionary : public KVDictionary {
    public:
        class SliceCmp {
            KVDictionary::Comparator _cmp;

        public:
            SliceCmp(KVDictionary::Comparator cmp)
                : _cmp(cmp)
            {}

            int cmp(const Slice &a, const Slice &b) const {
                return _cmp(a, b);
            }

            bool operator()(const Slice &a, const Slice &b) const {
                const int c = cmp(a, b);
                return c < 0;
            }
        };

    private:
        SliceCmp _cmp;

        typedef std::map<Slice, Slice, SliceCmp> SliceMap;
        SliceMap _map;

        Stats _stats;

        class Cursor : public KVDictionary::Cursor {
            const SliceMap &_map;
            const SliceCmp &_cmp;
            const int _direction;

            SliceMap::const_iterator _it;
            SliceMap::const_reverse_iterator _rit;

            bool _forward() const { return _direction > 0; }

        public:
            Cursor(const SliceMap &map, const SliceCmp &cmp, const int direction);

            bool ok() const;

            void seek(const Slice &key);

            void advance();

            Slice currKey() const;

            Slice currVal() const;
        };

        void _insertPair(const Slice &key, const Slice &value);

        void _deleteKey(const Slice &key);

    public:
        KVHeapDictionary(const KVDictionary::Comparator cmp = KVDictionary::Comparator::useMemcmp());

        Status get(OperationContext *opCtx, const Slice &key, Slice &value) const;

        Status insert(OperationContext *opCtx, const Slice &key, const Slice &value);

        void rollbackInsertByDeleting(const Slice &key);

        void rollbackInsertToOldValue(const Slice &key, const Slice &value);

        Status remove(OperationContext *opCtx, const Slice &key);

        void rollbackDelete(const Slice &key, const Slice &value);

        // --------

        const char *name() const { return "KVHeapDictionary"; }

        Stats getStats() const { return _stats; }

        void appendCustomStats(OperationContext *opCtx, BSONObjBuilder* result, double scale ) const { } 

        Status setCustomOption(OperationContext *opCtx, const BSONElement& option, BSONObjBuilder* info );

        virtual Status compact(OperationContext *opCtx) {
            return Status::OK();
        }

        KVDictionary::Cursor *getCursor(OperationContext *opCtx, const int direction = 1) const;
    };

} // namespace mongo
