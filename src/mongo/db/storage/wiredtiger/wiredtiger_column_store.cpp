/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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


#include "mongo/db/storage/wiredtiger/wiredtiger_column_store.h"

#include <algorithm>
#include <cstring>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <wiredtiger.h>

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/index_names.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/resource_consumption_metrics.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_cursor.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_cursor_helpers.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_customization_hooks.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_index_cursor_generic.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_index_util.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_prepare_conflict.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/endian.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
StatusWith<std::string> WiredTigerColumnStore::generateCreateString(
    const std::string& engineName,
    const NamespaceString& collectionNamespace,
    const IndexDescriptor& desc,
    bool isLogged) {
    StringBuilder sb;

    invariant(desc.getIndexType() == INDEX_COLUMN);

    // Separate out a prefix and suffix in the default string. User configuration will override
    // values in the prefix, but not values in the suffix.
    sb << "type=file,internal_page_max=16k,leaf_page_max=16k,";
    sb << "checksum=on,";
    sb << "prefix_compression=true,";
    // Ignoring wiredTigerGlobalOptions.useIndexPrefixCompression because we *always* want prefix
    // compression for column indexes.

    sb << "dictionary=128,";
    sb << WiredTigerCustomizationHooks::get(getGlobalServiceContext())
              ->getTableCreateConfig(NamespaceStringUtil::serializeForCatalog(collectionNamespace));

    sb << "block_compressor="
       << desc.compressor().value_or(WiredTigerGlobalOptions::kDefaultColumnStoreIndexCompressor)
       << ",";

    // WARNING: No user-specified config can appear below this line. These options are required
    // for correct behavior of the server.

    // Indexes need to store the metadata for collation to work as expected.
    sb << "key_format=u,";
    sb << "value_format=u,";

    if (isLogged) {
        sb << "log=(enabled=true)";
    } else {
        sb << "log=(enabled=false)";
    }

    LOGV2_DEBUG(6510200, 3, "index create string", "str"_attr = sb.stringData());
    return sb.str();
}

Status WiredTigerColumnStore::create(OperationContext* opCtx,
                                     const std::string& uri,
                                     const std::string& config) {
    // Don't use the session from the recovery unit: create should not be used in a transaction
    WiredTigerSession session(WiredTigerRecoveryUnit::get(opCtx)->getSessionCache()->conn());
    WT_SESSION* s = session.getSession();
    LOGV2_DEBUG(
        6510201, 1, "create uri: {uri} config: {config}", "uri"_attr = uri, "config"_attr = config);
    return wtRCToStatus(s->create(s, uri.c_str(), config.c_str()), s);
}

WiredTigerColumnStore::WiredTigerColumnStore(OperationContext* ctx,
                                             const std::string& uri,
                                             StringData ident,
                                             const IndexDescriptor* desc,
                                             bool isLogged)
    : ColumnStore(ident),
      _uri(uri),
      _tableId(WiredTigerSession::genTableId()),
      _desc(desc),
      _indexName(desc->indexName()),
      _isLogged(isLogged) {}

std::string& WiredTigerColumnStore::makeKeyInBuffer(std::string& buffer, PathView path, RowId rid) {
    buffer.clear();
    buffer.reserve(path.size() + 1 /*NUL byte*/ + sizeof(RowId));
    buffer += path;
    // If we end up reserving more values, as we've done with kRowIdPath, this check should be
    // changed to include the newly reserved values.
    if (path != kRowIdPath) {
        buffer += '\0';
    }
    if (rid > 0) {
        RowId num = endian::nativeToBig(rid);
        buffer.append(reinterpret_cast<const char*>(&num), sizeof(num));
    }
    return buffer;
}

class WiredTigerColumnStore::WriteCursor final : public ColumnStore::WriteCursor {
public:
    WriteCursor(OperationContext* opCtx, const std::string& uri, uint64_t tableId)
        : _opCtx(opCtx),
          _curwrap(*WiredTigerRecoveryUnit::get(opCtx), uri, tableId, true /* allow overwrite */) {
        _curwrap.assertInActiveTxn();
    }

