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

namespace mongo {

    const Status& Status::OK = *(new Status(ErrorCodes::OK, ""));

    Status::ErrorInfo::ErrorInfo(ErrorCodes::Error aCode, const std::string& aReason, int aLocation)
        : code(aCode), reason(aReason), location(aLocation) {}

    Status::Status(ErrorCodes::Error code, const char* reason, int location) {
        _error = new ErrorInfo(code, std::string(reason), location);
        ref(_error);
    }

    Status::Status(ErrorCodes::Error code, const std::string& reason, int location) {
        _error = new ErrorInfo(code, reason, location);
        ref(_error);
    }

    Status::Status(const Status& other) {
        ref(other._error);
        _error = other._error;
    }

    Status& Status::operator=(const Status& other) {
        ref(other._error);
        unref(_error);
        _error = other._error;
        return *this;
    }

    Status::~Status() {
        unref(_error);
    }

    bool Status::compare(const Status& other) const {
        return _error->code == other._error->code &&
               _error->location == other._error->location;
    }

    bool Status::operator==(const Status& other) const {
        return compare(other);
    }

    bool Status::operator!=(const Status& other) const {
        return ! compare(other);
    }

    bool Status::compareCode(const ErrorCodes::Error other) const {
        return _error->code == other;
    }

    bool Status::operator==(const ErrorCodes::Error other) const {
        return compareCode(other);
    }

    bool Status::operator!=(const ErrorCodes::Error other) const {
        return ! compareCode(other);
    }

    void Status::ref(ErrorInfo* error) {
        // OK is never deallocated, so no need to bump ref here.
        if (error->code == ErrorCodes::OK) {
            return;
        }
        error->refs.fetchAndAdd(1);
    }

    void Status::unref(ErrorInfo* error) {
        // OK is never deallocated.
        if (error->code == ErrorCodes::OK) {
            return;
        }

        if (error->refs.subtractAndFetch(1) == 0) {
            delete error;
        }
    }

    std::ostream& operator<<(std::ostream& os, const Status& status) {
        return os << status.codeString() << " " << status.reason();
    }

    std::ostream& operator<<(std::ostream& os, ErrorCodes::Error code) {
        return os << ErrorCodes::errorString(code);
    }

} // namespace mongo
