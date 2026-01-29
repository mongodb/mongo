/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/storage/wiredtiger/wiredtiger_container.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_cursor_helpers.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"

namespace mongo {
namespace {

boost::optional<std::span<const char>> _get(WiredTigerCursor& cursor) {
    WT_ITEM value;
    invariantWTOK(cursor->get_value(cursor.get(), &value), cursor->session);
    return std::span<const char>{static_cast<const char*>(value.data), value.size};
}

boost::optional<std::span<const char>> _find(WiredTigerCursor& cursor) {
    auto ret = cursor->search(cursor.get());
    if (ret == WT_NOTFOUND) {
        return boost::none;
    }
    invariantWTOK(ret, cursor->session);
    return _get(cursor);
}

boost::optional<std::span<const char>> _next(WiredTigerCursor& cursor) {
    auto ret = cursor->next(cursor.get());
    if (ret == WT_NOTFOUND) {
        return boost::none;
    }
    invariantWTOK(ret, cursor->session);
    return _get(cursor);
}

}  // namespace

WiredTigerContainer::WiredTigerContainer(std::string uri, uint64_t tableId)
    : _uri(std::move(uri)), _tableId(tableId) {}

WiredTigerIntegerKeyedContainer::WiredTigerIntegerKeyedContainer(std::shared_ptr<Ident> ident,
                                                                 std::string uri,
                                                                 uint64_t tableId)
    : WiredTigerContainer(std::move(uri), tableId), IntegerKeyedContainerBase(std::move(ident)) {}

Status WiredTigerIntegerKeyedContainer::insert(RecoveryUnit& ru,
                                               int64_t key,
                                               std::span<const char> value) {
    auto& wtRu = WiredTigerRecoveryUnit::get(ru);
    WiredTigerCursor cursor{getWiredTigerCursorParams(wtRu, tableId()), uri(), *wtRu.getSession()};
    wtRu.assertInActiveTxn();
    int ret = insert(wtRu, *cursor.get(), key, value);
    return wtRCToStatus(ret, cursor->session);
}

int WiredTigerIntegerKeyedContainer::insert(WiredTigerRecoveryUnit& ru,
                                            WT_CURSOR& cursor,
                                            int64_t key,
                                            std::span<const char> value) {
    cursor.set_key(&cursor, key);
    cursor.set_value(&cursor, WiredTigerItem{value}.get());
    return WT_OP_CHECK(wiredTigerCursorInsert(ru, &cursor));
}

Status WiredTigerIntegerKeyedContainer::remove(RecoveryUnit& ru, int64_t key) {
    auto& wtRu = WiredTigerRecoveryUnit::get(ru);
    WiredTigerCursor cursor{getWiredTigerCursorParams(wtRu, tableId()), uri(), *wtRu.getSession()};
    wtRu.assertInActiveTxn();
    int ret = remove(wtRu, *cursor.get(), key);
    return wtRCToStatus(ret, cursor->session);
}

int WiredTigerIntegerKeyedContainer::remove(WiredTigerRecoveryUnit& ru,
                                            WT_CURSOR& cursor,
                                            int64_t key) {
    cursor.set_key(&cursor, key);
    return WT_OP_CHECK(wiredTigerCursorRemove(ru, &cursor));
}

std::unique_ptr<IntegerKeyedContainer::Cursor> WiredTigerIntegerKeyedContainer::getCursor(
    RecoveryUnit& ru) const {
    return std::make_unique<Cursor>(ru, tableId(), uri());
}

std::shared_ptr<IntegerKeyedContainer::Cursor> WiredTigerIntegerKeyedContainer::getSharedCursor(
    RecoveryUnit& ru) const {
    return std::make_shared<Cursor>(ru, tableId(), uri());
}

WiredTigerIntegerKeyedContainer::Cursor::Cursor(RecoveryUnit& ru, uint64_t tableId, StringData uri)
    : _cursor(getWiredTigerCursorParams(WiredTigerRecoveryUnit::get(ru), tableId),
              uri,
              *WiredTigerRecoveryUnit::get(ru).getSession()) {}

boost::optional<std::span<const char>> WiredTigerIntegerKeyedContainer::Cursor::find(int64_t key) {
    _cursor->set_key(_cursor.get(), key);
    return _find(_cursor);
}

boost::optional<std::pair<int64_t, std::span<const char>>>
WiredTigerIntegerKeyedContainer::Cursor::next() {
    if (_done) {
        return boost::none;
    }

    auto value = _next(_cursor);
    if (!value) {
        _done = true;
        return boost::none;
    }

    int64_t key;
    invariantWTOK(_cursor->get_key(_cursor.get(), &key), _cursor->session);

    return {{key, *value}};
}

WiredTigerStringKeyedContainer::WiredTigerStringKeyedContainer(std::shared_ptr<Ident> ident,
                                                               std::string uri,
                                                               uint64_t tableId)
    : WiredTigerContainer(std::move(uri), tableId), StringKeyedContainerBase(std::move(ident)) {}

Status WiredTigerStringKeyedContainer::insert(RecoveryUnit& ru,
                                              std::span<const char> key,
                                              std::span<const char> value) {
    auto& wtRu = WiredTigerRecoveryUnit::get(ru);
    WiredTigerCursor cursor{getWiredTigerCursorParams(wtRu, tableId()), uri(), *wtRu.getSession()};
    wtRu.assertInActiveTxn();
    int ret = insert(wtRu, *cursor.get(), key, value);
    return wtRCToStatus(ret, cursor->session);
}

int WiredTigerStringKeyedContainer::insert(WiredTigerRecoveryUnit& ru,
                                           WT_CURSOR& cursor,
                                           std::span<const char> key,
                                           std::span<const char> value) {
    cursor.set_key(&cursor, WiredTigerItem{key}.get());
    cursor.set_value(&cursor, WiredTigerItem{value}.get());
    return WT_OP_CHECK(wiredTigerCursorInsert(ru, &cursor));
}

Status WiredTigerStringKeyedContainer::remove(RecoveryUnit& ru, std::span<const char> key) {
    auto& wtRu = WiredTigerRecoveryUnit::get(ru);
    WiredTigerCursor cursor{getWiredTigerCursorParams(wtRu, tableId()), uri(), *wtRu.getSession()};
    wtRu.assertInActiveTxn();
    int ret = remove(wtRu, *cursor.get(), key);
    return wtRCToStatus(ret, cursor->session);
}

int WiredTigerStringKeyedContainer::remove(WiredTigerRecoveryUnit& ru,
                                           WT_CURSOR& cursor,
                                           std::span<const char> key) {
    cursor.set_key(&cursor, WiredTigerItem{key}.get());
    return WT_OP_CHECK(wiredTigerCursorRemove(ru, &cursor));
}

std::unique_ptr<StringKeyedContainer::Cursor> WiredTigerStringKeyedContainer::getCursor(
    RecoveryUnit& ru) const {
    return std::make_unique<Cursor>(ru, tableId(), uri());
}

std::shared_ptr<StringKeyedContainer::Cursor> WiredTigerStringKeyedContainer::getSharedCursor(
    RecoveryUnit& ru) const {
    return std::make_shared<Cursor>(ru, tableId(), uri());
}

WiredTigerStringKeyedContainer::Cursor::Cursor(RecoveryUnit& ru, uint64_t tableId, StringData uri)
    : _cursor(getWiredTigerCursorParams(WiredTigerRecoveryUnit::get(ru), tableId),
              uri,
              *WiredTigerRecoveryUnit::get(ru).getSession()) {}

boost::optional<std::span<const char>> WiredTigerStringKeyedContainer::Cursor::find(
    std::span<const char> key) {
    _cursor->set_key(_cursor.get(), WiredTigerItem{key}.get());
    return _find(_cursor);
}

boost::optional<std::pair<std::span<const char>, std::span<const char>>>
WiredTigerStringKeyedContainer::Cursor::next() {
    if (_done) {
        return boost::none;
    }

    auto value = _next(_cursor);
    if (!value) {
        _done = true;
        return boost::none;
    }

    WiredTigerItem key;
    invariantWTOK(_cursor->get_key(_cursor.get(), &key), _cursor->session);

    return {{{key.data(), key.size()}, *value}};
}

}  // namespace mongo
