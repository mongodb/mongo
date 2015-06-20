// in_memory_engine.cpp

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

#include "mongo/db/storage/in_memory/in_memory_engine.h"

#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/storage/in_memory/in_memory_btree_impl.h"
#include "mongo/db/storage/in_memory/in_memory_record_store.h"
#include "mongo/db/storage/in_memory/in_memory_recovery_unit.h"

namespace mongo {

RecoveryUnit* InMemoryEngine::newRecoveryUnit() {
    return new InMemoryRecoveryUnit();
}

Status InMemoryEngine::createRecordStore(OperationContext* opCtx,
                                         StringData ns,
                                         StringData ident,
                                         const CollectionOptions& options) {
    // All work done in getRecordStore
    return Status::OK();
}

RecordStore* InMemoryEngine::getRecordStore(OperationContext* opCtx,
                                            StringData ns,
                                            StringData ident,
                                            const CollectionOptions& options) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (options.capped) {
        return new InMemoryRecordStore(ns,
                                       &_dataMap[ident],
                                       true,
                                       options.cappedSize ? options.cappedSize : 4096,
                                       options.cappedMaxDocs ? options.cappedMaxDocs : -1);
    } else {
        return new InMemoryRecordStore(ns, &_dataMap[ident]);
    }
}

Status InMemoryEngine::createSortedDataInterface(OperationContext* opCtx,
                                                 StringData ident,
                                                 const IndexDescriptor* desc) {
    // All work done in getSortedDataInterface
    return Status::OK();
}

SortedDataInterface* InMemoryEngine::getSortedDataInterface(OperationContext* opCtx,
                                                            StringData ident,
                                                            const IndexDescriptor* desc) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return getInMemoryBtreeImpl(Ordering::make(desc->keyPattern()), &_dataMap[ident]);
}

Status InMemoryEngine::dropIdent(OperationContext* opCtx, StringData ident) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _dataMap.erase(ident);
    return Status::OK();
}

int64_t InMemoryEngine::getIdentSize(OperationContext* opCtx, StringData ident) {
    return 1;
}

std::vector<std::string> InMemoryEngine::getAllIdents(OperationContext* opCtx) const {
    std::vector<std::string> all;
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        for (DataMap::const_iterator it = _dataMap.begin(); it != _dataMap.end(); ++it) {
            all.push_back(it->first);
        }
    }
    return all;
}
}
