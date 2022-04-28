/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#if !defined(_WIN32)
#include <errno.h>   // For the E* error codes
#include <locale.h>  // For setlocale()
#endif

#include "mongo/unittest/unittest.h"
#include "mongo/util/errno_util.h"

namespace mongo {
namespace {

const std::string kUnknownError = "Unknown error";

/** Force a predictable error message language. */
void initLanguage() {
#if defined(_WIN32)
    LANGID lang = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
    ASSERT_EQ(SetThreadUILanguage(lang), lang);
#else
    ASSERT(setlocale(LC_MESSAGES, "C"));
#endif
}

TEST(ErrnoWithDescription, CommonErrors) {
#if defined(_WIN32)
    static const std::array knownErrors{
        ERROR_SUCCESS,
        ERROR_FILE_NOT_FOUND,
        ERROR_PATH_NOT_FOUND,
        ERROR_TOO_MANY_OPEN_FILES,
        ERROR_ACCESS_DENIED,
        ERROR_PRIVILEGE_NOT_HELD,
    };
#else
    static const std::array knownErrors{
        EPERM,
        ENOENT,
        EIO,
        EBADF,
        ENOMEM,
        EACCES,
    };
#endif

    initLanguage();

    for (auto e : knownErrors)
        ASSERT_STRING_OMITS(errorMessage(systemError(e)), kUnknownError);

    // Update if INT_MAX becomes a valid code.
    ASSERT_STRING_CONTAINS(errorMessage(systemError(INT_MAX)), kUnknownError);
}
}  // namespace
}  // namespace mongo
