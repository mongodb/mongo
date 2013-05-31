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

#pragma once

namespace mongo {

    inline Status Status::OK() {
        return Status();
    }

    inline Status::Status(const Status& other)
        : _error(other._error) {
        ref(_error);
    }

    inline Status& Status::operator=(const Status& other) {
        ref(other._error);
        unref(_error);
        _error = other._error;
        return *this;
    }

    inline Status::~Status() {
        unref(_error);
    }

    inline bool Status::isOK() const {
        return code() == ErrorCodes::OK;
    }

    inline ErrorCodes::Error Status::code() const {
        return _error ? _error->code : ErrorCodes::OK;
    }

    inline const char* Status::codeString() const {
        return ErrorCodes::errorString(code());
    }

    inline std::string Status::reason() const {
        return _error ? _error->reason : std::string();
    }

    inline int Status::location() const {
        return _error ? _error->location : 0;
    }

    inline AtomicUInt32::WordType Status::refCount() const {
        return _error ? _error->refs.load() : 0;
    }

    inline Status::Status()
        : _error(NULL) {
    }

    inline void Status::ref(ErrorInfo* error) {
        if (error)
            error->refs.fetchAndAdd(1);
    }

    inline void Status::unref(ErrorInfo* error) {
        if (error && (error->refs.subtractAndFetch(1) == 0))
            delete error;
    }

    inline bool operator==(const ErrorCodes::Error lhs, const Status& rhs) {
        return rhs == lhs;
    }

    inline bool operator!=(const ErrorCodes::Error lhs, const Status& rhs) {
        return rhs != lhs;
    }

} // namespace mongo
