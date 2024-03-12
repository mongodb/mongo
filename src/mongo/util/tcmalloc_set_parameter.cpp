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
#include <boost/optional/optional.hpp>
#include <cstdlib>
#include <fmt/format.h>
#include <limits>
#include <valgrind/valgrind.h>

#include "mongo/base/error_codes.h"
#include "mongo/base/initializer.h"
#include "mongo/base/parse_number.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/config.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/tenant_id.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/tcmalloc_parameters_gen.h"
#include "mongo/util/tcmalloc_set_parameter.h"

#ifdef MONGO_CONFIG_TCMALLOC_GOOGLE
#include <tcmalloc/malloc_extension.h>
#elif defined(MONGO_CONFIG_TCMALLOC_GPERF)
#include <gperftools/malloc_extension.h>
#endif  // MONGO_CONFIG_TCMALLOC_GOOGLE

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {

using namespace fmt::literals;

constexpr absl::string_view toStringView(StringData s) {
    return {s.data(), s.size()};
}

StatusWith<size_t> validateTCMallocValue(StringData name, const BSONElement& newValueElement) {
    if (!newValueElement.isNumber()) {
        return {ErrorCodes::TypeMismatch,
                "Expected server parameter {} to have numeric type, but found {} of type {}"_format(
                    name, newValueElement.toString(false), typeName(newValueElement.type()))};
    }
    static constexpr unsigned long long maxOkValue =
        std::min(static_cast<unsigned long long>(std::numeric_limits<size_t>::max()),
                 static_cast<unsigned long long>(std::numeric_limits<long long>::max()));

    auto valueULL = static_cast<unsigned long long>(newValueElement.safeNumberLong());
    if (valueULL > maxOkValue) {
        return Status(ErrorCodes::BadValue,
                      "Value {} is outside range [0, {}]"_format(name, maxOkValue));
    }
    return static_cast<size_t>(valueULL);
}
}  // namespace

#ifdef MONGO_CONFIG_TCMALLOC_GOOGLE
// Although there is only one valid property to get, we use a function to conform to the get/set
// tcmalloc api.
size_t getTcmallocProperty(StringData propName) {
    iassert(ErrorCodes::InternalError,
            "Failed to retreive tcmalloc property: {}"_format(propName),
            propName == kMaxPerCPUCacheSizePropertyName);
    return static_cast<size_t>(tcmalloc::MallocExtension::GetMaxPerCpuCacheSize());
}

// Although there is only one valid property to set, we use a function to conform to the get/set
// tcmalloc api.
void setTcmallocProperty(StringData propName, size_t value) {
    if (!RUNNING_ON_VALGRIND) {  // NOLINT
        iassert(ErrorCodes::InternalError,
                "Failed to set internal tcmalloc property: {}"_format(propName),
                propName == kMaxPerCPUCacheSizePropertyName);
        tcmalloc::MallocExtension::SetMaxPerCpuCacheSize(value);
    }
}

TcmallocReleaseRateT getMemoryReleaseRate() {
    return static_cast<size_t>(tcmalloc::MallocExtension::GetBackgroundReleaseRate());
}

void setMemoryReleaseRate(TcmallocReleaseRateT val) {
    tcmalloc::MallocExtension::SetBackgroundReleaseRate(
        tcmalloc::MallocExtension::BytesPerSecond{static_cast<size_t>(val)});
}

#elif defined(MONGO_CONFIG_TCMALLOC_GPERF)
size_t getTcmallocProperty(StringData propName) {
    size_t value;
    iassert(ErrorCodes::InternalError,
            "Failed to retreive tcmalloc property: {}"_format(propName),
            MallocExtension::instance()->GetNumericProperty(std::string{propName}.c_str(), &value));
    return value;
}

void setTcmallocProperty(StringData propName, size_t value) {
    if (!RUNNING_ON_VALGRIND) {  // NOLINT
        iassert(
            ErrorCodes::InternalError,
            "Failed to set internal tcmalloc property: {}"_format(propName),
            MallocExtension::instance()->SetNumericProperty(std::string{propName}.c_str(), value));
    }
}

TcmallocReleaseRateT getMemoryReleaseRate() {
    return static_cast<size_t>(MallocExtension::instance()->GetMemoryReleaseRate());
}

