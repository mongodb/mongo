/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/base/status.h"

#include <ostream>
#include <sstream>

namespace mongo {

    Status::ErrorInfo::ErrorInfo(ErrorCodes::Error aCode, const StringData& aReason, int aLocation)
        : code(aCode), reason(aReason.toString()), location(aLocation) {
    }

    Status::ErrorInfo* Status::ErrorInfo::create(ErrorCodes::Error c, const StringData& r, int l) {
        const bool needRep = ((c != ErrorCodes::OK) ||
                              !r.empty() ||
                              (l != 0));
        return needRep ? new ErrorInfo(c, r, l) : NULL;
    }

    Status::Status(ErrorCodes::Error code, const std::string& reason, int location)
        : _error(ErrorInfo::create(code, reason, location)) {
        ref(_error);
    }

    Status::Status(ErrorCodes::Error code, const char* reason, int location)
        : _error(ErrorInfo::create(code, reason, location)) {
        ref(_error);
    }

    bool Status::compare(const Status& other) const {
        return
            code() == other.code() &&
            location() == other.location();
    }

    bool Status::operator==(const Status& other) const {
        return compare(other);
    }

    bool Status::operator!=(const Status& other) const {
        return ! compare(other);
    }

    bool Status::compareCode(const ErrorCodes::Error other) const {
        return code() == other;
    }

    bool Status::operator==(const ErrorCodes::Error other) const {
        return compareCode(other);
    }

    bool Status::operator!=(const ErrorCodes::Error other) const {
        return ! compareCode(other);
    }

    std::ostream& operator<<(std::ostream& os, const Status& status) {
        return os << status.codeString() << " " << status.reason();
    }

    std::ostream& operator<<(std::ostream& os, ErrorCodes::Error code) {
        return os << ErrorCodes::errorString(code);
    }

    std::string Status::toString() const {
        std::ostringstream ss;
        ss << codeString();
        if ( !isOK() )
            ss << " " << reason();
        if ( location() != 0 )
            ss << " @ " << location();
        return ss.str();
    }

} // namespace mongo
