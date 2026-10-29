// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#if defined(_WIN32)


#include "mongo/util/winutil.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"

namespace mongo {

StatusWith<boost::optional<DWORD>> windows::getDWORDRegistryKey(const CString& group,
                                                                const CString& key) {
    CRegKey regkey;
    if (ERROR_SUCCESS != regkey.Open(HKEY_LOCAL_MACHINE, group, KEY_READ)) {
        return Status(ErrorCodes::InternalError, "Unable to access windows registry");
    }

    DWORD val;
    const auto res = regkey.QueryDWORDValue(key, val);
    if (ERROR_INVALID_DATA == res) {
        return Status(ErrorCodes::TypeMismatch,
                      "Invalid data type in windows registry, expected DWORD");
    }

    if (ERROR_SUCCESS != res) {
        return boost::none;
    }

    return val;
}

}  // namespace mongo

#endif
