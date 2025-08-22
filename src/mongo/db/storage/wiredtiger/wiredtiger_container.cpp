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

WiredTigerContainer::WiredTigerContainer(std::string uri, uint64_t tableId)
    : _uri(std::move(uri)), _tableId(tableId) {}

StringData WiredTigerContainer::uri() const {
    return _uri;
}

uint64_t WiredTigerContainer::tableId() const {
    return _tableId;
}

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
    return wtRCToStatus(insert(wtRu, *cursor.get(), key, value), cursor->session);
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
    return wtRCToStatus(remove(wtRu, *cursor.get(), key), cursor->session);
}

int WiredTigerIntegerKeyedContainer::remove(WiredTigerRecoveryUnit& ru,
                                            WT_CURSOR& cursor,
                                            int64_t key) {
    cursor.set_key(&cursor, key);
    return WT_OP_CHECK(wiredTigerCursorRemove(ru, &cursor));
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
    return wtRCToStatus(insert(wtRu, *cursor.get(), key, value), cursor->session);
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
    return wtRCToStatus(remove(wtRu, *cursor.get(), key), cursor->session);
}

int WiredTigerStringKeyedContainer::remove(WiredTigerRecoveryUnit& ru,
                                           WT_CURSOR& cursor,
                                           std::span<const char> key) {
    cursor.set_key(&cursor, WiredTigerItem{key}.get());
    return WT_OP_CHECK(wiredTigerCursorRemove(ru, &cursor));
}

}  // namespace mongo
