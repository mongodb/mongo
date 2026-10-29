// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/storage/wiredtiger/wiredtiger_cursor.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_connection.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_cursor_helpers.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/overloaded_visitor.h"
#include "mongo/util/str.h"

#include <cstring>
#include <string_view>

#include <wiredtiger.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {
using namespace std::literals::string_view_literals;
static constexpr std::string_view kOverwriteFalse = "overwrite=false"sv;

void setKeyOnCursor(WT_CURSOR* c, const std::variant<std::span<const char>, int64_t>& key) {
    std::visit(OverloadedVisitor{
                   [&](const std::span<const char> k) { c->set_key(c, WiredTigerItem{k}.get()); },
                   [&](int64_t k) {
                       c->set_key(c, k);
                   }},
               key);
}
}  // namespace

WiredTigerCursor::WiredTigerCursor(Params params,
                                   const std::string& uri,
                                   WiredTigerSession& session)
    : _tableID(params.tableID), _session(session), _sizeStats(params.sizeStats) {
    invariant(!(params.sizeStats && params.random),
              "size_stats is incompatible with a random cursor");

    // Retain the URI only for size_stats cursors; onScanComplete() uses it to read back and log the
    // accumulated summary. Left unset otherwise.
    if (_sizeStats) {
        _uri.emplace(uri);
    }

    // Passing nullptr is significantly faster for WiredTiger than passing an empty string.
    const char* configStr = nullptr;

    // If we have uncommon cursor options, use a costlier string builder.
    if (params.readOnce || params.random || params.sizeStats) {
        str::stream builder;
        if (params.readOnce) {
            builder << "read_once=true,";
        }

        if (params.random) {
            builder << "next_random,";
        }

        if (params.sizeStats) {
            builder << "debug=(size_stats=true),";
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

    // A size_stats cursor initializes its size-summary counters on open, bypass the cache.
    if (!_sizeStats) {
        // Attempt to retrieve a cursor from the cache.
        _cursor = _session.getCachedCursor(_tableID, _config);
        if (_cursor) {
            return;
        }
    }

    try {
        _cursor = _session.getNewCursor(uri, configStr);
    } catch (const ExceptionFor<ErrorCodes::CursorNotFound>& ex) {
        LOGV2_FATAL_NOTRACE(50883, "Cursor not found", "error"_attr = ex);
        throw;
    }
}

void WiredTigerCursor::onScanComplete() {
    if (MONGO_likely(!_sizeStats || _sizeStatsLogged)) {
        return;
    }
    _sizeStatsLogged = true;
    // The forward walk is complete, so the accumulated summary is whole. Read it back and log it.
    // A size-stats read failure must not fail the surrounding scan (diagnostic-only), so swallow
    // errors into a warning.
    try {
        WiredTigerUtil::logStorageSizeStats(_session, *_uri);
    } catch (const DBException& ex) {
        LOGV2_WARNING(12951901,
                      "Failed to log storage size summary for size_stats cursor",
                      "uri"_attr = *_uri,
                      "error"_attr = ex.toStatus());
    }
}

WiredTigerCursor::~WiredTigerCursor() {
    if (_sizeStats) {
        // Never cache a size_stats cursor: closing it guarantees the next open resets the counters.
        _session.closeCursor(_cursor);
        return;
    }
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

Status WiredTigerDirectCrudCursor::insert(RecoveryUnit& ru, Key key, std::span<const char> value) {
    invariant(ru.inUnitOfWork());
    auto& wtRu = WiredTigerRecoveryUnit::get(ru);
    wtRu.assertInActiveTxn();
    WT_CURSOR* c = _cursor.get();

    setKeyOnCursor(c, key);
    c->set_value(c, WiredTigerItem{value}.get());

    int rc = WT_OP_CHECK(wiredTigerCursorInsert(wtRu, c));
    return wtRCToStatus(rc, _cursor->session);
}

Status WiredTigerDirectCrudCursor::update(RecoveryUnit& ru, Key key, std::span<const char> value) {
    invariant(ru.inUnitOfWork());
    auto& wtRu = WiredTigerRecoveryUnit::get(ru);
    wtRu.assertInActiveTxn();
    WT_CURSOR* c = _cursor.get();

    setKeyOnCursor(c, key);
    c->set_value(c, WiredTigerItem{value}.get());

    int rc = WT_OP_CHECK(wiredTigerCursorUpdate(wtRu, c));
    if (rc == WT_NOTFOUND)
        return Status(ErrorCodes::NoSuchKey, "No such key exists in ident");
    return wtRCToStatus(rc, _cursor->session);
}

StatusWith<UniqueBuffer> WiredTigerDirectCrudCursor::get(Key key) {
    WT_CURSOR* c = _cursor.get();

    setKeyOnCursor(c, key);

    int rc = WT_OP_CHECK(c->search(c));
    if (rc == WT_NOTFOUND)
        return Status(ErrorCodes::NoSuchKey, "No such key exists in ident");
    if (auto status = wtRCToStatus(rc, _cursor->session); !status.isOK())
        return status;

    WiredTigerItem v;
    rc = c->get_value(c, v.get());
    if (auto status = wtRCToStatus(rc, _cursor->session); !status.isOK())
        return status;

    UniqueBuffer out = UniqueBuffer::allocate(v.size());
    std::copy(v.data(), v.data() + v.size(), out.get());
    return out;
}

Status WiredTigerDirectCrudCursor::remove(RecoveryUnit& ru, Key key) {
    invariant(ru.inUnitOfWork());
    auto& wtRu = WiredTigerRecoveryUnit::get(ru);
    wtRu.assertInActiveTxn();
    WT_CURSOR* c = _cursor.get();

    setKeyOnCursor(c, key);

    int rc = WT_OP_CHECK(wiredTigerCursorRemove(wtRu, c));
    if (rc == WT_NOTFOUND)
        return Status(ErrorCodes::NoSuchKey, "No such key exists in ident");
    return wtRCToStatus(rc, _cursor->session);
}
}  // namespace mongo
