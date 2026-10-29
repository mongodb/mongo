// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

class [[MONGO_MOD_PUBLIC]] IndexConstants {
public:
    static constexpr std::string_view kIdIndexName = "_id_"sv;
};

}  // namespace mongo
