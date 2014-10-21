// kv_heap_recovery_unit.cpp

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

#include "mongo/db/operation_context.h"
#include "mongo/db/storage/kv_heap/kv_heap_dictionary.h"
#include "mongo/db/storage/kv_heap/kv_heap_recovery_unit.h"

namespace mongo {

    void KVHeapRecoveryUnit::commitUnitOfWork() {
        for (std::vector< boost::shared_ptr<Change> >::iterator it = _ops.begin();
             it != _ops.end(); ++it) {
            Change *op = it->get();
            op->commit();
        }
        _ops.clear();
    }

    void KVHeapRecoveryUnit::commitAndRestart() {
        commitUnitOfWork();
    }

    void KVHeapRecoveryUnit::endUnitOfWork() {
        for (std::vector< boost::shared_ptr<Change> >::reverse_iterator it = _ops.rbegin();
             it != _ops.rend(); ++it) {
            Change *op = it->get();
            op->rollback();
        }
        _ops.clear();
    }

    void KVHeapRecoveryUnit::registerChange(Change* change) {
        _ops.push_back(boost::shared_ptr<Change>(change));
    }

    KVHeapRecoveryUnit* KVHeapRecoveryUnit::getKVHeapRecoveryUnit(OperationContext* opCtx) {
        return dynamic_cast<KVHeapRecoveryUnit*>(opCtx->recoveryUnit());
    }

    void InsertOperation::rollback() {
        if (_wasDeleted) {
            _dict->rollbackInsertByDeleting(_key);
        } else {
            _dict->rollbackInsertToOldValue(_key, _value);
        }
    }

    void DeleteOperation::rollback() {
        _dict->rollbackDelete(_key, _value);
    }

} // namespace mongo
