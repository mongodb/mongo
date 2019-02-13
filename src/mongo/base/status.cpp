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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

#include <ostream>
#include <sstream>

namespace mongo {

Status::ErrorInfo::ErrorInfo(ErrorCodes::Error code,
                             StringData reason,
                             std::shared_ptr<const ErrorExtraInfo> extra)
    : code(code), reason(reason.toString()), extra(std::move(extra)) {}

Status::ErrorInfo* Status::ErrorInfo::create(ErrorCodes::Error code,
                                             StringData reason,
                                             std::shared_ptr<const ErrorExtraInfo> extra) {
    if (code == ErrorCodes::OK)
        return nullptr;
    if (extra) {
        // The public API prevents getting in to this state.
        invariant(ErrorCodes::shouldHaveExtraInfo(code));
    } else if (ErrorCodes::shouldHaveExtraInfo(code)) {
        // This is possible if code calls a 2-argument Status constructor with a code that should
        // have extra info.
        if (kDebugBuild) {
            // Make it easier to find this issue by fatally failing in debug builds.
            severe() << "Code " << code << " is supposed to have extra info";
            fassertFailed(40680);
        }

        // In release builds, replace the error code. This maintains the invariant that all Statuses
        // for a code that is supposed to hold extra info hold correctly-typed extra info, without
        // crashing the server.
        return new ErrorInfo{ErrorCodes::Error(40671),
                             str::stream() << "Missing required extra info for error code " << code,
                             std::move(extra)};
    }
    return new ErrorInfo{code, reason, std::move(extra)};
}


Status::Status(ErrorCodes::Error code,
               StringData reason,
               std::shared_ptr<const ErrorExtraInfo> extra)
    : _error(ErrorInfo::create(code, reason, std::move(extra))) {
    ref(_error);
}

Status::Status(ErrorCodes::Error code, const std::string& reason) : Status(code, reason, nullptr) {}
Status::Status(ErrorCodes::Error code, const char* reason)
    : Status(code, StringData(reason), nullptr) {}
Status::Status(ErrorCodes::Error code, StringData reason) : Status(code, reason, nullptr) {}

Status::Status(ErrorCodes::Error code, StringData reason, const BSONObj& extraInfoHolder)
    : Status(OK()) {
    if (auto parser = ErrorExtraInfo::parserFor(code)) {
        try {
            *this = Status(code, reason, parser(extraInfoHolder));
        } catch (const DBException& ex) {
            *this = ex.toStatus(str::stream() << "Error parsing extra info for " << code);
        }
    } else {
        *this = Status(code, reason);
    }
}

Status::Status(ErrorCodes::Error code, const mongoutils::str::stream& reason)
    : Status(code, std::string(reason)) {}

Status Status::withReason(StringData newReason) const {
    return isOK() ? OK() : Status(code(), newReason, _error->extra);
}

Status Status::withContext(StringData reasonPrefix) const {
    return isOK() ? OK() : withReason(reasonPrefix + causedBy(reason()));
}

std::ostream& operator<<(std::ostream& os, const Status& status) {
    return os << status.codeString() << " " << status.reason();
}

template <typename Allocator>
StringBuilderImpl<Allocator>& operator<<(StringBuilderImpl<Allocator>& sb, const Status& status) {
    sb << status.codeString();
    if (!status.isOK()) {
        try {
            if (auto extra = status.extraInfo()) {
                BSONObjBuilder bob;
                extra->serialize(&bob);
                sb << bob.obj();
            }
        } catch (const DBException&) {
            // This really shouldn't happen but it would be really annoying if it broke error
            // logging in production.
            if (kDebugBuild) {
                severe() << "Error serializing extra info for " << status.code()
                         << " in Status::toString()";
                std::terminate();
            }
        }
        sb << ": " << status.reason();
    }
    return sb;
}
template StringBuilder& operator<<(StringBuilder& sb, const Status& status);

std::string Status::toString() const {
    StringBuilder sb;
    sb << *this;
    return sb.str();
}

}  // namespace mongo
