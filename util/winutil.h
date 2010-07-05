// @file winutil.cpp : Windows related utility functions
//
// /**
// *    Copyright (C) 2008 10gen Inc.
// *
// *    This program is free software: you can redistribute it and/or  modify
// *    it under the terms of the GNU Affero General Public License, version 3,
// *    as published by the Free Software Foundation.
// *
// *    This program is distributed in the hope that it will be useful,
// *    but WITHOUT ANY WARRANTY; without even the implied warranty of
// *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// *    GNU Affero General Public License for more details.
// *
// *    You should have received a copy of the GNU Affero General Public License
// *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
// */
//
// #include "pch.h"

#pragma once

#if defined(_WIN32)
#include <windows.h>
#include "text.h"

namespace mongo {

    inline string GetWinErrMsg(DWORD err) {
        LPTSTR errMsg;
        ::FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM, NULL, err, 0, (LPTSTR)&errMsg, 0, NULL );
        std::string errMsgStr = toUtf8String( errMsg );
        ::LocalFree( errMsg );
        // FormatMessage() appends a newline to the end of error messages, we trim it because endl flushes the buffer.
        errMsgStr = errMsgStr.erase( errMsgStr.length() - 2 );
        std::ostringstream output;
        output << errMsgStr << " (" << err << ")";

        return output.str();
    }
}

#endif

