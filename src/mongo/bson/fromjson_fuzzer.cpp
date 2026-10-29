// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {

void doFuzz(const char* data, size_t size) try {
    BSONObj obj;
    try {
        obj = fromjson(std::string_view(data, size));
    } catch (...) {
        return;  // ignore fromjson exceptions
    }
    invariant(validateBSON(obj.objdata(), obj.objsize()));
} catch (...) {
    LOGV2_FATAL(10041200, "Exception", "error"_attr = exceptionToStatus());
}

}  // namespace
}  // namespace mongo

extern "C" int LLVMFuzzerTestOneInput(const char* Data, size_t Size) {
    mongo::doFuzz(Data, Size);
    return 0;
}
