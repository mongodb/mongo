// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/collection_operation_source.h"

#include "mongo/util/assert_util.h"

#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;
std::string_view toString(OperationSource source) {
    static constexpr std::string_view kStandardString = "standard"sv;
    static constexpr std::string_view kFromMigrateString = "from migrate"sv;
    static constexpr std::string_view kTimeseriesInsertString = "time-series insert"sv;
    static constexpr std::string_view kTimeseriesUpdateString = "time-series update"sv;
    static constexpr std::string_view kTimeseriesDeleteString = "time-series delete"sv;

    switch (source) {
        case OperationSource::kStandard:
            return kStandardString;
        case OperationSource::kFromMigrate:
            return kFromMigrateString;
        case OperationSource::kTimeseriesInsert:
            return kTimeseriesInsertString;
        case OperationSource::kTimeseriesUpdate:
            return kTimeseriesUpdateString;
        case OperationSource::kTimeseriesDelete:
            return kTimeseriesDeleteString;
    }

    MONGO_UNREACHABLE_TASSERT(10083501);
}
}  // namespace mongo
