// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/test_harness_helper.h"
#include "mongo/util/modules.h"

#include <functional>
#include <memory>
#include <string>

namespace mongo {

class RecordStore;

class RecoveryUnitHarnessHelper : public HarnessHelper {
public:
    std::unique_ptr<RecoveryUnit> newRecoveryUnit() override = 0;
    virtual std::unique_ptr<RecordStore> createRecordStore(OperationContext* opCtx,
                                                           const std::string& ns) = 0;
};

void registerRecoveryUnitHarnessHelperFactory(
    std::function<std::unique_ptr<RecoveryUnitHarnessHelper>()> factory);

std::unique_ptr<RecoveryUnitHarnessHelper> newRecoveryUnitHarnessHelper();
}  // namespace mongo
