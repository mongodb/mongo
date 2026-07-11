// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"

#include <memory>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Contains information about an Incremental Feature Rollout (IFR) flag that was disabled
 * during a command retry. This is primarily used for shard-router communication when retrying
 * an aggregate command with previously-enabled IFR flags disabled.
 */
class IFRFlagRetryInfo final : public ErrorExtraInfo {
public:
    // Required member of every ErrorExtraInfo.
    static constexpr auto code = ErrorCodes::IFRFlagRetry;

    explicit IFRFlagRetryInfo(std::string disabledFlagName)
        : _disabledFlagName(std::move(disabledFlagName)) {}

    const std::string& getDisabledFlagName() const {
        return _disabledFlagName;
    }

    void serialize(BSONObjBuilder* bob) const override;
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj& obj);

private:
    std::string _disabledFlagName;
};

}  // namespace mongo
