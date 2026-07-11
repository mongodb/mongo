// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/modules.h"

#include <memory>
#include <vector>

namespace mongo {

class OperationContext;

class [[MONGO_MOD_OPEN]] RecoveryUnitNoop : public RecoveryUnit {
public:
    bool isNoop() const final {
        return true;
    }

    void setOrderedCommit(bool orderedCommit) final {}

    void validateInUnitOfWork() const final {}

    void doBeginUnitOfWork() override {}

    void doAbandonSnapshot() override {}

    void doCommitUnitOfWork(boost::optional<Timestamp> commitTime) override {
        _executeCommitHandlers(boost::none);
    }

    void doAbortUnitOfWork() override {
        _executeRollbackHandlers();
    }

    void _setIsolation(Isolation) override {}

private:
    std::vector<std::unique_ptr<Change>> _changes;
};

}  // namespace mongo