    void insert(PathView, RowId, CellView) override;
    void remove(PathView, RowId) override;
    void update(PathView, RowId, CellView) override;

    WT_CURSOR* c() {
        return _curwrap.get();
    }

private:
    OperationContext* _opCtx;
    WiredTigerCursor _curwrap;
};

std::unique_ptr<ColumnStore::WriteCursor> WiredTigerColumnStore::newWriteCursor(
    OperationContext* opCtx) {
    return std::make_unique<WriteCursor>(opCtx, _uri, _tableId);
}

void WiredTigerColumnStore::insert(OperationContext* opCtx,
                                   PathView path,
                                   RowId rid,
                                   CellView cell) {
    WriteCursor(opCtx, _uri, _tableId).insert(path, rid, cell);
}
void WiredTigerColumnStore::WriteCursor::insert(PathView path, RowId rid, CellView cell) {
    // Lock invariant relaxed because index builds apply side writes while holding collection MODE_S
    // (global MODE_IS).
    dassert(shard_role_details::getLocker(_opCtx)->isLocked());

    auto key = makeKey(path, rid);
    auto keyItem = WiredTigerItem(key);
    auto valueItem = WiredTigerItem(cell.rawData(), cell.size());

    c()->set_key(c(), keyItem.Get());
    c()->set_value(c(), valueItem.Get());
    int ret = WT_OP_CHECK(wiredTigerCursorInsert(*WiredTigerRecoveryUnit::get(_opCtx), c()));

    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(_opCtx);
    metricsCollector.incrementOneIdxEntryWritten(c()->uri, keyItem.size);

    if (ret) {
        uassertStatusOK(wtRCToStatus(ret, c()->session));
    }
}

void WiredTigerColumnStore::remove(OperationContext* opCtx, PathView path, RowId rid) {
    WriteCursor(opCtx, _uri, _tableId).remove(path, rid);
}
void WiredTigerColumnStore::WriteCursor::remove(PathView path, RowId rid) {
    // Lock invariant relaxed because index builds apply side writes while holding collection MODE_S
    // (global MODE_IS).
    dassert(shard_role_details::getLocker(_opCtx)->isLocked());

    auto key = makeKey(path, rid);
    auto keyItem = WiredTigerItem(key);
    c()->set_key(c(), keyItem.Get());
    int ret = WT_OP_CHECK(wiredTigerCursorRemove(*WiredTigerRecoveryUnit::get(_opCtx), c()));
    if (ret == WT_NOTFOUND) {
        return;
    }
    invariantWTOK(ret, c()->session);

    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(_opCtx);
    metricsCollector.incrementOneIdxEntryWritten(c()->uri, keyItem.size);
}
void WiredTigerColumnStore::update(OperationContext* opCtx,
                                   PathView path,
                                   RowId rid,
                                   CellView cell) {
    WriteCursor(opCtx, _uri, _tableId).update(path, rid, cell);
}
void WiredTigerColumnStore::WriteCursor::update(PathView path, RowId rid, CellView cell) {
    // Lock invariant relaxed because index builds apply side writes while holding collection MODE_S
    // (global MODE_IS).
    dassert(shard_role_details::getLocker(_opCtx)->isLocked());

    auto key = makeKey(path, rid);
    auto keyItem = WiredTigerItem(key);
    auto valueItem = WiredTigerItem(cell.rawData(), cell.size());

    c()->set_key(c(), keyItem.Get());
    c()->set_value(c(), valueItem.Get());
    int ret = WT_OP_CHECK(wiredTigerCursorUpdate(*WiredTigerRecoveryUnit::get(_opCtx), c()));

    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(_opCtx);
    metricsCollector.incrementOneIdxEntryWritten(c()->uri, keyItem.size);

    if (ret != 0)
        return uassertStatusOK(wtRCToStatus(ret, c()->session));
}

