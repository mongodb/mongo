// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/storage/container_base.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_cursor.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/util/modules.h"

#include <string_view>

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
        Cursor(RecoveryUnit& ru, uint64_t tableId, std::string_view uri);

        boost::optional<std::span<const char>> find(int64_t key) final;

        boost::optional<std::pair<int64_t, std::span<const char>>> next() final;

    private:
        WiredTigerCursor _cursor;
        bool _done = false;
    };

    WiredTigerIntegerKeyedContainer(std::shared_ptr<Ident> ident,
                                    std::string uri,
                                    uint64_t tableId);

    Status insert(RecoveryUnit& ru,
                  int64_t key,
                  std::span<const char> value,
                  container::ExistingKeyPolicy policy) final;

    int insert(WiredTigerRecoveryUnit& ru,
               WT_CURSOR& cursor,
               int64_t key,
               std::span<const char> value);

    Status update(RecoveryUnit& ru, int64_t key, std::span<const char> value) final;

    int update(WiredTigerRecoveryUnit& ru,
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
        Cursor(RecoveryUnit& ru, uint64_t tableId, std::string_view uri);

        boost::optional<std::span<const char>> find(std::span<const char> key) final;

        boost::optional<std::pair<std::span<const char>, std::span<const char>>> next() final;

    private:
        WiredTigerCursor _cursor;
        bool _done = false;
    };

    WiredTigerStringKeyedContainer(std::shared_ptr<Ident> ident, std::string uri, uint64_t tableId);

    Status insert(RecoveryUnit& ru,
                  std::span<const char> key,
                  std::span<const char> value,
                  container::ExistingKeyPolicy policy) final;

    int insert(WiredTigerRecoveryUnit& ru,
               WT_CURSOR& cursor,
               std::span<const char> key,
               std::span<const char> value);

    Status update(RecoveryUnit& ru, std::span<const char> key, std::span<const char> value) final;

    int update(WiredTigerRecoveryUnit& ru,
               WT_CURSOR& cursor,
               std::span<const char> key,
               std::span<const char> value);

    Status remove(RecoveryUnit& ru, std::span<const char> key) final;

    int remove(WiredTigerRecoveryUnit& ru, WT_CURSOR& cursor, std::span<const char> key);

    std::unique_ptr<StringKeyedContainer::Cursor> getCursor(RecoveryUnit& ru) const final;
};

}  // namespace mongo
