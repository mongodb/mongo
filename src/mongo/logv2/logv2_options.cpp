// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/parse_number.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/tenant_id.h"
#include "mongo/logv2/log_util.h"
#include "mongo/logv2/logv2_options_gen.h"
#include "mongo/logv2/ramlog.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string_view>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {
namespace {

size_t getValue(const BSONElement& newValueElement, std::string_view tag) {
    long long newVal;
    bool success = newValueElement.coerce(&newVal);
    uassert(ErrorCodes::BadValue,
            fmt::format("Invalid value for {}: {}", tag, newValueElement.toString()),
            success);
    uassert(ErrorCodes::BadValue,
            fmt::format("Value for {} must not be negative: {}", tag, newVal),
            newVal >= 0);

    return static_cast<size_t>(newVal);
}

size_t getValueFromString(std::string_view strVal, std::string_view tag) {
    long long newVal;
    auto status = NumberParser{}(strVal, &newVal);
    uassert(ErrorCodes::BadValue,
            fmt::format("{} must be a numeric value, {} provided", tag, strVal),
            status.isOK());
    uassert(ErrorCodes::BadValue,
            fmt::format("Value for {} must not be negative: {}", tag, newVal),
            newVal >= 0);

    return static_cast<size_t>(newVal);
}

}  // namespace

void RedactEncryptedFields::append(OperationContext* opCtx,
                                   BSONObjBuilder* b,
                                   std::string_view name,
                                   const boost::optional<TenantId>&) {
    *b << name << logv2::shouldRedactBinDataEncrypt();
}

Status RedactEncryptedFields::set(const BSONElement& newValueElement,
                                  const boost::optional<TenantId>&) {
    bool newVal;
    if (!newValueElement.coerce(&newVal)) {
        return {ErrorCodes::BadValue,
                str::stream() << "Invalid value for redactEncryptedFields: " << newValueElement};
    }

    logv2::setShouldRedactBinDataEncrypt(newVal);
    return Status::OK();
}

Status RedactEncryptedFields::setFromString(std::string_view str,
                                            const boost::optional<TenantId>&) {
    if (str == "true" || str == "1") {
        logv2::setShouldRedactBinDataEncrypt(true);
    } else if (str == "false" || str == "0") {
        logv2::setShouldRedactBinDataEncrypt(false);
    } else {
        return {ErrorCodes::BadValue,
                str::stream() << "Invalid value for redactEncryptedFields: " << str};
    }
    return Status::OK();
}

void RamLogMaxLines::append(OperationContext* opCtx,
                            BSONObjBuilder* b,
                            std::string_view name,
                            const boost::optional<TenantId>&) {
    *b << name << static_cast<long long>(logv2::RamLog::getGlobalMaxLines());
}

Status RamLogMaxLines::set(const BSONElement& newValueElement,
                           const boost::optional<TenantId>&) try {
    logv2::RamLog::setGlobalMaxLines(getValue(newValueElement, "ramLogMaxLines"));
    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus();
}

Status RamLogMaxLines::setFromString(std::string_view str, const boost::optional<TenantId>&) try {
    logv2::RamLog::setGlobalMaxLines(getValueFromString(str, "ramLogMaxLines"));
    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus();
}

void RamLogMaxSizeBytes::append(OperationContext* opCtx,
                                BSONObjBuilder* b,
                                std::string_view name,
                                const boost::optional<TenantId>&) {
    *b << name << static_cast<long long>(logv2::RamLog::getGlobalMaxSizeBytes());
}

Status RamLogMaxSizeBytes::set(const BSONElement& newValueElement,
                               const boost::optional<TenantId>&) try {
    logv2::RamLog::setGlobalMaxSizeBytes(getValue(newValueElement, "ramLogMaxSizeBytes"));
    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus();
}

Status RamLogMaxSizeBytes::setFromString(std::string_view str,
                                         const boost::optional<TenantId>&) try {
    logv2::RamLog::setGlobalMaxSizeBytes(getValueFromString(str, "ramLogMaxSizeBytes"));
    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus();
}

}  // namespace mongo
