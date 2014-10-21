// kv_heap_dictionary.cpp

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
#include <map>
#include <utility>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/storage/kv_heap/kv_heap_dictionary.h"
#include "mongo/db/storage/kv_heap/kv_heap_recovery_unit.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    bool KVHeapDictionary::Cursor::ok() const {
        if (_forward()) {
            return _it != _map.end();
        } else {
            return _rit != _map.rend();
        }
    }

    KVHeapDictionary::Cursor::Cursor(const SliceMap &map, const SliceCmp &cmp, const int direction) :
        _map(map), _cmp(cmp), _direction(direction) {
        invariant(direction == 1 || direction == -1);
        if (_forward()) {
            _it = _map.begin();
        } else{
            _rit = _map.rbegin();
        }
    }

    void KVHeapDictionary::Cursor::seek(const Slice &key) {
        if (_forward()) {
            _it = _map.lower_bound(key);
        } else {
            _rit = std::reverse_iterator<SliceMap::const_iterator>(_map.upper_bound(key));
        }
    }

    void KVHeapDictionary::Cursor::advance() {
        invariant(ok());
        if (_forward()) {
            _it++;
        } else {
            _rit++;
        }
    }

    Slice KVHeapDictionary::Cursor::currKey() const {
        invariant(ok());
        if (_forward()) {
            return _it->first;
        } else {
            return _rit->first;
        }
    }

    Slice KVHeapDictionary::Cursor::currVal() const {
        invariant(ok());
        if (_forward()) {
            return _it->second;
        } else {
            return _rit->second;
        }
    }

    // ---------------------------------------------------------------------- //

    void KVHeapDictionary::_insertPair(const Slice &key, const Slice &value) {
        SliceMap::const_iterator it = _map.find(key);
        if (it == _map.end()) {
            _stats.numKeys++;
            _stats.dataSize += key.size() + value.size();
            _stats.storageSize += key.size() + value.size();
        } else {
            _stats.dataSize += value.size() - it->second.size();
            _stats.storageSize += value.size() - it->second.size();
        }
        _map[key.owned()] = value.owned();
    }

    void KVHeapDictionary::_deleteKey(const Slice &key) {
        SliceMap::const_iterator it = _map.find(key);
        invariant(it != _map.end());
        _stats.numKeys--;
        _stats.dataSize -= it->first.size() + it->second.size();
        _stats.storageSize -= it->first.size() + it->second.size();
        _map.erase(it);
    }

    KVHeapDictionary::KVHeapDictionary(const KVDictionary::Comparator cmp)
        : _cmp(cmp),
          _map(_cmp)
    {}

    Status KVHeapDictionary::get(OperationContext *opCtx, const Slice &key, Slice &value) const {
        SliceMap::const_iterator it = _map.find(key);
        if (it != _map.end()) {
            value = it->second;
            return Status::OK();
        }
        return Status(ErrorCodes::NoSuchKey, "not found");
    }

    Status KVHeapDictionary::insert(OperationContext *opCtx, const Slice &key, const Slice &value) {
        KVHeapRecoveryUnit *ru = KVHeapRecoveryUnit::getKVHeapRecoveryUnit(opCtx);
        SliceMap::const_iterator it = _map.find(key);
        if (it == _map.end()) {
            ru->registerChange(new InsertOperation(this, key));
        } else {
            ru->registerChange(new InsertOperation(this, it->first, it->second));
        }
        _insertPair(key, value);
        return Status::OK();
    }

    void KVHeapDictionary::rollbackInsertByDeleting(const Slice &key) {
        _deleteKey(key);
    }

    void KVHeapDictionary::rollbackInsertToOldValue(const Slice &key, const Slice &value) {
        _insertPair(key, value);
    }

    Status KVHeapDictionary::remove(OperationContext *opCtx, const Slice &key) {
        KVHeapRecoveryUnit *ru = KVHeapRecoveryUnit::getKVHeapRecoveryUnit(opCtx);
        SliceMap::const_iterator it = _map.find(key);
        if (it != _map.end()) {
            ru->registerChange(new DeleteOperation(this, it->first, it->second));
            _deleteKey(key);
        }
        return Status::OK();
    }

    void KVHeapDictionary::rollbackDelete(const Slice &key, const Slice &value) {
        _insertPair(key, value);
    }

    Status KVHeapDictionary::setCustomOption(OperationContext *opCtx, const BSONElement& option, BSONObjBuilder* info ) {
        StringData name = option.fieldName();
        if ( name == "usePowerOf2Sizes" ) {
            // we ignore, so just say ok
            return Status::OK();
        }

        return Status( ErrorCodes::InvalidOptions,
                       mongoutils::str::stream()
                       << "unknown custom option to KVHeapDictionary: "
                       << name );
    }

    KVDictionary::Cursor *KVHeapDictionary::getCursor(OperationContext *opCtx, const int direction ) const {
        return new Cursor(_map, _cmp, direction);
    }

} // namespace mongo
