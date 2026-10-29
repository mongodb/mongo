// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <codecvt>  // IWYU pragma: keep
#include <locale>   // IWYU pragma: keep
#include <stdexcept>
#include <string>

#include <boost/filesystem/path.hpp>

namespace mongo {

MONGO_INITIALIZER_GENERAL(ValidateLocale, (), ("default"))
(InitializerContext*) {
    try {
        // Validate that boost can correctly load the user's locale
        boost::filesystem::path("/").has_root_directory();
    } catch (const std::runtime_error& e) {
        std::string extraHint;
#ifndef _WIN32
        extraHint = " Please ensure LANG and/or LC_* environment variables are set correctly. ";
#endif
        uasserted(ErrorCodes::BadValue,
                  str::stream() << "Invalid or no user locale set. " << extraHint << e.what());
    }

#ifdef _WIN32
    // Make boost filesystem treat all strings as UTF-8 encoded instead of CP_ACP.
    std::locale loc(std::locale(""), new std::codecvt_utf8_utf16<wchar_t>);
    boost::filesystem::path::imbue(loc);
#endif
}

}  // namespace mongo
