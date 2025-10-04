/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/db/exec/convert_utils.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {

void doFuzz(const char* data, size_t size) try {
    Value val;
    try {
        val = exec::expression::convert_utils::parseJson(StringData{data, size},
                                                         boost::none /*expectedType*/);
    } catch (const ExceptionFor<ErrorCodes::ConversionFailure>&) {
        return;
    }
    // Any input should result in a top-level array, object or a ConversionFailure.
    invariant(val.getType() == BSONType::array || val.getType() == BSONType::object);
} catch (...) {
    LOGV2_FATAL(10508700,
                "Exception",
                "input"_attr = StringData{data, size},
                "error"_attr = exceptionToStatus());
}

}  // namespace
}  // namespace mongo

extern "C" int LLVMFuzzerTestOneInput(const char* Data, size_t Size) {
    mongo::doFuzz(Data, Size);
    return 0;
}
