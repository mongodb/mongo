// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"

#include <functional>  // std::function
#include <string_view>

namespace mongo::variableValidation {
Status isValidNameForUserWrite(std::string_view varName);
void validateNameForUserWrite(std::string_view varName);
void validateNameForUserRead(std::string_view varName);
void validateName(std::string_view varName,
                  std::function<bool(char)> prefixPred,
                  std::function<bool(char)> suffixPred,
                  int prefixLen);
}  // namespace mongo::variableValidation
