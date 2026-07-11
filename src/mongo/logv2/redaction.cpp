// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/logv2/redaction.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/logv2/log_util.h"
#include "mongo/util/assert_util.h"

#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {

namespace {
using namespace std::literals::string_view_literals;

constexpr auto kRedactionDefaultMask = "###"sv;

}  // namespace

BSONObj redact(const BSONObj& objectToRedact, bool forceRedaction) {
    if (!logv2::shouldRedactLogs() && !forceRedaction) {
        if (!logv2::shouldRedactBinDataEncrypt()) {
            return objectToRedact.redact(BSONObj::RedactLevel::sensitiveOnly);
        }
        return objectToRedact.redact(BSONObj::RedactLevel::encryptedAndSensitive);
    }

    return objectToRedact.redact(BSONObj::RedactLevel::all);
}

std::string_view redact(std::string_view stringToRedact, bool forceRedaction) {
    if (!logv2::shouldRedactLogs() && !forceRedaction) {
        return stringToRedact;
    }

    // Return the default mask.
    return kRedactionDefaultMask;
}

std::string redact(const Status& statusToRedact, bool forceRedaction) {
    if (!logv2::shouldRedactLogs() && !forceRedaction) {
        return statusToRedact.toString();
    }

    // Construct a status representation without the reason()
    StringBuilder sb;
    sb << statusToRedact.codeString();
    if (!statusToRedact.isOK())
        sb << ": " << kRedactionDefaultMask;
    return sb.str();
}

std::string redact(const DBException& exceptionToRedact, bool forceRedaction) {
    if (!logv2::shouldRedactLogs() && !forceRedaction) {
        return exceptionToRedact.toString();
    }

    // Construct an exception representation without the what()
    StringBuilder sb;
    sb << exceptionToRedact.code() << " " << kRedactionDefaultMask;
    return sb.str();
}

}  // namespace mongo
