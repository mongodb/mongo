// wiredtiger_size_storer.h

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

#pragma once

#include <map>
#include <string>
#include <wiredtiger.h>

#include "mongo/base/string_data.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

class WiredTigerRecordStore;
class WiredTigerSession;

class WiredTigerSizeStorer {
public:
    WiredTigerSizeStorer(WT_CONNECTION* conn,
                         const std::string& storageUri,
                         const bool isWiredTigerLoggingEnabled,
                         const bool readOnly = false);
    ~WiredTigerSizeStorer();

    void onCreate(WiredTigerRecordStore* rs, long long nr, long long ds);
    void onDestroy(WiredTigerRecordStore* rs);

    void storeToCache(StringData uri, long long numRecords, long long dataSize);

    void loadFromCache(StringData uri, long long* numRecords, long long* dataSize) const;

    /**
     * Loads from the underlying table.
     */
    void fillCache();

    /**
     * Writes all changes to the underlying table.
     */
    void syncCache(bool syncToDisk);

private:
    void _checkMagic() const;

    struct Entry {
        Entry() : numRecords(0), dataSize(0), dirty(false), rs(NULL) {}
        long long numRecords;
        long long dataSize;
        bool dirty;
        WiredTigerRecordStore* rs;  // not owned
    };

    int _magic;

    // Guards _cursor. Acquire *before* _entriesMutex.
    mutable stdx::mutex _cursorMutex;
    const WiredTigerSession _session;
    WT_CURSOR* _cursor;  // pointer is const after constructor

    typedef std::map<std::string, Entry> Map;
    Map _entries;
    mutable stdx::mutex _entriesMutex;
};
}