void setMemoryReleaseRate(TcmallocReleaseRateT val) {
    MallocExtension::instance()->SetMemoryReleaseRate(val);
}
#endif  // MONGO_CONFIG_TCMALLOC_GOOGLE

namespace {
template <typename T>
constexpr StringData kParameterName;

template <>
constexpr StringData kParameterName<TCMallocMaxPerCPUCacheSizeServerParameter> =
    kMaxPerCPUCacheSizePropertyName;

template <>
constexpr StringData kParameterName<TCMallocMaxTotalThreadCacheBytesServerParameter> =
    kMaxTotalThreadCacheBytesPropertyName;

template <>
constexpr StringData kParameterName<TCMallocAggressiveMemoryDecommitServerParameter> =
    kAggressiveMemoryDecommitPropertyName;

template <typename T>
void doAppendProperty(BSONObjBuilder* b, StringData name) {
    try {
        b->appendNumber(name, static_cast<long long>(getTcmallocProperty(kParameterName<T>)));
    } catch (const ExceptionFor<ErrorCodes::InternalError>& ex) {
        LOGV2_ERROR(8646000, "Could not get parameter", "error"_attr = ex.toString());
    }
}

template <typename T>
Status doSetProperty(StringData name, const BSONElement& newValueElement) {
    auto swValue = validateTCMallocValue(name, newValueElement);
    if (!swValue.isOK()) {
        return swValue.getStatus();
    }
    try {
        setTcmallocProperty(kParameterName<T>, swValue.getValue());
        return Status::OK();
    } catch (const ExceptionFor<ErrorCodes::InternalError>& ex) {
        return ex.toStatus();
    }
}

template <typename T>
Status doSetPropertyFromString(StringData str) {
    size_t value;
    Status status = NumberParser{}(str, &value);
    if (!status.isOK()) {
        return status;
    }
    try {
        setTcmallocProperty(kParameterName<T>, value);
        return Status::OK();
    } catch (const ExceptionFor<ErrorCodes::InternalError>& ex) {
        return ex.toStatus();
    }
}
}  // namespace

void TCMallocMaxPerCPUCacheSizeServerParameter::append(OperationContext*,
                                                       BSONObjBuilder* b,
                                                       StringData name,
                                                       const boost::optional<TenantId>&) {
#ifdef MONGO_CONFIG_TCMALLOC_GOOGLE
    doAppendProperty<TCMallocMaxPerCPUCacheSizeServerParameter>(b, name);
#endif  // MONGO_CONFIG_TCMALLOC_GOOGLE
}

Status TCMallocMaxPerCPUCacheSizeServerParameter::set(const BSONElement& newValueElement,
                                                      const boost::optional<TenantId>&) {
#ifdef MONGO_CONFIG_TCMALLOC_GOOGLE
    return doSetProperty<TCMallocMaxPerCPUCacheSizeServerParameter>(name(), newValueElement);
#endif  // MONGO_CONFIG_TCMALLOC_GOOGLE

    LOGV2_WARNING(
        8752700,
        "The tcmallocMaxPerCPUCacheSize server parameter is unavailable when using TCMalloc "
        "with thread caching enabled. Setting this parameter will have no effect.");
    return Status::OK();
}

Status TCMallocMaxPerCPUCacheSizeServerParameter::setFromString(StringData str,
                                                                const boost::optional<TenantId>&) {
#ifdef MONGO_CONFIG_TCMALLOC_GOOGLE
    return doSetPropertyFromString<TCMallocMaxPerCPUCacheSizeServerParameter>(str);
#endif  // MONGO_CONFIG_TCMALLOC_GOOGLE

    LOGV2_WARNING(
        8752701,
        "The tcmallocMaxPerCPUCacheSize server parameter is unavailable when using TCMalloc "
        "with thread caching enabled. Setting this parameter will have no effect.");
    return Status::OK();
}

void TCMallocMaxTotalThreadCacheBytesServerParameter::append(OperationContext*,
                                                             BSONObjBuilder* b,
                                                             StringData name,
                                                             const boost::optional<TenantId>&) {
#ifdef MONGO_CONFIG_TCMALLOC_GPERF
    doAppendProperty<TCMallocMaxTotalThreadCacheBytesServerParameter>(b, name);
#endif  // MONGO_CONFIG_TCMALLOC_GPERF
}

