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


#include "mongo/base/error_codes.h"

#include "mongo/base/static_assert.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

//#set $codes_with_extra = [ec for ec in $codes if ec.extra]
//#set $codes_with_non_optional_extra = [ec for ec in $codes if ec.extra and not ec.extraIsOptional]

namespace mongo {

namespace {
// You can think of this namespace as a compile-time map<ErrorCodes::Error, ErrorExtraInfoParser*>.
namespace parsers {
//#for $ec in $codes_with_extra
ErrorExtraInfo::Parser* $ec.name = nullptr;
//#end for
}  // namespace parsers
}  // namespace


MONGO_STATIC_ASSERT(sizeof(ErrorCodes::Error) == sizeof(int));

std::string ErrorCodes::errorString(Error err) {
    switch (err) {
        //#for $ec in $codes
        case $ec.name:
            return "$ec.name";
        //#end for
        default:
            return str::stream() << "Location" << int(err);
    }
}

ErrorCodes::Error ErrorCodes::fromString(StringData name) {
    //#for $ec in $codes
    if (name == "$ec.name"_sd)
        return $ec.name;
    //#end for
    return UnknownError;
}

std::ostream& operator<<(std::ostream& stream, ErrorCodes::Error code) {
    return stream << ErrorCodes::errorString(code);
}

//#for $cat in $categories
template <>
bool ErrorCodes::isA<ErrorCategory::$cat.name>(Error err) {
    switch (err) {
        //#for $code in $cat.codes
        case $code:
            return true;
        //#end for
        default:
            return false;
    }
}

//#end for
bool ErrorCodes::canHaveExtraInfo(Error code) {
    switch (code) {
        //#for $ec in $codes_with_extra
        case ErrorCodes::$ec.name:
            return true;
        //#end for
        default:
            return false;
    }
}

bool ErrorCodes::mustHaveExtraInfo(Error code) {
    switch (code) {
        //#for $ec in $codes_with_non_optional_extra
        case ErrorCodes::$ec.name:
            return true;
        //#end for
        default:
            return false;
    }
}

ErrorExtraInfo::Parser* ErrorExtraInfo::parserFor(ErrorCodes::Error code) {
    switch (code) {
        //#for $ec in $codes_with_extra
        case ErrorCodes::$ec.name:
            invariant(parsers::$ec.name);
            return parsers::$ec.name;
        //#end for
        default:
            return nullptr;
    }
}

void ErrorExtraInfo::registerParser(ErrorCodes::Error code, Parser* parser) {
    switch (code) {
        //#for $ec in $codes_with_extra
        case ErrorCodes::$ec.name:
            invariant(!parsers::$ec.name);
            parsers::$ec.name = parser;
            break;
        //#end for
        default:
            MONGO_UNREACHABLE;
    }
}

void ErrorExtraInfo::invariantHaveAllParsers() {
    //#for $ec in $codes_with_extra
    invariant(parsers::$ec.name);
    //#end for
}

void error_details::throwExceptionForStatus(const Status& status) {
    /**
     * This type is used for all exceptions that don't have a more specific type. It is defined
     * locally in this function to prevent anyone from catching it specifically separately from
     * AssertionException.
     */
    class NonspecificAssertionException final : public AssertionException {
    public:
        using AssertionException::AssertionException;

    private:
        void defineOnlyInFinalSubclassToPreventSlicing() final {}
    };

    switch (status.code()) {
        //#for $ec in $codes
        case ErrorCodes::$ec.name:
            throw ExceptionFor<ErrorCodes::$ec.name>(status);
        //#end for
        default:
            throw NonspecificAssertionException(status);
    }
}

}  // namespace mongo
