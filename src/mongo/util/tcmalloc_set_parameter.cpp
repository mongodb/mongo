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

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <string>
#include <valgrind/valgrind.h>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/parse_number.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/str.h"
#include "mongo/util/tcmalloc_parameters_gen.h"

#ifdef MONGO_CONFIG_TCMALLOC_GOOGLE
#include <tcmalloc/malloc_extension.h>
#elif defined(MONGO_CONFIG_TCMALLOC_GPERF)
#include <gperftools/malloc_extension.h>
#endif

namespace mongo {
namespace {

constexpr absl::string_view toStringView(StringData s) {
    return {s.rawData(), s.size()};
}

constexpr auto kMaxTotalThreadCacheBytesPropertyName = "tcmalloc.max_total_thread_cache_bytes"_sd;
constexpr auto kAggressiveMemoryDecommitPropertyName = "tcmalloc.aggressive_memory_decommit"_sd;

#if defined(MONGO_CONFIG_TCMALLOC_GPERF)
StatusWith<size_t> getProperty(StringData propname) {
    size_t value;
    if (!MallocExtension::instance()->GetNumericProperty(propname.toString().c_str(), &value)) {
        return {ErrorCodes::InternalError,
                str::stream() << "Failed to retreive tcmalloc prop: " << propname};
    }
    return value;
}

Status setProperty(StringData propname, size_t value) {
    if (!RUNNING_ON_VALGRIND) {  // NOLINT
        if (!MallocExtension::instance()->SetNumericProperty(propname.toString().c_str(), value)) {
            return {ErrorCodes::InternalError,
                    str::stream() << "Failed to set internal tcmalloc property " << propname};
        }
    }
    return Status::OK();
}
#endif

void setMaxTotalThreadCacheBytes(size_t cacheSize) {
#if defined(MONGO_CONFIG_TCMALLOC_GOOGLE)
    tcmalloc::MallocExtension::SetMaxTotalThreadCacheBytes(cacheSize);
#elif defined(MONGO_CONFIG_TCMALLOC_GPERF)
    uassertStatusOK(setProperty(kMaxTotalThreadCacheBytesPropertyName, cacheSize));
#endif  // MONGO_CONFIG_TCMALLOC_GPERF
}


#ifdef MONGO_CONFIG_TCMALLOC_GOOGLE
// Implement abstraction for the differences between gperftools and new tcmalloc.
bool getNumericProperty(absl::string_view key, size_t* val) {
    auto optVal = tcmalloc::MallocExtension::GetNumericProperty(key);
    if (!optVal)
        return false;
    *val = *optVal;
    return true;
}

StatusWith<size_t> getProperty(StringData propname) {
    size_t value;
    if (!getNumericProperty(propname.toString().c_str(), &value)) {
        return {ErrorCodes::InternalError,
                str::stream() << "Failed to retreive tcmalloc prop: " << propname};
    }
    return value;
}

bool setNumericProperty(absl::string_view key, size_t val) {
    if (key == toStringView(kMaxTotalThreadCacheBytesPropertyName)) {
        setMaxTotalThreadCacheBytes(val);
        return true;
    }
    return false;
}

Status setProperty(StringData propname, size_t value) {
    if (!RUNNING_ON_VALGRIND) {  // NOLINT
        if (!setNumericProperty(propname.toString().c_str(), value)) {
            return {ErrorCodes::InternalError,
                    str::stream() << "Failed to set internal tcmalloc property " << propname};
        }
    }
    return Status::OK();
}

long long getMemoryReleaseRate() {
    return static_cast<size_t>(tcmalloc::MallocExtension::GetBackgroundReleaseRate());
}

bool setMemoryReleaseRate(size_t val) {
    tcmalloc::MallocExtension::SetBackgroundReleaseRate(
        tcmalloc::MallocExtension::BytesPerSecond{val});
    return true;
}

#endif


StatusWith<size_t> validateTCMallocValue(StringData name, const BSONElement& newValueElement) {
    if (!newValueElement.isNumber()) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << "Expected server parameter " << name
                              << " to have numeric type, but found "
                              << newValueElement.toString(false) << " of type "
                              << typeName(newValueElement.type())};
    }
    long long valueAsLongLong = newValueElement.safeNumberLong();
    if (valueAsLongLong < 0 ||
        static_cast<unsigned long long>(valueAsLongLong) > std::numeric_limits<size_t>::max()) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "Value " << newValueElement.toString(false) << " is out of range for "
                          << name << "; expected a value between 0 and "
                          << std::min<unsigned long long>(std::numeric_limits<size_t>::max(),
                                                          std::numeric_limits<long long>::max()));
    }
    return static_cast<size_t>(valueAsLongLong);
}

}  // namespace

