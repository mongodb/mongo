/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include <wiredtiger.h>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_begin_transaction_block.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_customization_hooks.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_size_storer.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/logv2/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

WiredTigerSizeStorer::WiredTigerSizeStorer(WT_CONNECTION* conn,
                                           const std::string& storageUri,
                                           bool readOnly)
    : _conn(conn),
      _storageUri(storageUri),
      _tableId(WiredTigerSession::genTableId()),
      _readOnly(readOnly) {
    if (_readOnly) {
        return;
    }

    std::string config = WiredTigerCustomizationHooks::get(getGlobalServiceContext())
                             ->getTableCreateConfig(_storageUri);

    WiredTigerSession session(_conn);
    invariantWTOK(
        session.getSession()->create(session.getSession(), _storageUri.c_str(), config.c_str()));
}

void WiredTigerSizeStorer::store(StringData uri, std::shared_ptr<SizeInfo> sizeInfo) {
    // If the SizeInfo is still dirty, we're done.
    if (sizeInfo->_dirty.load() || _readOnly)
        return;

    // Ordering is important: as the entry may be flushed concurrently, set the dirty flag last.
    stdx::lock_guard<Latch> lk(_bufferMutex);
    auto& entry = _buffer[uri];
    // During rollback it is possible to get a new SizeInfo. In that case clear the dirty flag,
    // so the SizeInfo can be destructed without triggering the dirty check invariant.
    if (entry && entry.get() != sizeInfo.get())
        entry->_dirty.store(false);
    entry = sizeInfo;
    entry->_dirty.store(true);
    LOGV2_DEBUG(22423,
                2,
                "WiredTigerSizeStorer::store",
                "uri"_attr = uri,
                "numRecords"_attr = sizeInfo->numRecords.load(),
                "dataSize"_attr = sizeInfo->dataSize.load(),
                "entryUseCount"_attr = entry.use_count());
}

std::shared_ptr<WiredTigerSizeStorer::SizeInfo> WiredTigerSizeStorer::load(OperationContext* opCtx,
                                                                           StringData uri) const {
    {
        // Check if we can satisfy the read from the buffer.
        stdx::lock_guard<Latch> bufferLock(_bufferMutex);
        Buffer::const_iterator it = _buffer.find(uri);
        if (it != _buffer.end())
            return it->second;
    }

    WiredTigerCursor cursor(_storageUri, _tableId, /*allowOverwrite=*/false, opCtx);

    {
        WT_ITEM key = {uri.rawData(), uri.size()};
        cursor->set_key(cursor.get(), &key);
        int ret = cursor->search(cursor.get());
        if (ret == WT_NOTFOUND)
            return std::make_shared<SizeInfo>();
        invariantWTOK(ret);
    }

    WT_ITEM value;
    invariantWTOK(cursor->get_value(cursor.get(), &value));
    BSONObj data(reinterpret_cast<const char*>(value.data));

    LOGV2_DEBUG(
        22424, 2, "WiredTigerSizeStorer::load", "uri"_attr = uri, "data"_attr = redact(data));
    return std::make_shared<SizeInfo>(data["numRecords"].safeNumberLong(),
                                      data["dataSize"].safeNumberLong());
}

void WiredTigerSizeStorer::flush(bool syncToDisk) {
    Buffer buffer;
    {
        stdx::lock_guard<Latch> bufferLock(_bufferMutex);
        _buffer.swap(buffer);
    }

    if (buffer.empty())
        return;  // Nothing to do.

    Timer t;

    // We serialize flushing to disk to avoid running into write conflicts from having multiple
    // threads try to flush at the same time.
    stdx::lock_guard<Latch> flushLock(_flushMutex);

    // When the session is destructed, it closes any cursors that remain open.
    WiredTigerSession session(_conn);
    WT_CURSOR* cursor = session.getNewCursor(_storageUri, "overwrite=true");

    {
        // On failure, place entries back into the map, unless a newer value already exists.
        ON_BLOCK_EXIT([this, &buffer]() {
            if (!buffer.empty()) {
                stdx::lock_guard<Latch> bufferLock(this->_bufferMutex);
                for (auto& it : buffer)
                    this->_buffer.try_emplace(it.first, it.second);
            }
        });

        // To avoid deadlocks with cache eviction, allow the transaction to time itself out. Once
        // the time limit has been exceeded on an operation in this transaction, WiredTiger returns
        // WT_ROLLBACK for that operation.
        std::string txnConfig = "operation_timeout_ms=10";
        if (syncToDisk) {
            txnConfig += ",sync=true";
        }
        WiredTigerBeginTxnBlock txnOpen(session.getSession(), txnConfig.c_str());

        for (auto it = buffer.begin(); it != buffer.end(); ++it) {

            // Ordering is important here: when the store method checks if the SizeInfo
            // is dirty and it returns true, the current values of numRecords and dataSize must
            // still be written back. So, the required order is to clear the dirty flag first.
            SizeInfo& sizeInfo = *it->second;
            sizeInfo._dirty.store(false);
            BSONObj data = BSON("numRecords" << sizeInfo.numRecords.load() << "dataSize"
                                             << sizeInfo.dataSize.load());

            auto& uri = it->first;
            LOGV2_DEBUG(22425,
                        2,
                        "WiredTigerSizeStorer::flush",
                        "uri"_attr = uri,
                        "data"_attr = redact(data));
            WiredTigerItem key(uri.c_str(), uri.size());
            WiredTigerItem value(data.objdata(), data.objsize());
            cursor->set_key(cursor, key.Get());
            cursor->set_value(cursor, value.Get());
            int ret = cursor->insert(cursor);
            if (ret == WT_ROLLBACK) {
                // One of the code paths calling this function is when a session is checked back
                // into the session cache. This could involve read-only operations which don't
                // except write conflicts. If WiredTiger returns WT_ROLLBACK during the flush, we
                // skip flushing.
                return;
            }
            invariantWTOK(ret);
        }
        txnOpen.done();
        invariantWTOK(session.getSession()->commit_transaction(session.getSession(), nullptr));
        buffer.clear();
    }

    LOGV2_DEBUG(22426,
                2,
                "WiredTigerSizeStorer::flush completed",
                "duration"_attr = Microseconds{t.micros()});
}
}  // namespace mongo