IndexValidateResults WiredTigerColumnStore::validate(OperationContext* opCtx, bool full) const {
    dassert(shard_role_details::getLocker(opCtx)->isReadLocked());

    IndexValidateResults results;

    WiredTigerUtil::validateTableLogging(*WiredTigerRecoveryUnit::get(opCtx),
                                         _uri,
                                         _isLogged,
                                         StringData{_indexName},
                                         results.valid,
                                         results.errors,
                                         results.warnings);

    if (!full) {
        return results;
    }

    WiredTigerIndexUtil::validateStructure(*WiredTigerRecoveryUnit::get(opCtx), _uri, results);

    return results;
}

int64_t WiredTigerColumnStore::numEntries(OperationContext* opCtx) const {
    int64_t count = 0;

    auto cursor = newCursor(opCtx);
    while (cursor->next()) {
        ++count;
    }

    return count;
}

class WiredTigerColumnStore::Cursor final : public ColumnStore::Cursor,
                                            public WiredTigerIndexCursorGeneric {
public:
    Cursor(OperationContext* opCtx, const WiredTigerColumnStore* idx)
        : WiredTigerIndexCursorGeneric(opCtx, true /* forward */),
          _uri(idx->uri()),
          _tableId(idx->_tableId),
          _indexName(idx->indexName()) {
        _cursor.emplace(*WiredTigerRecoveryUnit::get(_opCtx), _uri, _tableId, false);
    }

    boost::optional<FullCellView> next() override {
        if (_eof) {
            return {};
        }
        if (!_lastMoveSkippedKey) {
            _eof = !advanceWTCursor();
        }

        return curr();
    }

    /**
     * Seeks the cursor to the column key specified by the given 'path' and 'rid'. If the key is not
     * found, then the next key in the same path will be returned; or the first entry in the
     * following column; or boost::none if there are no further entries.
     */
    boost::optional<FullCellView> seekAtOrPast(PathView path, RowId rid) override {
        // Initialize the _buffer with the column's key (path/rid).
        makeKeyInBuffer(_buffer, path, rid);
        seekWTCursorAtOrPast(_buffer);
        return curr();
    }

    boost::optional<FullCellView> seekExact(PathView path, RowId rid) override {
        makeKeyInBuffer(_buffer, path, rid);
        seekWTCursorAtOrPast(_buffer, /*exactOnly*/ true);
        return curr();
    }

    void save() override {
        if (!_eof && !_lastMoveSkippedKey) {
            WT_ITEM key;
            WT_CURSOR* c = _cursor->get();
            if (c->get_key(c, &key) == 0) {
                _buffer.assign(static_cast<const char*>(key.data), key.size);
            } else {
                _buffer.clear();
            }
        }
        resetCursor();
    }
    void saveUnpositioned() override {
        resetCursor();
        _buffer.clear();
        _eof = true;
    }

    void restore() override {
        if (!_cursor) {
            _cursor.emplace(*WiredTigerRecoveryUnit::get(_opCtx), _uri, _tableId, false);
        }

        // Ensure an active session exists, so any restored cursors will bind to it
        invariant(WiredTigerRecoveryUnit::get(_opCtx)->getSession() == _cursor->getSession());

        if (!_eof) {
            // Check if the exact search key stashed in _buffer was not found.
            _lastMoveSkippedKey = !seekWTCursorAtOrPast(_buffer);
        }
    }

    void detachFromOperationContext() override {
        WiredTigerIndexCursorGeneric::detachFromOperationContext();
    }
    void reattachToOperationContext(OperationContext* opCtx) override {
        WiredTigerIndexCursorGeneric::reattachToOperationContext(opCtx);
    }
    void setSaveStorageCursorOnDetachFromOperationContext(bool detach) {
        WiredTigerIndexCursorGeneric::setSaveStorageCursorOnDetachFromOperationContext(detach);
    }

    /**
     *  Returns the checkpoint ID for checkpoint cursors, otherwise 0.
     */
    uint64_t getCheckpointId() const override {
        return _cursor->getCheckpointId();
    }

private:
    void resetCursor() {
        WiredTigerIndexCursorGeneric::resetCursor();
    }

    /**
     * Helper function to iterate the cursor to the given 'searchKey', or the closest key
     * immediately after the 'searchKey' if 'searchKey' does not exist and 'exactOnly' is false.
     * Returns true if an exact match is found.
     */
    bool seekWTCursorAtOrPast(const std::string& searchKey, bool exactOnly = false) {
        // Ensure an active transaction is open.
        WiredTigerRecoveryUnit::get(_opCtx)->getSession();

        WT_CURSOR* c = _cursor->get();

        if (searchKey.empty()) {
            return false;
        }

        const WiredTigerItem searchKeyItem(searchKey);
        c->set_key(c, searchKeyItem.Get());

        int cmp = 0;
        int ret = wiredTigerPrepareConflictRetry(
            _opCtx, [&] { return exactOnly ? c->search(c) : c->search_near(c, &cmp); });
        if (ret == WT_NOTFOUND) {
            _eof = true;
            return false;
        }
        invariantWTOK(ret, c->session);
        _eof = false;

        auto& metricsCollector = ResourceConsumption::MetricsCollector::get(_opCtx);
        metricsCollector.incrementOneCursorSeek(c->uri);

        // Make sure we land on a key matching the search key or a key immediately after.
        //
        // If this operation is ignoring prepared updates and WT::search_near() lands on a key that
        // compares lower than the search key, calling next() is not guaranteed to return a key that
        // compares greater than the search key. This is because ignoring prepare conflicts does not
        // provide snapshot isolation and the call to next() may land on a newly-committed prepared
        // entry. We must advance our cursor until we find a key that compares greater than the
        // search key. See SERVER-56839.
        //
        // Note: the problem described above is currently not possible for column indexes because
        // (a) There is a special path in the column index present with the "path" value 0xFF, which
        // is greater than all other paths and (b) there is incidental behavior in the
        // WT::search_near() function. To elaborate on (b), if WT::search_near() doesn't find an
        // exact match, it will 'prefer' to return the following key/value, which is guaranteed to
        // exist because of (a). However, the contract of search_near is that it may return either
        // the previous or the next value.
        //
        // (a) is unlikely to change, but (b) is incidental behavior. To avoid relying on this, we
        // iterate the cursor until we find a value that is greater than or equal to the search key.
        const bool enforcingPrepareConflicts =
            shard_role_details::getRecoveryUnit(_opCtx)->getPrepareConflictBehavior() ==
            PrepareConflictBehavior::kEnforce;
        WT_ITEM curKey;
        while (cmp < 0) {
            _eof = !advanceWTCursor();

            if (_eof) {
                break;
            }

            if (!kDebugBuild && enforcingPrepareConflicts) {
                break;
            }

            getKey(c, &curKey, &metricsCollector);
            cmp = std::memcmp(
                curKey.data, searchKeyItem.data, std::min(searchKeyItem.size, curKey.size));

            LOGV2(6609700,
                  "Column store index {idxName} cmp after advance: {cmp}",
                  "cmp"_attr = cmp,
                  "idxName"_attr = _indexName);

            if (enforcingPrepareConflicts) {
                // If we are enforcing prepare conflicts, calling next() must always give us a key
                // that compares greater than than our search key. An exact match is also possible
                // in the case of _id indexes, because the recordid is not a part of the key.
                dassert(cmp >= 0);
            }
        }

        return cmp == 0;
    }

    boost::optional<FullCellView> curr() {
        boost::optional<FullCellView> out;
        _lastMoveSkippedKey = false;
        if (_eof) {
            return out;
        }

        auto* c = _cursor->get();
        WT_ITEM key;
        WT_ITEM value;
        out.emplace();
        c->get_raw_key_value(c, &key, &value);
        size_t nullByteSize = 1;
        invariant(key.size >= 1);
        // If we end up reserving more values, like kRowIdPath, this check should be changed to
        // include the newly reserved values.
        if (static_cast<const char*>(key.data)[0] == '\xFF' /* kRowIdPath */) {
            nullByteSize = 0;
            out->path = PathView("\xFF"_sd);
        } else {
            out->path = PathView(static_cast<const char*>(key.data));
            out->value = CellView(static_cast<const char*>(value.data), value.size);
        }
        const auto ridSize = key.size - out->path.size() - nullByteSize;
        const auto ridStart = static_cast<const char*>(key.data) + out->path.size() + nullByteSize;

        invariant(ridSize == 8);
        out->rid = ConstDataView(ridStart).read<BigEndian<int64_t>>();
        return out;
    }

    std::string _buffer;
    bool _eof = false;

    // Used by next to decide to return current position rather than moving. Should be reset to
    // false by any operation that moves the cursor, other than subsequent save/restore pairs.
    bool _lastMoveSkippedKey = false;

    std::string _uri;
    uint64_t _tableId = 0;
    std::string _indexName;
};