#define TCMALLOC_SP_METHODS(cls)                                                                   \
    void TCMalloc##cls##ServerParameter::append(                                                   \
        OperationContext*, BSONObjBuilder* b, StringData name, const boost::optional<TenantId>&) { \
        auto swValue = getProperty(k##cls##PropertyName);                                          \
        if (swValue.isOK()) {                                                                      \
            b->appendNumber(name, static_cast<long long>(swValue.getValue()));                     \
        }                                                                                          \
    }                                                                                              \
    Status TCMalloc##cls##ServerParameter::set(const BSONElement& newValueElement,                 \
                                               const boost::optional<TenantId>&) {                 \
        auto swValue = validateTCMallocValue(name(), newValueElement);                             \
        if (!swValue.isOK()) {                                                                     \
            return swValue.getStatus();                                                            \
        }                                                                                          \
        return setProperty(k##cls##PropertyName, swValue.getValue());                              \
    }                                                                                              \
    Status TCMalloc##cls##ServerParameter::setFromString(StringData str,                           \
                                                         const boost::optional<TenantId>&) {       \
        size_t value;                                                                              \
        Status status = NumberParser{}(str, &value);                                               \
        if (!status.isOK()) {                                                                      \
            return status;                                                                         \
        }                                                                                          \
        return setProperty(k##cls##PropertyName, value);                                           \
    }

TCMALLOC_SP_METHODS(MaxTotalThreadCacheBytes)
TCMALLOC_SP_METHODS(AggressiveMemoryDecommit)
#undef TCMALLOC_SP_METHODS

namespace {

MONGO_INITIALIZER_GENERAL(TcmallocConfigurationDefaults, (), ("BeginStartupOptionHandling"))
(InitializerContext*) {
    // Before processing the command line options, if the user has not specified a value in via
    // the environment, set tcmalloc.max_total_thread_cache_bytes to its default value.
    if (getenv("TCMALLOC_MAX_TOTAL_THREAD_CACHE_BYTES")) {
        return;
    }

    ProcessInfo pi;
    size_t systemMemorySizeMB = pi.getMemSizeMB();
    size_t defaultTcMallocCacheSize = 1024 * 1024 * 1024;  // 1024MB in bytes
    size_t derivedTcMallocCacheSize =
        (systemMemorySizeMB / 8) * 1024 * 1024;  // 1/8 of system memory in bytes
    size_t cacheSize = std::min(defaultTcMallocCacheSize, derivedTcMallocCacheSize);

    setMaxTotalThreadCacheBytes(cacheSize);

#if defined(MONGO_CONFIG_TCMALLOC_GOOGLE)
    size_t numCores = pi.getNumAvailableCores();
    // 1024MB in bytes spread across cores.
    size_t defaultTcMallocPerCPUCacheSize = (1024 * 1024 * 1024) / numCores;
    size_t derivedTcMallocPerCPUCacheSize =
        ((systemMemorySizeMB / 8) * 2 * 1024 * 1024) / numCores;  // 1/4 of system memory in bytes

    size_t perCPUCacheSize =
        std::min(defaultTcMallocPerCPUCacheSize, derivedTcMallocPerCPUCacheSize);

    tcmalloc::MallocExtension::SetMaxPerCpuCacheSize(perCPUCacheSize);
#endif  // MONGO_CONFIG_TCMALLOC_GOOGLE
}

}  // namespace

// setParameter for tcmalloc_release_rate
void TCMallocReleaseRateServerParameter::append(OperationContext*,
                                                BSONObjBuilder* builder,
                                                StringData fieldName,
                                                const boost::optional<TenantId>&) {
#if defined(MONGO_CONFIG_TCMALLOC_GOOGLE)
    auto value = getMemoryReleaseRate();
#elif defined(MONGO_CONFIG_TCMALLOC_GPERF)
    auto value = MallocExtension::instance()->GetMemoryReleaseRate();
#endif
    builder->append(fieldName, value);
}

Status TCMallocReleaseRateServerParameter::setFromString(StringData tcmalloc_release_rate,
                                                         const boost::optional<TenantId>&) {
    double value;
    Status status = NumberParser{}(tcmalloc_release_rate, &value);
    if (!status.isOK()) {
        return status;
    }
    if (value < 0) {
        return {ErrorCodes::BadValue,
                str::stream() << "tcmallocReleaseRate cannot be negative: "
                              << tcmalloc_release_rate};
    }
#if defined(MONGO_CONFIG_TCMALLOC_GOOGLE)
    setMemoryReleaseRate(value);
#elif defined(MONGO_CONFIG_TCMALLOC_GPERF)
    MallocExtension::instance()->SetMemoryReleaseRate(value);
#endif
    return Status::OK();
}

}  // namespace mongo
