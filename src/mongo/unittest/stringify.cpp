// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/unittest/stringify.h"

#include "mongo/util/demangle.h"
#include "mongo/util/hex.h"

#include <string>
#include <string_view>
#include <typeinfo>

#include <fmt/format.h>

namespace mongo::unittest::stringify {

std::string formatTypedObj(const std::type_info& ti, std::string_view s) {
    return fmt::format("[{}={}]", demangleName(ti), s);
}

std::string lastResortFormat(const std::type_info& ti, const void* p, size_t sz) {
    return formatTypedObj(ti, hexdump(p, sz));
}

}  // namespace mongo::unittest::stringify