std::unique_ptr<ColumnStore::Cursor> WiredTigerColumnStore::newCursor(
    OperationContext* opCtx) const {
    return std::make_unique<Cursor>(opCtx, this);
}

class WiredTigerColumnStore::BulkBuilder final : public ColumnStore::BulkBuilder {
public:
    BulkBuilder(WiredTigerColumnStore* idx, OperationContext* opCtx)
        : _opCtx(opCtx), _cursor(*WiredTigerRecoveryUnit::get(opCtx), idx->uri()) {}

    void addCell(PathView path, RowId rid, CellView cell) override {
        const std::string& key = makeKeyInBuffer(_buffer, path, rid);
        WiredTigerItem keyItem(key.c_str(), key.size());
        _cursor->set_key(_cursor.get(), keyItem.Get());

        WiredTigerItem cellItem(cell.rawData(), cell.size());
        _cursor->set_value(_cursor.get(), cellItem.Get());

        invariantWTOK(wiredTigerCursorInsert(*WiredTigerRecoveryUnit::get(_opCtx), _cursor.get()),
                      _cursor->session);

        ResourceConsumption::MetricsCollector::get(_opCtx).incrementOneIdxEntryWritten(
            _cursor->uri, keyItem.size);
    }

private:
    std::string _buffer;
    OperationContext* const _opCtx;
    WiredTigerBulkLoadCursor _cursor;
};

