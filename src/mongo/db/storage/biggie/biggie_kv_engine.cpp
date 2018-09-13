/**
 *    Copyright (C) 2018 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/db/storage/biggie/biggie_kv_engine.h"

#include "mongo/base/disallow_copying.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/snapshot_window_options.h"
#include "mongo/db/storage/biggie/biggie_recovery_unit.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/platform/basic.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"


namespace mongo {
namespace biggie {

mongo::RecoveryUnit* KVEngine::newRecoveryUnit() {
    return new RecoveryUnit(this, nullptr);
}

void KVEngine::setCachePressureForTest(int pressure) {
    // TODO : implement.
}

Status KVEngine::createRecordStore(OperationContext* opCtx,
                                   StringData ns,
                                   StringData ident,
                                   const CollectionOptions& options) {
    log() << "Creating Ident in KVEngine with ident: " << ident;
    _idents.insert(ident);
    return Status::OK();
}

std::unique_ptr<::mongo::RecordStore> KVEngine::getRecordStore(OperationContext* opCtx,
                                                               StringData ns,
                                                               StringData ident,
                                                               const CollectionOptions& options) {
    // TODO: deal with options.
    return std::make_unique<RecordStore>(ns, ident);
}

void KVEngine::setMaster_inlock(std::unique_ptr<StringStore> newMaster) {
    _master.reset(newMaster.release());
}

std::shared_ptr<StringStore> KVEngine::getMaster() const {
    // TODO : later on this needs to be changed to use their copy function.
    stdx::lock_guard<stdx::mutex> lk(_masterLock);
    return _master;
}

std::shared_ptr<StringStore> KVEngine::getMaster_inlock() const {
    return _master;
}


Status KVEngine::createSortedDataInterface(OperationContext* opCtx,
                                           StringData ident,
                                           const IndexDescriptor* desc) {
    return Status::OK();  // I don't think we actually need to do anything here
}

mongo::SortedDataInterface* KVEngine::getSortedDataInterface(OperationContext* opCtx,
                                                             StringData ident,
                                                             const IndexDescriptor* desc) {
    return new SortedDataInterface(Ordering::make(desc->keyPattern()), desc->unique(), ident);
}


class EmptyRecordCursor final : public SeekableRecordCursor {
public:
    boost::optional<Record> next() final {
        return {};
    }
    boost::optional<Record> seekExact(const RecordId& id) final {
        return {};
    }
    void save() final {}
    bool restore() final {
        return true;
    }
    void detachFromOperationContext() final {}
    void reattachToOperationContext(OperationContext* opCtx) final {}
};
}  // namespace biggie
}  // namespace mongo
