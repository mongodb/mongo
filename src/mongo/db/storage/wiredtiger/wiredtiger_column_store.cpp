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


#include "mongo/platform/basic.h"

#include "mongo/db/global_settings.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_column_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_cursor.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_cursor_helpers.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_customization_hooks.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_index_cursor_generic.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_prepare_conflict.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {
StatusWith<std::string> WiredTigerColumnStore::generateCreateString(
    const std::string& engineName,
    const NamespaceString& collectionNamespace,
    const IndexDescriptor& desc) {
    StringBuilder sb;

    // TODO: SERVER-65487 uncomment this invariant once INDEX_COLUMN is defined.
    // invariant(desc.getIndexType() == INDEX_COLUMN);

    // TODO: SERVER-65976 Tune values used in WT config string.

    // Separate out a prefix and suffix in the default string. User configuration will override
    // values in the prefix, but not values in the suffix.
    sb << "type=file,internal_page_max=16k,leaf_page_max=16k,";
    sb << "checksum=on,";
    sb << "prefix_compression=true,";
    sb << "lsm=(chunk_size=100M),";
    // Ignoring wiredTigerGlobalOptions.useIndexPrefixCompression because we *always* want prefix
    // compression for column indexes.

    sb << "dictionary=128,";
    sb << WiredTigerCustomizationHooks::get(getGlobalServiceContext())
              ->getTableCreateConfig(collectionNamespace.ns());

    // TODO: SERVER-65976 User config goes here.

    // WARNING: No user-specified config can appear below this line. These options are required
    // for correct behavior of the server.

    // Indexes need to store the metadata for collation to work as expected.
    sb << "key_format=u,";
    sb << "value_format=u,";

    // TODO: SERVER-65976 app_metadata goes here (none yet)

    if (WiredTigerUtil::useTableLogging(collectionNamespace)) {
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
                                             bool readOnly)
    : ColumnStore(ident),
      _uri(uri),
      _tableId(WiredTigerSession::genTableId()),
      _desc(desc),
      _indexName(desc->indexName()) {}

std::string& WiredTigerColumnStore::makeKey(std::string& buffer, PathView path, RowId rid) {
    buffer.clear();
    buffer.reserve(path.size() + 1 /*NUL byte*/ + sizeof(RowId));
    buffer += path;
    if (path != kRowIdPath) {
        // If we end up reserving more values, the above check should be changed.
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
        : _opCtx(opCtx), _curwrap(uri, tableId, true /* allow overwrite */, opCtx) {
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
    dassert(_opCtx->lockState()->isWriteLocked());

    auto key = makeKey(path, rid);
    auto keyItem = WiredTigerItem(key);
    auto valueItem = WiredTigerItem(cell.rawData(), cell.size());

    c()->set_key(c(), keyItem.Get());
    c()->set_value(c(), valueItem.Get());
    int ret = WT_OP_CHECK(wiredTigerCursorInsert(_opCtx, c()));

    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(_opCtx);
    metricsCollector.incrementOneIdxEntryWritten(c()->uri, keyItem.size);

    // TODO: SERVER-65978, we may have to specially handle WT_DUPLICATE_KEY error here.
    if (ret) {
        uassertStatusOK(wtRCToStatus(ret, c()->session));
    }
}

void WiredTigerColumnStore::remove(OperationContext* opCtx, PathView path, RowId rid) {
    WriteCursor(opCtx, _uri, _tableId).remove(path, rid);
}
void WiredTigerColumnStore::WriteCursor::remove(PathView path, RowId rid) {
    dassert(_opCtx->lockState()->isWriteLocked());

    auto key = makeKey(path, rid);
    auto keyItem = WiredTigerItem(key);
    c()->set_key(c(), keyItem.Get());
    int ret = WT_OP_CHECK(wiredTigerCursorRemove(_opCtx, c()));
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
    dassert(_opCtx->lockState()->isWriteLocked());

    auto key = makeKey(path, rid);
    auto keyItem = WiredTigerItem(key);
    auto valueItem = WiredTigerItem(cell.rawData(), cell.size());

    c()->set_key(c(), keyItem.Get());
    c()->set_value(c(), valueItem.Get());
    int ret = WT_OP_CHECK(wiredTigerCursorUpdate(_opCtx, c()));

    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(_opCtx);
    metricsCollector.incrementOneIdxEntryWritten(c()->uri, keyItem.size);

    // TODO: SERVER-65978, may want to handle WT_NOTFOUND specially.
    if (ret != 0)
        return uassertStatusOK(wtRCToStatus(ret, c()->session));
}

void WiredTigerColumnStore::fullValidate(OperationContext* opCtx,
                                         int64_t* numKeysOut,
                                         IndexValidateResults* fullResults) const {
    // TODO SERVER-65484: Validation for column indexes.
    // uasserted(ErrorCodes::NotImplemented, "WiredTigerColumnStore::fullValidate()");
    return;
}

class WiredTigerColumnStore::Cursor final : public ColumnStore::Cursor,
                                            public WiredTigerIndexCursorGeneric {
public:
    Cursor(OperationContext* opCtx, const WiredTigerColumnStore* idx)
        : WiredTigerIndexCursorGeneric(opCtx, true /* forward */), _idx(*idx) {
        _cursor.emplace(_idx.uri(), _idx._tableId, false, _opCtx);
    }
    boost::optional<FullCellView> next() override {
        if (_eof) {
            return {};
        }
        if (!_lastMoveSkippedKey) {
            advanceWTCursor();
        }

        return curr();
    }
    boost::optional<FullCellView> seekAtOrPast(PathView path, RowId rid) override {
        makeKey(_buffer, path, rid);
        seekWTCursor();
        return curr();
    }
    boost::optional<FullCellView> seekExact(PathView path, RowId rid) override {
        makeKey(_buffer, path, rid);
        seekWTCursor(/*exactOnly*/ true);
        return curr();
    }

    void save() override {
        if (!_eof && !_lastMoveSkippedKey) {
            WT_ITEM key;
            WT_CURSOR* c = _cursor->get();
            c->get_key(c, &key);
            _buffer.assign(static_cast<const char*>(key.data), key.size);
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
            _cursor.emplace(_idx.uri(), _idx._tableId, false, _opCtx);
        }

        // Ensure an active session exists, so any restored cursors will bind to it
        invariant(WiredTigerRecoveryUnit::get(_opCtx)->getSession() == _cursor->getSession());

        if (!_eof) {
            _lastMoveSkippedKey = !seekWTCursor();
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

private:
    void resetCursor() {
        WiredTigerIndexCursorGeneric::resetCursor();
    }
    bool seekWTCursor(bool exactOnly = false) {
        // Ensure an active transaction is open.
        WiredTigerRecoveryUnit::get(_opCtx)->getSession();

        WT_CURSOR* c = _cursor->get();

        const WiredTigerItem keyItem(_buffer);
        c->set_key(c, keyItem.Get());

        int cmp = 0;
        int ret = wiredTigerPrepareConflictRetry(
            _opCtx, [&] { return exactOnly ? c->search(c) : c->search_near(c, &cmp); });
        if (ret == WT_NOTFOUND) {
            _eof = true;
            return false;
        }
        invariantWTOK(ret, c->session);

        auto& metricsCollector = ResourceConsumption::MetricsCollector::get(_opCtx);
        metricsCollector.incrementOneCursorSeek(c->uri);

        _eof = false;

        // Make sure we land on a matching key at or after.
        if (cmp < 0) {
            advanceWTCursor();
        }

        return cmp == 0;
    }
    void advanceWTCursor() {
        _eof = WiredTigerIndexCursorGeneric::advanceWTCursor();
    }

    boost::optional<FullCellView> curr() {
        boost::optional<FullCellView> out;
        _lastMoveSkippedKey = false;
        if (_eof) {
            return out;
        }

        auto* c = _cursor->get();
        WT_ITEM key;
        out.emplace();
        c->get_key(c, &key);
        size_t nullByteSize = 1;
        invariant(key.size >= 1);
        if (static_cast<const char*>(key.data)[0] == '\xFF') {
            // See also related comment in makeKey().
            nullByteSize = 0;
            out->path = PathView("\xFF"_sd);
        } else {
            WT_ITEM value;
            c->get_value(c, &value);
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

    const WiredTigerColumnStore& _idx;  // not owned
};

std::unique_ptr<ColumnStore::Cursor> WiredTigerColumnStore::newCursor(
    OperationContext* opCtx) const {
    return std::make_unique<Cursor>(opCtx, this);
}

class WiredTigerColumnStore::BulkBuilder final : public ColumnStore::BulkBuilder {
public:
    BulkBuilder(WiredTigerColumnStore* idx, OperationContext* opCtx)
        : _opCtx(opCtx), _cursor(idx->uri(), opCtx) {}

    void addCell(PathView path, RowId rid, CellView cell) override {
        const std::string& key = makeKey(_buffer, path, rid);
        WiredTigerItem keyItem(key.c_str(), key.size());
        _cursor->set_key(_cursor.get(), keyItem.Get());

        WiredTigerItem cellItem(cell.rawData(), cell.size());
        _cursor->set_value(_cursor.get(), cellItem.Get());

        invariantWTOK(wiredTigerCursorInsert(_opCtx, _cursor.get()), _cursor->session);

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
    // TODO: SERVER-65980, this logic could be shared with WiredTigerIndex::isEmpty().
    WiredTigerCursor curwrap(_uri, _tableId, false, opCtx);
    WT_CURSOR* c = curwrap.get();
    if (!c)
        return true;
    int ret = wiredTigerPrepareConflictRetry(opCtx, [&] { return c->next(c); });
    if (ret == WT_NOTFOUND)
        return true;
    invariantWTOK(ret, c->session);
    return false;
}

long long WiredTigerColumnStore::getSpaceUsedBytes(OperationContext* opCtx) const {
    // TODO: SERVER-65980.
    // For now we just return  this so that tests can successfully obtain collection-level stats on
    // a collection with a columnstore index.
    return 27017;
}

long long WiredTigerColumnStore::getFreeStorageBytes(OperationContext* opCtx) const {
    // TODO: SERVER-65980.
    // For now we just fake this so that tests can successfully obtain collection-level stats on a
    // collection with a columnstore index.
    return 27017;
}

Status WiredTigerColumnStore::compact(OperationContext* opCtx) {
    // TODO: SERVER-65980.
    uasserted(ErrorCodes::NotImplemented, "WiredTigerColumnStore::compact");
}
bool WiredTigerColumnStore::appendCustomStats(OperationContext* opCtx,
                                              BSONObjBuilder* output,
                                              double scale) const {
    // TODO: SERVER-65980.
    // For now we just skip this so that tests can successfully obtain collection-level stats on a
    // collection with a columnstore index.
    output->append("note"_sd, "columnstore stats are not yet implemented"_sd);
    return true;
}

}  // namespace mongo
