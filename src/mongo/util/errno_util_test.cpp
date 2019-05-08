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

namespace {
using namespace mongo;

const std::string kUnknownError = "Unknown error";

TEST(ErrnoWithDescription, CommonErrors) {
#if defined(_WIN32)
    // Force error messages to be returned in en-US.
    LANGID lang = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
    ASSERT_EQ(SetThreadUILanguage(lang), lang);

    ASSERT_STRING_OMITS(errnoWithDescription(ERROR_SUCCESS), kUnknownError);
    ASSERT_STRING_OMITS(errnoWithDescription(ERROR_FILE_NOT_FOUND), kUnknownError);
    ASSERT_STRING_OMITS(errnoWithDescription(ERROR_PATH_NOT_FOUND), kUnknownError);
    ASSERT_STRING_OMITS(errnoWithDescription(ERROR_TOO_MANY_OPEN_FILES), kUnknownError);
    ASSERT_STRING_OMITS(errnoWithDescription(ERROR_ACCESS_DENIED), kUnknownError);
    ASSERT_STRING_OMITS(errnoWithDescription(ERROR_PRIVILEGE_NOT_HELD), kUnknownError);
#else
    // Force the minimal locale to ensure the standard error message localization text.
    ASSERT(setlocale(LC_MESSAGES, "C"));

    ASSERT_STRING_OMITS(errnoWithDescription(EPERM), kUnknownError);
    ASSERT_STRING_OMITS(errnoWithDescription(ENOENT), kUnknownError);
    ASSERT_STRING_OMITS(errnoWithDescription(EIO), kUnknownError);
    ASSERT_STRING_OMITS(errnoWithDescription(EBADF), kUnknownError);
    ASSERT_STRING_OMITS(errnoWithDescription(ENOMEM), kUnknownError);
    ASSERT_STRING_OMITS(errnoWithDescription(EACCES), kUnknownError);
#endif

    // INT_MAX is currently invalid.  In the unlikely event that it becomes valid, then this check
    // will have to be removed or adjusted.
    ASSERT_STRING_CONTAINS(errnoWithDescription(INT_MAX), kUnknownError);
}
}  // namespace
