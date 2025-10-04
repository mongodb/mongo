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
