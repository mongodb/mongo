/**
 * Copyright 2012 10gen Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mongo/platform/basic.h"

#include "mongo/base/init.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/socket_poll.h"

namespace mongo {

#ifdef _WIN32

    typedef int (WSAAPI *WSAPollFunction)(pollfd* fdarray, ULONG nfds, INT timeout);

    static WSAPollFunction wsaPollFunction = NULL;

    MONGO_INITIALIZER(DynamicLinkWin32Poll)(InitializerContext* context) {
        HINSTANCE wsaPollLib = LoadLibraryW( L"Ws2_32.dll" );
        if (wsaPollLib) {
            wsaPollFunction =
                reinterpret_cast<WSAPollFunction>(GetProcAddress(wsaPollLib, "WSAPoll"));
        }

        return Status::OK();
    }

    bool isPollSupported() { return wsaPollFunction != NULL; }

    int socketPoll( pollfd* fdarray, unsigned long nfds, int timeout ) {
        fassert(17185, isPollSupported());
        return wsaPollFunction(fdarray, nfds, timeout);
    }

#else

    bool isPollSupported() { return true; }

    int socketPoll( pollfd* fdarray, unsigned long nfds, int timeout ) {
        return ::poll(fdarray, nfds, timeout);
    }

#endif

}




