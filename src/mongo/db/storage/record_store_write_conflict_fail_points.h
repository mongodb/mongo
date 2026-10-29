// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/fail_point.h"
#include "mongo/util/modules.h"

#include <functional>
#include <memory>
#include <string_view>

namespace mongo {

using WriteConflictFailPointFn =
    std::function<std::unique_ptr<FailPointEnableBlock>(FailPoint::ModeOptions)>;

void registerWriteConflictForWritesFactory(std::string_view engineName,
                                           WriteConflictFailPointFn factory);
void registerWriteConflictForReadsFactory(std::string_view engineName,
                                          WriteConflictFailPointFn factory);

[[MONGO_MOD_PUBLIC]] std::unique_ptr<FailPointEnableBlock> enableWriteConflictForWrites(
    FailPoint::ModeOptions mode);
[[MONGO_MOD_PUBLIC]] std::unique_ptr<FailPointEnableBlock> enableWriteConflictForReads(
    FailPoint::ModeOptions mode);

}  // namespace mongo
