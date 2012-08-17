// ntservice.h

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#if defined(_WIN32)
#include <windows.h>
#include "boost/program_options.hpp"

namespace mongo {

    struct ntServiceDefaultStrings {
        const wchar_t* serviceName;
        const wchar_t* displayName;
        const wchar_t* serviceDescription;
    };

    typedef bool ( *ServiceCallback )( void );
    bool serviceParamsCheck(
            boost::program_options::variables_map& params,
            const std::string dbpath,
            const ntServiceDefaultStrings& defaultStrings,
            const vector<string>& disallowedOptions,
            int argc,
            char* argv[]
    );

    class ServiceController {
    public:
        ServiceController();
        virtual ~ServiceController() {}

        static bool installService(
                const std::wstring& serviceName,
                const std::wstring& displayName,
                const std::wstring& serviceDesc,
                const std::wstring& serviceUser,
                const std::wstring& servicePassword,
                const std::string dbpath,
                int argc,
                char* argv[]
        );
        static bool removeService( const std::wstring& serviceName );
        static bool startService( const std::wstring& serviceName, ServiceCallback startService );
        static bool reportStatus( DWORD reportState, DWORD waitHint = 0 );

        static void WINAPI initService( DWORD argc, LPTSTR *argv );
        static void WINAPI serviceCtrl( DWORD ctrlCode );

    protected:
        static std::wstring _serviceName;
        static SERVICE_STATUS_HANDLE _statusHandle;
        static ServiceCallback _serviceCallback;
    };

} // namespace mongo

#endif
