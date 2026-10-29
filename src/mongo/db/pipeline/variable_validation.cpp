// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <functional>
#include <string_view>

namespace mongo::variableValidation {

namespace {
Status isValidName(std::string_view varName,
                   std::function<bool(char)> prefixPred,
                   std::function<bool(char)> suffixPred,
                   int prefixLen) {
    if (varName.empty()) {
        return Status{ErrorCodes::FailedToParse, "empty variable names are not allowed"};
    }
    for (int i = 0; i < prefixLen; ++i)
        if (!prefixPred(varName[i])) {
            return Status{ErrorCodes::FailedToParse,
                          str::stream()
                              << "'" << varName
                              << "' starts with an invalid character for a user variable name"};
        }

    for (size_t i = prefixLen; i < varName.size(); i++)
        if (!suffixPred(varName[i])) {
            return Status{ErrorCodes::FailedToParse,
                          str::stream() << "'" << varName << "' contains an invalid character "
                                        << "for a variable name: '" << varName[i] << "'"};
        }
    return Status::OK();
}
}  // namespace

void validateName(std::string_view varName,
                  std::function<bool(char)> prefixPred,
                  std::function<bool(char)> suffixPred,
                  int prefixLen) {
    uassertStatusOK(isValidName(varName, prefixPred, suffixPred, prefixLen));
}

Status isValidNameForUserWrite(std::string_view varName) {
    // System variables users allowed to write to (currently just one)
    if (varName == "CURRENT") {
        return Status::OK();
    }
    return isValidName(
        varName,
        [](char ch) -> bool {
            return (ch >= 'a' && ch <= 'z') || (ch & '\x80');  // non-ascii
        },
        [](char ch) -> bool {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || (ch == '_') || (ch & '\x80');  // non-ascii
        },
        1);
}

void validateNameForUserWrite(std::string_view varName) {
    uassertStatusOK(isValidNameForUserWrite(varName));
}

void validateNameForUserRead(std::string_view varName) {
    validateName(
        varName,
        [](char ch) -> bool {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch & '\x80');  // non-ascii
        },
        [](char ch) -> bool {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || (ch == '_') || (ch & '\x80');  // non-ascii
        },
        1);
}

}  // namespace mongo::variableValidation
