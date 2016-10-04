/*    Copyright 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

namespace mongo {

inline Status Status::OK() {
    return Status();
}

inline Status::Status(const Status& other) : _error(other._error) {
    ref(_error);
}

inline Status& Status::operator=(const Status& other) {
    ref(other._error);
    unref(_error);
    _error = other._error;
    return *this;
}

inline Status::Status(Status&& other) BOOST_NOEXCEPT : _error(other._error) {
    other._error = nullptr;
}

inline Status& Status::operator=(Status&& other) BOOST_NOEXCEPT {
    unref(_error);
    _error = other._error;
    other._error = nullptr;
    return *this;
}

inline Status::~Status() {
    unref(_error);
}

inline bool Status::isOK() const {
    return !_error;
}

inline ErrorCodes::Error Status::code() const {
    return _error ? _error->code : ErrorCodes::OK;
}

inline std::string Status::codeString() const {
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

inline Status::Status() : _error(NULL) {}

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

}  // namespace mongo
