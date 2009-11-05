// ntservice.cpp

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

#include "stdafx.h"
#include "ntservice.h"

#if defined(_WIN32)

namespace mongo {

	void shutdown();

	SERVICE_STATUS_HANDLE ServiceController::_statusHandle = null;
	std::wstring ServiceController::_serviceName;
	ServiceCallback ServiceController::_serviceCallback = null;

	ServiceController::ServiceController() {
    }
    
    bool ServiceController::installService( const std::wstring& serviceName, const std::wstring& displayName, const std::wstring& serviceDesc, int argc, char* argv[] ) {
        
        std::string commandLine;
        
        for ( int i = 0; i < argc; i++ ) {
			std::string arg( argv[ i ] );
			
			// replace install command to indicate process is being started as a service
			if ( arg == "--install" )
				arg = "--service";
				
			commandLine += arg + " ";
		}
		
		SC_HANDLE schSCManager = ::OpenSCManager( null, null, SC_MANAGER_ALL_ACCESS );
		if ( schSCManager == null )
			return false;
		
		std::basic_ostringstream< TCHAR > commandLineWide;
        commandLineWide << commandLine.c_str();

		// create new service
		SC_HANDLE schService = ::CreateService( schSCManager, serviceName.c_str(), displayName.c_str(),
												SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
												SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
												commandLineWide.str().c_str(), null, null, L"\0\0", null, null );

		if ( schService == null ) {
			::CloseServiceHandle( schSCManager );
			return false;
		}

		SERVICE_DESCRIPTION serviceDescription;
		serviceDescription.lpDescription = (LPTSTR)serviceDesc.c_str();
		
		// set new service description
		bool serviceInstalled = ::ChangeServiceConfig2( schService, SERVICE_CONFIG_DESCRIPTION, &serviceDescription );
		
		if ( serviceInstalled ) {
			SC_ACTION aActions[ 3 ] = { { SC_ACTION_RESTART, 0 }, { SC_ACTION_RESTART, 0 }, { SC_ACTION_RESTART, 0 } };
			
			SERVICE_FAILURE_ACTIONS serviceFailure;
			ZeroMemory( &serviceFailure, sizeof( SERVICE_FAILURE_ACTIONS ) );
			serviceFailure.cActions = 3;
			serviceFailure.lpsaActions = aActions;
			
			// set service recovery options
			serviceInstalled = ::ChangeServiceConfig2( schService, SERVICE_CONFIG_FAILURE_ACTIONS, &serviceFailure );
		}
		
		::CloseServiceHandle( schService );
		::CloseServiceHandle( schSCManager );
		
		return serviceInstalled;
    }
    
    bool ServiceController::removeService( const std::wstring& serviceName ) {
		SC_HANDLE schSCManager = ::OpenSCManager( null, null, SC_MANAGER_ALL_ACCESS );
		if ( schSCManager == null )
			return false;

		SC_HANDLE schService = ::OpenService( schSCManager, serviceName.c_str(), SERVICE_ALL_ACCESS );

		if ( schService == null ) {
			::CloseServiceHandle( schSCManager );
			return false;
		}

		SERVICE_STATUS serviceStatus;
		
		// stop service if running
		if ( ::ControlService( schService, SERVICE_CONTROL_STOP, &serviceStatus ) ) {
			while ( ::QueryServiceStatus( schService, &serviceStatus ) ) {
				if ( serviceStatus.dwCurrentState == SERVICE_STOP_PENDING )
					Sleep( 1000 );
			}
		}

		bool serviceRemoved = ::DeleteService( schService );
		
		::CloseServiceHandle( schService );
		::CloseServiceHandle( schSCManager );

		return serviceRemoved;
    }
    
    bool ServiceController::startService( const std::wstring& serviceName, ServiceCallback startService ) {
        _serviceName = serviceName;
		_serviceCallback = startService;
	
        SERVICE_TABLE_ENTRY dispTable[] = {
			{ (LPTSTR)serviceName.c_str(), (LPSERVICE_MAIN_FUNCTION)ServiceController::initService },
			{ null, null }
		};

		return StartServiceCtrlDispatcher( dispTable );
    }
    
    bool ServiceController::reportStatus( DWORD reportState, DWORD waitHint ) {
		if ( _statusHandle == null )
			return false;

		static DWORD checkPoint = 1;
		
		SERVICE_STATUS ssStatus;

		ssStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
		ssStatus.dwServiceSpecificExitCode = 0;
		ssStatus.dwControlsAccepted = reportState == SERVICE_START_PENDING ? 0 : SERVICE_ACCEPT_STOP;
		ssStatus.dwCurrentState = reportState;
		ssStatus.dwWin32ExitCode = NO_ERROR;
		ssStatus.dwWaitHint = waitHint;
		ssStatus.dwCheckPoint = ( reportState == SERVICE_RUNNING || reportState == SERVICE_STOPPED ) ? 0 : checkPoint++;

		return SetServiceStatus( _statusHandle, &ssStatus );
	}
    
    void WINAPI ServiceController::initService( DWORD argc, LPTSTR *argv ) {
		_statusHandle = RegisterServiceCtrlHandler( _serviceName.c_str(), serviceCtrl );
		if ( !_statusHandle )
			return;

		reportStatus( SERVICE_START_PENDING, 1000 );
		
		_serviceCallback();
		
		reportStatus( SERVICE_STOPPED );
	}
	
	void WINAPI ServiceController::serviceCtrl( DWORD ctrlCode ) {
		switch ( ctrlCode ) {
			case SERVICE_CONTROL_STOP:
			case SERVICE_CONTROL_SHUTDOWN:
				shutdown();
				reportStatus( SERVICE_STOPPED );
				return;
		}
	}

} // namespace mongo

#endif