Status TCMallocMaxTotalThreadCacheBytesServerParameter::set(const BSONElement& newValueElement,
                                                            const boost::optional<TenantId>&) {
#ifdef MONGO_CONFIG_TCMALLOC_GPERF
    return doSetProperty<TCMallocMaxTotalThreadCacheBytesServerParameter>(name(), newValueElement);
#endif  // MONGO_CONFIG_TCMALLOC_GPERF

    LOGV2_WARNING(
        8752702,
        "The tcmallocMaxTotalThreadCacheBytes server parameter is unavailable when using TCMalloc "
        "with per-cpu caching enabled. Setting this parameter will have no effect.");
    return Status::OK();
}

Status TCMallocMaxTotalThreadCacheBytesServerParameter::setFromString(
    StringData str, const boost::optional<TenantId>&) {
#ifdef MONGO_CONFIG_TCMALLOC_GPERF
    return doSetPropertyFromString<TCMallocMaxTotalThreadCacheBytesServerParameter>(str);
#endif  // MONGO_CONFIG_TCMALLOC_GPERF

    LOGV2_WARNING(
        8752703,
        "The tcmallocMaxTotalThreadCacheBytes server parameter is unavailable when using TCMalloc "
        "with per-cpu caching enabled. Setting this parameter will have no effect.");
    return Status::OK();
}

void TCMallocAggressiveMemoryDecommitServerParameter::append(OperationContext*,
                                                             BSONObjBuilder* b,
                                                             StringData name,
                                                             const boost::optional<TenantId>&) {
#ifdef MONGO_CONFIG_TCMALLOC_GPERF
    doAppendProperty<TCMallocAggressiveMemoryDecommitServerParameter>(b, name);
#endif  //  MONGO_CONFIG_TCMALLOC_GPERF
}

Status TCMallocAggressiveMemoryDecommitServerParameter::set(const BSONElement& newValueElement,
                                                            const boost::optional<TenantId>&) {
#ifdef MONGO_CONFIG_TCMALLOC_GPERF
    return doSetProperty<TCMallocAggressiveMemoryDecommitServerParameter>(name(), newValueElement);
#endif  //  MONGO_CONFIG_TCMALLOC_GPERF

    LOGV2_WARNING(
        8627600,
        "The tcmallocAggressiveMemoryDecommit server parameter is unavailable when using TCMalloc "
        "with per-CPU caching enabled. Setting this parameter will have no effect.");
    return Status::OK();
}

Status TCMallocAggressiveMemoryDecommitServerParameter::setFromString(
    StringData str, const boost::optional<TenantId>&) {
#ifdef MONGO_CONFIG_TCMALLOC_GPERF
    return doSetPropertyFromString<TCMallocAggressiveMemoryDecommitServerParameter>(str);
#endif  //  MONGO_CONFIG_TCMALLOC_GPERF

    LOGV2_WARNING(
        8627601,
        "The tcmallocAggressiveMemoryDecommit server parameter is unavailable when using TCMalloc "
        "with per-CPU caching enabled. Setting this parameter will have no effect.");
    return Status::OK();
}

void TCMallocReleaseRateServerParameter::append(OperationContext*,
                                                BSONObjBuilder* builder,
                                                StringData fieldName,
                                                const boost::optional<TenantId>&) {
    builder->append(fieldName, getMemoryReleaseRate());
}

Status TCMallocReleaseRateServerParameter::setFromString(StringData tcmallocReleaseRate,
                                                         const boost::optional<TenantId>&) {
    double value;
    Status status = NumberParser{}(tcmallocReleaseRate, &value);
    if (!status.isOK()) {
        return status;
    }
    if (value < 0) {
        return {ErrorCodes::BadValue,
                "tcmallocReleaseRate cannot be negative: {}"_format(tcmallocReleaseRate)};
    }
    setMemoryReleaseRate(value);
    return Status::OK();
}

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

    try {
        setTcmallocProperty(kMaxTotalThreadCacheBytesPropertyName, cacheSize);
    } catch (const ExceptionFor<ErrorCodes::InternalError>& ex) {
        LOGV2_ERROR(8646001,
                    "Could not set tcmallocMaxTotalThreadCacheBytes",
                    "reason"_attr = ex.toString());
    }

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
}  // namespace mongo
