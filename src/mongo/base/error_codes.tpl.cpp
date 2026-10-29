// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"

#include "mongo/base/static_assert.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include <string_view>

//#set $codes_with_extra = [ec for ec in $codes if ec.extra]
//#set $codes_with_non_optional_extra = [ec for ec in $codes if ec.extra and not ec.extraIsOptional]

namespace mongo {
using namespace std::literals::string_view_literals;

namespace {
// You can think of this namespace as a compile-time map<ErrorCodes::Error, ErrorExtraInfoParser*>.
namespace parsers {
//#for $ec in $codes_with_extra
ErrorExtraInfo::Parser* $ec.name = nullptr;
//#end for
}  // namespace parsers
}  // namespace


MONGO_STATIC_ASSERT(sizeof(ErrorCodes::Error) == sizeof(int));

std::string toString(ErrorCategory cat) {
    switch (cat) {
        //#for $cat in $categories
        case ErrorCategory::$cat.name:
            return "$cat.name";
        //#end for
        default:
            return fmt::format("Location{}", int(cat));
    }
}

std::ostream& operator<<(std::ostream& stream, ErrorCategory cat) {
    return stream << toString(cat);
}

std::string ErrorCodes::errorString(Error err) {
    switch (err) {
        //#for $ec in $codes
        case $ec.name:
            return "$ec.name";
        //#end for
        default:
            return fmt::format("Location{}", int(err));
    }
}

ErrorCodes::Error ErrorCodes::fromString(std::string_view name) {
    //#for $ec in $codes
    if (name == "$ec.name"sv)
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

std::span<const ErrorCodes::Error> allErrorCodes_forTest() {
    static const constexpr std::array<ErrorCodes::Error, ${len($codes)}> arr{
        //#for $ec in $codes:
        ErrorCodes::Error{$ec.code},  // $ec.name
        //#end for
    };
    return arr;
}

}  // namespace mongo
