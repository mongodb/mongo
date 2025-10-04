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


#include "mongo/db/storage/wiredtiger/wiredtiger_cursor.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_connection.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <wiredtiger.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {
static constexpr StringData kOverwriteFalse = "overwrite=false"_sd;
}  // namespace

WiredTigerCursor::WiredTigerCursor(Params params, StringData uri, WiredTigerSession& session)
    : _tableID(params.tableID), _session(session) {
    // Passing nullptr is significantly faster for WiredTiger than passing an empty string.
    const char* configStr = nullptr;

    // If we have uncommon cursor options, use a costlier string builder.
    if (params.readOnce || params.random) {
        str::stream builder;
        if (params.readOnce) {
            builder << "read_once=true,";
        }

        if (params.random) {
            builder << "next_random,";
        }

        // Add this option last as the string does not have a trailing comma.
        if (!params.allowOverwrite) {
            builder << kOverwriteFalse;
        }

        _config = builder;
        configStr = _config.c_str();
    } else {
        // Add this option without a trailing comma. This enables an optimization in WiredTiger to
        // skip parsing the config string if this is the only option. See SERVER-43232 for details.
        if (!params.allowOverwrite) {
            _config = std::string{kOverwriteFalse};
            configStr = kOverwriteFalse.data();
        }
    }

    // Attempt to retrieve a cursor from the cache.
    _cursor = _session.getCachedCursor(_tableID, _config);
    if (_cursor) {
        return;
    }

    try {
        _cursor = _session.getNewCursor(uri, configStr);
    } catch (const ExceptionFor<ErrorCodes::CursorNotFound>& ex) {
        LOGV2_FATAL_NOTRACE(50883, "Cursor not found", "error"_attr = ex);
        throw;
    }
}

WiredTigerCursor::~WiredTigerCursor() {
    _session.releaseCursor(_tableID, _cursor, std::move(_config));
}

WiredTigerBulkLoadCursor::WiredTigerBulkLoadCursor(OperationContext* opCtx,
                                                   WiredTigerSession& outerSession,
                                                   const std::string& indexUri)
    : _session(outerSession.getConnection().getSession(*opCtx)) {
    // Open cursors can cause bulk open_cursor to fail with EBUSY.
    // TODO any other cases that could cause EBUSY?
    outerSession.closeAllCursors(indexUri);

    // The 'checkpoint_wait=false' option is set to prefer falling back on the "non-bulk" cursor
    // over waiting a potentially long time for a checkpoint.
    int err =
        _session->open_cursor(indexUri.c_str(), nullptr, "bulk,checkpoint_wait=false", &_cursor);
    if (!err) {
        return;  // Success
    }

    LOGV2_WARNING(51783,
                  "Failed to create WiredTiger bulk cursor, falling back to non-bulk",
                  "error"_attr = wiredtiger_strerror(err),
                  "index"_attr = indexUri);

    invariantWTOK(_session->open_cursor(indexUri.c_str(), nullptr, nullptr, &_cursor), *_session);
}
}  // namespace mongo
