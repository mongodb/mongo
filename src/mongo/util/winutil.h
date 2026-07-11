// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

/**
 * Windows related utility functions
 */

#if defined(_WIN32)
#include <sstream>
#include <string>

#include "text.h"
#include <atlbase.h>
#include <atlstr.h>
#include <windows.h>

#include <boost/optional.hpp>
#include <mongo/base/status_with.h>

namespace mongo {
namespace windows {

inline std::string GetErrMsg(DWORD err) {
    LPTSTR errMsg;
    ::FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                    nullptr,
                    err,
                    0,
                    (LPTSTR)&errMsg,
                    0,
                    nullptr);
    std::string errMsgStr = toUtf8String(errMsg);
    ::LocalFree(errMsg);
    // FormatMessage() appends a newline to the end of error messages, we trim it because std::endl
    // flushes the buffer.
    errMsgStr = errMsgStr.erase(errMsgStr.length() - 2);
    std::ostringstream output;
    output << errMsgStr << " (" << err << ")";

    return output.str();
}

/**
 * Retrieve a DWORD value from the Local Machine Windows Registry for element:
 * group\key
 * e.g. HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters\KeepAliveTime
 *
 * On success, returns:
 *   boost::none if the key does not exist.
 *   The value read from the registry.
 *
 * On failure, returns:
 *   ErrorCodes::InternalError - Unable to access the registry group.
 *   ErrorCodes::TypeMismatch - Key exists, but is of the wrong type.
 */
StatusWith<boost::optional<DWORD>> getDWORDRegistryKey(const CString& group, const CString& key);

}  // namespace windows
}  // namespace mongo

#endif
