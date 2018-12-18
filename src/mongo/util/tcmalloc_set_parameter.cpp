
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#ifdef _WIN32
#define NVALGRIND
#endif

#include "mongo/platform/basic.h"

#include <algorithm>
#include <gperftools/malloc_extension.h>
#include <valgrind/valgrind.h>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/init.h"
#include "mongo/base/parse_number.h"
#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/server_parameters.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/tcmalloc_set_parameter.h"

namespace mongo {
namespace {

void tcmallocServerParameterAppendBSON(StringData tcmallocPropertyName,
                                       OperationContext* opCtx,
                                       BSONObjBuilder* b,
                                       StringData name) {
    size_t value;
    if (MallocExtension::instance()->GetNumericProperty(tcmallocPropertyName.toString().c_str(),
                                                        &value)) {
        b->appendNumber(name, value);
    }
}

Status tcmallocServerParameterSetFromBSON(StringData tcmallocPropertyName,
                                          const BSONElement& newValueElement) {
    if (!newValueElement.isNumber()) {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream() << "Expected server parameter " << newValueElement.fieldName()
                                    << " to have numeric type, but found "
                                    << newValueElement.toString(false)
                                    << " of type "
                                    << typeName(newValueElement.type()));
    }
    long long valueAsLongLong = newValueElement.safeNumberLong();
    if (valueAsLongLong < 0 ||
        static_cast<unsigned long long>(valueAsLongLong) > std::numeric_limits<size_t>::max()) {
        return Status(
            ErrorCodes::BadValue,
            str::stream() << "Value " << newValueElement.toString(false) << " is out of range for "
                          << newValueElement.fieldName()
                          << "; expected a value between 0 and "
                          << std::min<unsigned long long>(std::numeric_limits<size_t>::max(),
                                                          std::numeric_limits<long long>::max()));
    }
    if (!RUNNING_ON_VALGRIND) {
        if (!MallocExtension::instance()->SetNumericProperty(
                tcmallocPropertyName.toString().c_str(), static_cast<size_t>(valueAsLongLong))) {
            return Status(ErrorCodes::InternalError,
                          str::stream() << "Failed to set internal tcmalloc property "
                                        << tcmallocPropertyName);
        }
    }
    return Status::OK();
}

Status tcmallocServerParameterFromString(StringData tcmallocPropertyName, StringData str) {
    long long valueAsLongLong;
    Status status = parseNumberFromString(str, &valueAsLongLong);
    if (!status.isOK()) {
        return status;
    }
    BSONObjBuilder builder;
    // The name of the field is irrelevant in setFromBSON, only its value
    builder.append("ignored", valueAsLongLong);
    return tcmallocServerParameterSetFromBSON(tcmallocPropertyName, builder.done().firstElement());
}

}  // namespace

#define DEFINE_TCMALLOC_FUNCTION(XX, YY)                                \
    Status XX##ServerParameterFromString(StringData str) {              \
        return tcmallocServerParameterFromString(YY, str);              \
    }                                                                   \
    Status XX##ServerParameterSetFromBSON(const BSONElement& element) { \
        return tcmallocServerParameterSetFromBSON(YY, element);         \
    }                                                                   \
    void XX##ServerParameterAppendBSON(                                 \
        OperationContext* opCtx, BSONObjBuilder* b, StringData name) {  \
        tcmallocServerParameterAppendBSON(YY, opCtx, b, name);          \
    }

TCMALLOC_PARAMETER_LIST(DEFINE_TCMALLOC_FUNCTION);

namespace {

MONGO_INITIALIZER_GENERAL(TcmallocConfigurationDefaults,
                          MONGO_NO_PREREQUISITES,
                          ("BeginStartupOptionHandling"))
(InitializerContext*) {
    // Before processing the command line options, if the user has not specified a value in via
    // the environment, set tcmalloc.max_total_thread_cache_bytes to its default value.
    if (getenv("TCMALLOC_MAX_TOTAL_THREAD_CACHE_BYTES")) {
        return Status::OK();
    }

    ProcessInfo pi;
    size_t systemMemorySizeMB = pi.getMemSizeMB();
    size_t defaultTcMallocCacheSize = 1024 * 1024 * 1024;  // 1024MB in bytes
    size_t derivedTcMallocCacheSize =
        (systemMemorySizeMB / 8) * 1024 * 1024;  // 1/8 of system memory in bytes
    size_t cacheSize = std::min(defaultTcMallocCacheSize, derivedTcMallocCacheSize);

    return tcmallocMaxTotalThreadCacheBytesServerParameterFromString(std::to_string(cacheSize));
}

}  // namespace
}  // namespace mongo
