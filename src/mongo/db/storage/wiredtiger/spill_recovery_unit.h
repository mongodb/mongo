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

#include "mongo/db/storage/wiredtiger/wiredtiger_cursor.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"

namespace mongo {

/**
 * This class does not support multi-document transactions or snapshot isolation. Any method calls
 * related to these will trigger an assertion. The primary functionality this class provides is a
 * way to obtain a WiredTigerSession instance via getSession().
 */
class SpillRecoveryUnit final : public WiredTigerRecoveryUnitBase {
public:
    explicit SpillRecoveryUnit(WiredTigerConnection* connection)
        : WiredTigerRecoveryUnitBase(connection) {}

    static SpillRecoveryUnit& get(RecoveryUnit& ru) {
        return checked_cast<SpillRecoveryUnit&>(ru);
    }

    static SpillRecoveryUnit* get(RecoveryUnit* ru) {
        return checked_cast<SpillRecoveryUnit*>(ru);
    }

    WiredTigerSession* getSession() override {
        return getSessionNoTxn();
    }

    Status setTimestamp(Timestamp timestamp) override {
        uasserted(ErrorCodes::CommandNotSupported,
                  "This storage engine does not support timestamps");
    }

    void setOrderedCommit(bool orderedCommit) final {
        uasserted(ErrorCodes::CommandNotSupported,
                  "This storage engine does not support ordering of commits");
    }

    void doAbandonSnapshot() override {
        uasserted(ErrorCodes::CommandNotSupported,
                  "This storage engine does not support snapshot isolation");
    }

    void doBeginUnitOfWork() override {
        uasserted(ErrorCodes::CommandNotSupported,
                  "This storage engine does not support multi-document transactions");
    }

    void doCommitUnitOfWork() override {
        uasserted(ErrorCodes::CommandNotSupported,
                  "This storage engine does not support multi-document transactions");
    }

    void doAbortUnitOfWork() override {
        uasserted(ErrorCodes::CommandNotSupported,
                  "This storage engine does not support multi-document transactions");
    }
};

}  // namespace mongo
