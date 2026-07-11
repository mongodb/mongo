// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/convert_utils.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {

void doFuzz(const char* data, size_t size) try {
    Value val;
    try {
        val = exec::expression::convert_utils::parseJson(std::string_view{data, size},
                                                         boost::none /*expectedType*/);
    } catch (const ExceptionFor<ErrorCodes::ConversionFailure>&) {
        return;
    }
    // Any input should result in a top-level array, object or a ConversionFailure.
    invariant(val.getType() == BSONType::array || val.getType() == BSONType::object);
} catch (...) {
    LOGV2_FATAL(10508700,
                "Exception",
                "input"_attr = std::string_view{data, size},
                "error"_attr = exceptionToStatus());
}

}  // namespace
}  // namespace mongo

extern "C" int LLVMFuzzerTestOneInput(const char* Data, size_t Size) {
    mongo::doFuzz(Data, Size);
    return 0;
}
