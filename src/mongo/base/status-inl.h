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

} // namespace mongo
