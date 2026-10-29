// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>
#include <utility>

namespace mongo {

/**
 * This exception is a workaround for the cases where switch from Shard to Router Role happens
 * without releasing/stashing Shard Role's transaction resources (locks, recovery unit, etc). No new
 * usages (throwers or catchers) should be added without consultation with the Shard Local Catalog
 * Team (CAR).
 *
 * TODO (SPM-3971): Remove this exception once its last user has stopped relying on it.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ShardCannotRefreshDueToLocksHeldInfo final
    : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::ShardCannotRefreshDueToLocksHeld;

    ShardCannotRefreshDueToLocksHeldInfo(NamespaceString nss) : _nss(std::move(nss)) {}

    const auto& getNss() const {
        return _nss;
    }

    void serialize(BSONObjBuilder* bob) const override;

    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj& obj);

    static ShardCannotRefreshDueToLocksHeldInfo parseFromCommandError(const BSONObj& obj);

private:
    static constexpr std::string_view kNssFieldName{"nss"};

    const NamespaceString _nss;
};

using ShardCannotRefreshDueToLocksHeldException [[MONGO_MOD_NEEDS_REPLACEMENT]] =
    ExceptionFor<ErrorCodes::ShardCannotRefreshDueToLocksHeld>;

}  // namespace mongo
