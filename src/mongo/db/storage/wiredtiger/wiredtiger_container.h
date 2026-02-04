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

#pragma once

#include "mongo/db/storage/container_base.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_cursor.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/util/modules.h"

#include <wiredtiger.h>

namespace mongo {

class WiredTigerContainer {
public:
    WiredTigerContainer(std::string uri, uint64_t tableId);

    const std::string& uri() const {
        return _uri;
    }

    uint64_t tableId() const {
        return _tableId;
    }

private:
    std::shared_ptr<Ident> _ident;
    std::string _uri;
    uint64_t _tableId;
};

class WiredTigerIntegerKeyedContainer : public WiredTigerContainer,
                                        public IntegerKeyedContainerBase {
public:
    class Cursor : public IntegerKeyedContainerBase::Cursor {
    public:
        Cursor(RecoveryUnit& ru, uint64_t tableId, StringData uri);

        boost::optional<std::span<const char>> find(int64_t key) final;

        boost::optional<std::pair<int64_t, std::span<const char>>> next() final;

    private:
        WiredTigerCursor _cursor;
        bool _done = false;
    };

    WiredTigerIntegerKeyedContainer(std::shared_ptr<Ident> ident,
                                    std::string uri,
                                    uint64_t tableId);

    Status insert(RecoveryUnit& ru, int64_t key, std::span<const char> value) final;

    int insert(WiredTigerRecoveryUnit& ru,
               WT_CURSOR& cursor,
               int64_t key,
               std::span<const char> value);

    Status remove(RecoveryUnit& ru, int64_t key) final;

    int remove(WiredTigerRecoveryUnit& ru, WT_CURSOR& cursor, int64_t key);

    std::unique_ptr<IntegerKeyedContainer::Cursor> getCursor(RecoveryUnit& ru) const final;
};

class WiredTigerStringKeyedContainer : public WiredTigerContainer, public StringKeyedContainerBase {
public:
    class Cursor : public StringKeyedContainerBase::Cursor {
    public:
        Cursor(RecoveryUnit& ru, uint64_t tableId, StringData uri);

        boost::optional<std::span<const char>> find(std::span<const char> key) final;

        boost::optional<std::pair<std::span<const char>, std::span<const char>>> next() final;

    private:
        WiredTigerCursor _cursor;
        bool _done = false;
    };

    WiredTigerStringKeyedContainer(std::shared_ptr<Ident> ident, std::string uri, uint64_t tableId);

    Status insert(RecoveryUnit& ru, std::span<const char> key, std::span<const char> value) final;

    int insert(WiredTigerRecoveryUnit& ru,
               WT_CURSOR& cursor,
               std::span<const char> key,
               std::span<const char> value);

    Status remove(RecoveryUnit& ru, std::span<const char> key) final;

    int remove(WiredTigerRecoveryUnit& ru, WT_CURSOR& cursor, std::span<const char> key);

    std::unique_ptr<StringKeyedContainer::Cursor> getCursor(RecoveryUnit& ru) const final;
};

}  // namespace mongo
