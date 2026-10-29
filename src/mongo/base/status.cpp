// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/status.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/str.h"

#include <exception>
#include <ostream>
#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {

Status& Status::addContext(std::string_view reasonPrefix) {
    if (!isOK()) {
        boost::intrusive_ptr oldInfo = std::move(_error);
        const Status::ErrorInfo& e = *oldInfo;
        _error = _createErrorInfo(
            e.code, fmt::format("{}{}", reasonPrefix, causedBy(e.reason)), e.extra);
    }
    return *this;
}

std::string Status::toString() const {
    StringBuilder sb;
    sb << *this;
    return sb.str();
}

void Status::serialize(BSONObjBuilder* builder) const {
    builder->append("code", code());
    builder->append("codeName", ErrorCodes::errorString(code()));
    if (!isOK()) {
        builder->append("errmsg", reason());
        if (const auto& ei = extraInfo())
            ei->serialize(builder);
    }
}

boost::intrusive_ptr<const Status::ErrorInfo> Status::_createErrorInfo(
    ErrorCodes::Error code, std::string reason, std::shared_ptr<const ErrorExtraInfo> extra) {
    if (code == ErrorCodes::OK)
        return nullptr;
    if (extra) {
        // The public API prevents getting into this state.
        invariant(ErrorCodes::canHaveExtraInfo(code));
    } else if (ErrorCodes::mustHaveExtraInfo(code)) {
        // If an ErrorExtraInfo class is non-optional, return an error.

        // This is possible if code calls a 2-argument Status constructor with a code that should
        // have extra info.
        if (kDebugBuild) {
            // Make it easier to find this issue by fatally failing in debug builds.
            LOGV2_FATAL(40680, "Code {code} is supposed to have extra info", "code"_attr = code);
        }

        // In release builds, replace the error code. This maintains the invariant that all Statuses
        // for a code that is supposed to hold extra info hold correctly-typed extra info, without
        // crashing the server.
        return make_intrusive<ErrorInfo>(
            ErrorCodes::Error(40671),
            str::stream() << "Missing required extra info for error code " << code,
            std::move(extra));
    }
    return make_intrusive<ErrorInfo>(code, std::move(reason), std::move(extra));
}

boost::intrusive_ptr<const Status::ErrorInfo> Status::_parseErrorInfo(ErrorCodes::Error code,
                                                                      std::string reason,
                                                                      const BSONObj& extraObj) {
    std::shared_ptr<const ErrorExtraInfo> extra;
    if (auto parser = ErrorExtraInfo::parserFor(code)) {
        try {
            extra = parser(extraObj);
        } catch (const DBException& ex) {
            if (ErrorCodes::mustHaveExtraInfo(code)) {
                return ex.toStatus(str::stream() << "Error parsing extra info for " << code)._error;
            }
        }
    }
    return _createErrorInfo(code, std::move(reason), std::move(extra));
}

std::ostream& Status::_streamTo(std::ostream& os) const {
    os << codeString();
    if (!isOK())
        os << " " << reason();
    return os;
}

StringBuilder& Status::_streamTo(StringBuilder& sb) const {
    sb << codeString();
    if (!isOK()) {
        try {
            if (const auto& extra = extraInfo()) {
                BSONObjBuilder bob;
                extra->serialize(&bob);
                sb << bob.done();
            }
        } catch (const DBException&) {
            // This really shouldn't happen but it would be really annoying if it broke error
            // logging in production.
            if (kDebugBuild) {
                LOGV2_FATAL_CONTINUE(
                    23806,
                    "Error serializing extra info for {status_code} in Status::toString()",
                    "status_code"_attr = code());
                std::terminate();
            }
        }
        sb << ": " << reason();
    }
    return sb;
}

}  // namespace mongo
