// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/generic_argument_util.h"

#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

MONGO_FAIL_POINT_DEFINE(overrideInternalWriteConcernTimeout);

WriteConcernOptions makeMajorityWriteConcernOptions(WriteConcernOptions::Timeout timeout) {
    static constexpr auto fpFieldName = "wtimeoutMillis"sv;

    auto& fp = overrideInternalWriteConcernTimeout;
    fp.execute([&](const BSONObj& data) {
        try {
            auto elem = data.getField(fpFieldName);
            Milliseconds wTimeout{elem.exactNumberLong()};
            timeout = WriteConcernOptions::Timeout{wTimeout};
        } catch (...) {
            LOGV2_FATAL(10277000,
                        "Missing or invalid parameter",
                        "fieldName"_attr = fpFieldName,
                        "failPoint"_attr = fp.getName());
        }
    });
    return WriteConcernOptions{WriteConcernOptions::kMajority,
                               // Note: Even though we're setting UNSET here, kMajority implies
                               // JOURNAL if journaling is supported by the mongod.
                               WriteConcernOptions::SyncMode::UNSET,
                               timeout};
}

}  // namespace

WriteConcernOptions defaultMajorityWriteConcern() {
    return makeMajorityWriteConcernOptions(WriteConcernOptions::kNoTimeout);
}

WriteConcernOptions defaultMajorityWriteConcernDoNotUse() {
    return makeMajorityWriteConcernOptions(WriteConcernOptions::Timeout{Seconds{60}});
}

}  // namespace mongo
