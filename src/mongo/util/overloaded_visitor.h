// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string_view>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * This is the "overload pattern" for use with variant visit calls.
 * See https://www.modernescpp.com/index.php/visiting-a-std-variant-with-the-overload-pattern
 * Example usage:
 *
 * auto r = visit(OverloadedVisitor{
 *                         [](int v) { return intStuff(v); },
 *                         [](std::string_view v) { return stringStuff(v); },
 *                      },
 *                      someStdxVariant);
 */
template <typename... Ts>
struct OverloadedVisitor : Ts... {
    using Ts::operator()...;
};
template <typename... Ts>
OverloadedVisitor(Ts...) -> OverloadedVisitor<Ts...>;

}  // namespace mongo
