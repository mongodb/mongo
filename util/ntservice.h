// ntservice.h

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
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
*/

#pragma once

#if defined(_WIN32)
#include <windows.h>

namespace mongo {

	typedef bool ( *ServiceCallback )( void );

    class ServiceController {
    public:
        ServiceController();
        virtual ~ServiceController() {}
        
        static bool installService( const std::wstring& serviceName, const std::wstring& displayName, const std::wstring& serviceDesc, int argc, char* argv[] );
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