std::unique_ptr<ColumnStore::BulkBuilder> WiredTigerColumnStore::makeBulkBuilder(
    OperationContext* opCtx) {
    return std::make_unique<BulkBuilder>(this, opCtx);
}

bool WiredTigerColumnStore::isEmpty(OperationContext* opCtx) {
    return WiredTigerIndexUtil::isEmpty(opCtx, _uri, _tableId);
}

long long WiredTigerColumnStore::getSpaceUsedBytes(OperationContext* opCtx) const {
    dassert(shard_role_details::getLocker(opCtx)->isReadLocked());
    auto ru = WiredTigerRecoveryUnit::get(opCtx);
    WT_SESSION* s = ru->getSession()->getSession();

    if (ru->getSessionCache()->isEphemeral()) {
        return static_cast<long long>(WiredTigerUtil::getEphemeralIdentSize(s, _uri));
    }
    return static_cast<long long>(WiredTigerUtil::getIdentSize(s, _uri));
}

long long WiredTigerColumnStore::getFreeStorageBytes(OperationContext* opCtx) const {
    dassert(shard_role_details::getLocker(opCtx)->isReadLocked());
    auto ru = WiredTigerRecoveryUnit::get(opCtx);
    WiredTigerSession* session = ru->getSession();

    return static_cast<long long>(WiredTigerUtil::getIdentReuseSize(session->getSession(), _uri));
}

StatusWith<int64_t> WiredTigerColumnStore::compact(OperationContext* opCtx,
                                                   const CompactOptions& options) {
    return WiredTigerIndexUtil::compact(*opCtx, *WiredTigerRecoveryUnit::get(opCtx), _uri, options);
}

bool WiredTigerColumnStore::appendCustomStats(OperationContext* opCtx,
                                              BSONObjBuilder* output,
                                              double scale) const {
    return WiredTigerIndexUtil::appendCustomStats(
        *WiredTigerRecoveryUnit::get(opCtx), output, scale, _uri);
}

}  // namespace mongo
