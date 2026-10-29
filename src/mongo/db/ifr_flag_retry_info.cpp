// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/ifr_flag_retry_info.h"

#include "mongo/base/init.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"

#include <string_view>

namespace mongo {
namespace {

constexpr std::string_view kDisabledFlagFieldName = "disabledFlagName";
MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(IFRFlagRetryInfo);

}  // namespace

void IFRFlagRetryInfo::serialize(BSONObjBuilder* bob) const {
    bob->append(kDisabledFlagFieldName, _disabledFlagName);
}

std::shared_ptr<const ErrorExtraInfo> IFRFlagRetryInfo::parse(const BSONObj& obj) {
    const auto& disabledFlagElement = obj.getField(kDisabledFlagFieldName);
    uassert(11577000,
            fmt::format("IFRFlagRetryInfo was missing '{}' field", kDisabledFlagFieldName),
            !disabledFlagElement.eoo());
    return std::make_shared<IFRFlagRetryInfo>(
        std::string(disabledFlagElement.checkAndGetStringData()));
}

}  // namespace mongo
