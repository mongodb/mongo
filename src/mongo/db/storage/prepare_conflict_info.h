// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <memory>

namespace mongo {

/**
 * Carries the notification generation sampled before a prepare conflict.
 */
class [[MONGO_MOD_PUBLIC]] PrepareConflictInfo final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::PrepareConflict;

    explicit PrepareConflictInfo(uint64_t generation);

    int64_t generation() const {
        return _generation;
    }

    void serialize(BSONObjBuilder* builder) const override;
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj& obj);

private:
    int64_t _generation;
};

}  // namespace mongo
