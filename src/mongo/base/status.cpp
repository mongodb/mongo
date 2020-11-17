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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

#include <ostream>
#include <sstream>

#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

Status Status::withContext(StringData reasonPrefix) const {
    return isOK() ? OK() : withReason(reasonPrefix + causedBy(reason()));
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
        return new ErrorInfo{ErrorCodes::Error(40671),
                             str::stream() << "Missing required extra info for error code " << code,
                             std::move(extra)};
    }
    return new ErrorInfo{code, std::move(reason), std::move(extra)};
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
    return os << codeString() << " " << reason();
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
