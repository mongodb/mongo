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
#include "mongo/db/storage/wiredtiger/wiredtiger_customization_hooks.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_size_storer.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::string;

namespace {
int MAGIC = 123123;
}

WiredTigerSizeStorer::WiredTigerSizeStorer(WT_CONNECTION* conn,
                                           const std::string& storageUri,
                                           bool logSizeStorerTable,
                                           bool readOnly)
    : _session(conn) {
    WT_SESSION* session = _session.getSession();

    std::string config = WiredTigerCustomizationHooks::get(getGlobalServiceContext())
                             ->getTableCreateConfig(storageUri);
    if (!readOnly) {
        invariantWTOK(session->create(session, storageUri.c_str(), config.c_str()));
        const bool keepOldLoggingSettings = true;
        if (keepOldLoggingSettings) {
            logSizeStorerTable = true;
        }
        uassertStatusOK(
            WiredTigerUtil::setTableLogging(session, storageUri.c_str(), logSizeStorerTable));
    }

    invariantWTOK(
        session->open_cursor(session, storageUri.c_str(), NULL, "overwrite=true", &_cursor));

    _magic = MAGIC;
}

WiredTigerSizeStorer::~WiredTigerSizeStorer() {
    // This shouldn't be necessary, but protects us if we screw up.
    stdx::lock_guard<stdx::mutex> cursorLock(_cursorMutex);

    _magic = 11111;
    _cursor->close(_cursor);
}

void WiredTigerSizeStorer::_checkMagic() const {
    if (MONGO_likely(_magic == MAGIC))
        return;
    log() << "WiredTigerSizeStorer magic wrong: " << _magic;
    invariant(_magic == MAGIC);
}

void WiredTigerSizeStorer::onCreate(WiredTigerRecordStore* rs,
                                    long long numRecords,
                                    long long dataSize) {
    _checkMagic();
    stdx::lock_guard<stdx::mutex> lk(_entriesMutex);
    Entry& entry = _entries[rs->getURI()];
    entry.rs = rs;
    entry.numRecords = numRecords;
    entry.dataSize = dataSize;
    entry.dirty = true;
}

void WiredTigerSizeStorer::onDestroy(WiredTigerRecordStore* rs) {
    _checkMagic();
    stdx::lock_guard<stdx::mutex> lk(_entriesMutex);
    Entry& entry = _entries[rs->getURI()];
    entry.numRecords = rs->numRecords(NULL);
    entry.dataSize = rs->dataSize(NULL);
    entry.dirty = true;
    entry.rs = NULL;
}


void WiredTigerSizeStorer::storeToCache(StringData uri, long long numRecords, long long dataSize) {
    _checkMagic();
    stdx::lock_guard<stdx::mutex> lk(_entriesMutex);
    Entry& entry = _entries[uri.toString()];
    entry.numRecords = numRecords;
    entry.dataSize = dataSize;
    entry.dirty = true;
}

void WiredTigerSizeStorer::loadFromCache(StringData uri,
                                         long long* numRecords,
                                         long long* dataSize) const {
    _checkMagic();
    stdx::lock_guard<stdx::mutex> lk(_entriesMutex);
    Map::const_iterator it = _entries.find(uri.toString());
    if (it == _entries.end()) {
        *numRecords = 0;
        *dataSize = 0;
        return;
    }
    *numRecords = it->second.numRecords;
    *dataSize = it->second.dataSize;
}

void WiredTigerSizeStorer::fillCache() {
    stdx::lock_guard<stdx::mutex> cursorLock(_cursorMutex);
    _checkMagic();

    Map m;
    {
        // Seek to beginning if needed.
        invariantWTOK(_cursor->reset(_cursor));

        // Intentionally ignoring return value.
        ON_BLOCK_EXIT(_cursor->reset, _cursor);

        int cursorNextRet;
        while ((cursorNextRet = _cursor->next(_cursor)) != WT_NOTFOUND) {
            invariantWTOK(cursorNextRet);

            WT_ITEM key;
            WT_ITEM value;
            invariantWTOK(_cursor->get_key(_cursor, &key));
            invariantWTOK(_cursor->get_value(_cursor, &value));
            std::string uriKey(reinterpret_cast<const char*>(key.data), key.size);
            BSONObj data(reinterpret_cast<const char*>(value.data));

            LOG(2) << "WiredTigerSizeStorer::loadFrom " << uriKey << " -> " << redact(data);

            Entry& e = m[uriKey];
            e.numRecords = data["numRecords"].safeNumberLong();
            e.dataSize = data["dataSize"].safeNumberLong();
            e.dirty = false;
            e.rs = NULL;
        }
    }

    stdx::lock_guard<stdx::mutex> lk(_entriesMutex);
    _entries.swap(m);
}

void WiredTigerSizeStorer::syncCache(bool syncToDisk) {
    stdx::lock_guard<stdx::mutex> cursorLock(_cursorMutex);
    _checkMagic();

    Map myMap;
    {
        stdx::lock_guard<stdx::mutex> lk(_entriesMutex);
        for (Map::iterator it = _entries.begin(); it != _entries.end(); ++it) {
            std::string uriKey = it->first;
            Entry& entry = it->second;
            if (entry.rs) {
                if (entry.dataSize != entry.rs->dataSize(NULL)) {
                    entry.dataSize = entry.rs->dataSize(NULL);
                    entry.dirty = true;
                }
                if (entry.numRecords != entry.rs->numRecords(NULL)) {
                    entry.numRecords = entry.rs->numRecords(NULL);
                    entry.dirty = true;
                }
            }

            if (!entry.dirty)
                continue;
            myMap[uriKey] = entry;
        }
    }

    if (myMap.empty())
        return;  // Nothing to do.

    WT_SESSION* session = _session.getSession();
    invariantWTOK(session->begin_transaction(session, syncToDisk ? "sync=true" : ""));
    ScopeGuard rollbacker = MakeGuard(session->rollback_transaction, session, "");

    for (Map::iterator it = myMap.begin(); it != myMap.end(); ++it) {
        string uriKey = it->first;
        Entry& entry = it->second;

        BSONObj data;
        {
            BSONObjBuilder b;
            b.append("numRecords", entry.numRecords);
            b.append("dataSize", entry.dataSize);
            data = b.obj();
        }

        LOG(2) << "WiredTigerSizeStorer::storeInto " << uriKey << " -> " << redact(data);

        WiredTigerItem key(uriKey.c_str(), uriKey.size());
        WiredTigerItem value(data.objdata(), data.objsize());
        _cursor->set_key(_cursor, key.Get());
        _cursor->set_value(_cursor, value.Get());
        invariantWTOK(_cursor->insert(_cursor));
    }

    invariantWTOK(_cursor->reset(_cursor));

    rollbacker.Dismiss();
    invariantWTOK(session->commit_transaction(session, NULL));

    {
        stdx::lock_guard<stdx::mutex> lk(_entriesMutex);
        for (Map::iterator it = _entries.begin(); it != _entries.end(); ++it) {
            it->second.dirty = false;
        }
    }
}
}
