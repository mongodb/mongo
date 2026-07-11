// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#if !defined(_WIN32)
#include <clocale>  // For setlocale()
#endif

#include "mongo/unittest/unittest.h"
#include "mongo/util/errno_util.h"

#include <array>
#include <climits>

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
