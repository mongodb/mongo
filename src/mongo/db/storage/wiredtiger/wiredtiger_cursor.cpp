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

#include "mongo/platform/basic.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_cursor.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"

namespace mongo {

WiredTigerCursor::WiredTigerCursor(const std::string& uri,
                                   uint64_t tableID,
                                   bool allowOverwrite,
                                   OperationContext* opCtx) {
    _tableID = tableID;
    _ru = WiredTigerRecoveryUnit::get(opCtx);
    _session = _ru->getSession();
    _readOnce = _ru->getReadOnce();

    if (_readOnce) {
        _cursor = _session->getReadOnceCursor(uri, allowOverwrite);
    } else {
        _cursor = _session->getCursor(uri, tableID, allowOverwrite);
    }
}

WiredTigerCursor::~WiredTigerCursor() {
    dassert(_ru->getReadOnce() == _readOnce);

    // Read-once cursors will never take cursors from the cursor cache, and should never release
    // cursors into the cursor cache.
    if (_readOnce) {
        _session->closeCursor(_cursor);
    } else {
        _session->releaseCursor(_tableID, _cursor);
    }
}

void WiredTigerCursor::reset() {
    invariantWTOK(_cursor->reset(_cursor));
}
}  // namespace mongo
