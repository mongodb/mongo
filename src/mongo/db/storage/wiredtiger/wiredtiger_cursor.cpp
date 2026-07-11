// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/storage/wiredtiger/wiredtiger_cursor.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_connection.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string_view>

#include <wiredtiger.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {
using namespace std::literals::string_view_literals;
static constexpr std::string_view kOverwriteFalse = "overwrite=false"sv;
}  // namespace

WiredTigerCursor::WiredTigerCursor(Params params, std::string_view uri, WiredTigerSession& session)
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

WiredTigerPrepareCursor::WiredTigerPrepareCursor(WiredTigerSession& session) : _session(session) {
    _cursor = session.getNewCursor("prepared_discover:", nullptr);
}

WiredTigerPrepareCursor::~WiredTigerPrepareCursor() {
    _session.closeCursor(_cursor);
}
}  // namespace mongo
