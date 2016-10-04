/**
*    Copyright (C) 2012 10gen Inc.
*
*    This program is free software: you can redistribute it and/or modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/platform/basic.h"

#ifdef _WIN32
#include <crtdbg.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#endif

#include "mongo/base/init.h"
#include "mongo/util/log.h"
#include "mongo/util/stacktrace.h"

#ifdef _WIN32

namespace mongo {

MONGO_INITIALIZER(Behaviors_Win32)(InitializerContext*) {
    // do not display dialog on abort()
    _set_abort_behavior(0, _CALL_REPORTFAULT | _WRITE_ABORT_MSG);

    // hook the C runtime's error display
    _CrtSetReportHook(crtDebugCallback);

    if (_setmaxstdio(2048) == -1) {
        warning() << "Failed to increase max open files limit from default of 512 to 2048";
    }

    // Let's set minimum Windows Kernel quantum length to 1ms in order to allow sleepmillis()
    // to support waiting periods below Windows default quantum length (which can vary per
    // Windows version)
    // see http://msdn.microsoft.com/en-us/library/windows/desktop/dd757624(v=vs.85).aspx
    if (timeBeginPeriod(1) != TIMERR_NOERROR) {
        warning() << "Failed to set minimum timer resolution to 1 millisecond";
    }

    return Status::OK();
}

}  // namespace mongo

#endif  // _WIN32
