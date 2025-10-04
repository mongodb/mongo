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
