// wiredtiger_size_storer.cpp

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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

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
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

WiredTigerSizeStorer::WiredTigerSizeStorer(WT_CONNECTION* conn,
                                           const std::string& storageUri,
                                           bool readOnly)
    : _session(conn), _readOnly(readOnly) {
    WT_SESSION* session = _session.getSession();

    std::string config = WiredTigerCustomizationHooks::get(getGlobalServiceContext())
                             ->getTableCreateConfig(storageUri);
    if (!readOnly) {
        invariantWTOK(session->create(session, storageUri.c_str(), config.c_str()));
    }

    invariantWTOK(
        session->open_cursor(session, storageUri.c_str(), nullptr, "overwrite=true", &_cursor));
}

WiredTigerSizeStorer::~WiredTigerSizeStorer() {
    stdx::lock_guard<stdx::mutex> cursorLock(_cursorMutex);
    _cursor->close(_cursor);
}

void WiredTigerSizeStorer::store(StringData uri, std::shared_ptr<SizeInfo> sizeInfo) {
    // If the SizeInfo is still dirty, we're done.
    if (sizeInfo->_dirty.load() || _readOnly)
        return;

    // Ordering is important: as the entry may be flushed concurrently, set the dirty flag last.
    stdx::lock_guard<stdx::mutex> lk(_bufferMutex);
    auto& entry = _buffer[uri];
    // During rollback it is possible to get a new SizeInfo. In that case clear the dirty flag,
    // so the SizeInfo can be destructed without triggering the dirty check invariant.
    if (entry && entry.get() != sizeInfo.get())
        entry->_dirty.store(false);
    entry = sizeInfo;
    entry->_dirty.store(true);
    LOG(2) << "WiredTigerSizeStorer::store Marking " << uri
           << " dirty, numRecords: " << sizeInfo->numRecords.load()
           << ", dataSize: " << sizeInfo->dataSize.load() << ", use_count: " << entry.use_count();
}

std::shared_ptr<WiredTigerSizeStorer::SizeInfo> WiredTigerSizeStorer::load(StringData uri) const {
    {
        // Check if we can satisfy the read from the buffer.
        stdx::lock_guard<stdx::mutex> bufferLock(_bufferMutex);
        Buffer::const_iterator it = _buffer.find(uri);
        if (it != _buffer.end())
            return it->second;
    }

    stdx::lock_guard<stdx::mutex> cursorLock(_cursorMutex);
    // Intentionally ignoring return value.
    ON_BLOCK_EXIT(_cursor->reset, _cursor);

    _cursor->reset(_cursor);

    {
        WT_ITEM key = {uri.rawData(), uri.size()};
        _cursor->set_key(_cursor, &key);
        int ret = _cursor->search(_cursor);
        if (ret == WT_NOTFOUND)
            return std::make_shared<SizeInfo>();
        invariantWTOK(ret);
    }

    WT_ITEM value;
    invariantWTOK(_cursor->get_value(_cursor, &value));
    BSONObj data(reinterpret_cast<const char*>(value.data));

    LOG(2) << "WiredTigerSizeStorer::load " << uri << " -> " << redact(data);
    auto result = std::make_shared<SizeInfo>();
    result->numRecords.store(data["numRecords"].safeNumberLong());
    result->dataSize.store(data["dataSize"].safeNumberLong());
    return result;
}

void WiredTigerSizeStorer::flush(bool syncToDisk) {
    Buffer buffer;
    {
        stdx::lock_guard<stdx::mutex> bufferLock(_bufferMutex);
        _buffer.swap(buffer);
    }

    if (buffer.empty())
        return;  // Nothing to do.

    Timer t;
    stdx::lock_guard<stdx::mutex> cursorLock(_cursorMutex);
    {
        // On failure, place entries back into the map, unless a newer value already exists.
        ON_BLOCK_EXIT([this, &buffer]() {
            this->_cursor->reset(this->_cursor);
            if (!buffer.empty()) {
                stdx::lock_guard<stdx::mutex> bufferLock(this->_bufferMutex);
                for (auto& it : buffer)
                    this->_buffer.try_emplace(it.first, it.second);
            }
        });

        WT_SESSION* session = _session.getSession();
        WiredTigerBeginTxnBlock txnOpen(session, syncToDisk ? "sync=true" : nullptr);

        for (auto it = buffer.begin(); it != buffer.end(); ++it) {

            // Ordering is important here: when the store method checks if the SizeInfo
            // is dirty and it returns true, the current values of numRecords and dataSize must
            // still be written back. So, the required order is to clear the dirty flag first.
            SizeInfo& sizeInfo = *it->second;
            sizeInfo._dirty.store(false);
            BSONObj data = BSON("numRecords" << sizeInfo.numRecords.load() << "dataSize"
                                             << sizeInfo.dataSize.load());

            auto& uri = it->first;
            LOG(2) << "WiredTigerSizeStorer::flush " << uri << " -> " << redact(data);
            WiredTigerItem key(uri.c_str(), uri.size());
            WiredTigerItem value(data.objdata(), data.objsize());
            _cursor->set_key(_cursor, key.Get());
            _cursor->set_value(_cursor, value.Get());
            invariantWTOK(_cursor->insert(_cursor));
        }
        txnOpen.done();
        invariantWTOK(session->commit_transaction(session, nullptr));
        buffer.clear();
    }

    auto micros = t.micros();
    LOG(2) << "WiredTigerSizeStorer flush took " << micros << " µs";
}
}  // namespace mongo
