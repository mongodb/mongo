/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/generic_argument_util.h"

#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(overrideInternalWriteConcernTimeout);

WriteConcernOptions makeMajorityWriteConcernOptions(WriteConcernOptions::Timeout timeout) {
    static constexpr auto fpFieldName = "wtimeoutMillis"_sd;

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
