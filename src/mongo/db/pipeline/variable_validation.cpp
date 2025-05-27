/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <functional>

namespace mongo::variableValidation {

namespace {
Status isValidName(StringData varName,
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

void validateName(StringData varName,
                  std::function<bool(char)> prefixPred,
                  std::function<bool(char)> suffixPred,
                  int prefixLen) {
    uassertStatusOK(isValidName(varName, prefixPred, suffixPred, prefixLen));
}

Status isValidNameForUserWrite(StringData varName) {
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

void validateNameForUserWrite(StringData varName) {
    uassertStatusOK(isValidNameForUserWrite(varName));
}

void validateNameForUserRead(StringData varName) {
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
