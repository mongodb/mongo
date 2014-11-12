// kv_heap_recovery_unit.h

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

#include <algorithm>
#include <string.h>
#include <vector>

#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/kv/slice.h"
#include "mongo/util/assert_util.h"
#include "third_party/boost/boost/shared_ptr.hpp"

namespace mongo {

    namespace {

        int sliceCompare(const Slice &a, const Slice &b) {
            const size_t cmp_len = std::min(a.size(), b.size());
            const int c = memcmp(a.data(), b.data(), cmp_len);
            return c || (a.size() < b.size() ? -1 : 1);
        }

    }

    class KVHeapDictionary;

    class KVHeapChange : public RecoveryUnit::Change {
    protected:
        KVHeapDictionary *_dict;
        Slice _key;
        Slice _value;

    public:
        KVHeapChange(KVHeapDictionary *dict, const Slice &key, const Slice &value)
            : _dict(dict),
              _key(key.owned()),
              _value(value.owned())
        {}

        KVHeapChange(KVHeapDictionary *dict, const Slice &key)
            : _dict(dict),
              _key(key.owned()),
              _value()
        {}

        virtual ~KVHeapChange() {}

        virtual void commit() {}
    };


    class InsertOperation : public KVHeapChange {
        bool _wasDeleted;

    public:
        InsertOperation(KVHeapDictionary *dict, const Slice &key, const Slice &value)
            : KVHeapChange(dict, key, value),
              _wasDeleted(false)
        {}

        InsertOperation(KVHeapDictionary *dict, const Slice &key)
            : KVHeapChange(dict, key),
              _wasDeleted(true)
        {}

        virtual void rollback();
    };

    class DeleteOperation : public KVHeapChange {
    public:
        DeleteOperation(KVHeapDictionary *dict, const Slice &key, const Slice &value)
            : KVHeapChange(dict, key, value)
        {}

        virtual void rollback();
    };

    class KVHeapRecoveryUnit : public RecoveryUnit {
        MONGO_DISALLOW_COPYING(KVHeapRecoveryUnit);

        std::vector<boost::shared_ptr<Change> > _ops;

    public:
        KVHeapRecoveryUnit() {}
        virtual ~KVHeapRecoveryUnit() {}

        virtual void beginUnitOfWork() {}

        virtual void commitUnitOfWork();

        virtual void commitAndRestart();

        virtual void endUnitOfWork();

        virtual bool awaitCommit() {
            return true;
        }

        virtual void* writingPtr(void* data, size_t len) {
            // die
            return data;
        }

        virtual void syncDataAndTruncateJournal() {}

        virtual void registerChange(Change* change);

        static KVHeapRecoveryUnit* getKVHeapRecoveryUnit(OperationContext* opCtx);
    };

} // namespace mongo
