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

#include "pch.h"
#include "ntservice.h"
#include "../db/client.h"
#include "../db/instance.h"
#include "winutil.h"
#include "text.h"

#if defined(_WIN32)

using std::wstring;

namespace mongo {

    SERVICE_STATUS_HANDLE ServiceController::_statusHandle = NULL;
    wstring ServiceController::_serviceName;
    ServiceCallback ServiceController::_serviceCallback = NULL;

    ServiceController::ServiceController() {}

    // defined in db/db.cpp for mongod.exe and in s/server.cpp for mongos.exe
    extern bool initService();

    // returns true if the service is started.
    bool serviceParamsCheck(
            boost::program_options::variables_map& params,
            const std::string dbpath,
            const ntServiceDefaultStrings& defaultStrings,
            const vector<string>& disallowedOptions,
            int argc,
            char* argv[]
    ) {
        bool installService = false;
        bool removeService = false;
        bool reinstallService = false;
        bool startService = false;

        int badOption = -1;
        for ( int i = 0, disallowedListLength = disallowedOptions.size(); i < disallowedListLength; ++i ) {
            if ( params.count( disallowedOptions[i] ) > 0 ) {
                badOption = i;
                break;
            }
        }

        wstring windowsServiceName( defaultStrings.serviceName );
        wstring windowsServiceDisplayName( defaultStrings.displayName );
        wstring windowsServiceDescription( defaultStrings.serviceDescription );
        wstring windowsServiceUser;
        wstring windowsServicePassword;

        if (params.count("install")) {
            if ( badOption != -1 ) {
                log() << "--install cannot be used with --" << disallowedOptions[badOption] << endl;
                ::exit( EXIT_BADOPTIONS );
            }
            if ( ! params.count( "logpath" ) ) {
                log() << "--install has to be used with --logpath" << endl;
                ::exit( EXIT_BADOPTIONS );
            }
            installService = true;
        }
        if (params.count("reinstall")) {
            if ( badOption != -1 ) {
                log() << "--reinstall cannot be used with --" << disallowedOptions[badOption] << endl;
                ::exit( EXIT_BADOPTIONS );
            }
            if ( ! params.count( "logpath" ) ) {
                log() << "--reinstall has to be used with --logpath" << endl;
                ::exit( EXIT_BADOPTIONS );
            }
            reinstallService = true;
        }
        if (params.count("remove")) {
            if ( badOption != -1 ) {
                log() << "--remove cannot be used with --" << disallowedOptions[badOption] << endl;
                ::exit( EXIT_BADOPTIONS );
            }
            removeService = true;
        }
        if (params.count("service")) {
            if ( badOption != -1 ) {
                log() << "--service cannot be used with --" << disallowedOptions[badOption] << endl;
                ::exit( EXIT_BADOPTIONS );
            }
            startService = true;
        }

        if (params.count("serviceName")) {
            if ( badOption != -1 ) {
                log() << "--serviceName cannot be used with --" << disallowedOptions[badOption] << endl;
                ::exit( EXIT_BADOPTIONS );
            }
            windowsServiceName = toWideString( params[ "serviceName" ].as<string>().c_str() );
        }
        if (params.count("serviceDisplayName")) {
            if ( badOption != -1 ) {
                log() << "--serviceDisplayName cannot be used with --" << disallowedOptions[badOption] << endl;
                ::exit( EXIT_BADOPTIONS );
            }
            windowsServiceDisplayName = toWideString( params[ "serviceDisplayName" ].as<string>().c_str() );
        }
        if (params.count("serviceDescription")) {
            if ( badOption != -1 ) {
                log() << "--serviceDescription cannot be used with --" << disallowedOptions[badOption] << endl;
                ::exit( EXIT_BADOPTIONS );
            }
            windowsServiceDescription = toWideString( params[ "serviceDescription" ].as<string>().c_str() );
        }
        if (params.count("serviceUser")) {
            if ( badOption != -1 ) {
                log() << "--serviceUser cannot be used with --" << disallowedOptions[badOption] << endl;
                ::exit( EXIT_BADOPTIONS );
            }
            windowsServiceUser = toWideString( params[ "serviceUser" ].as<string>().c_str() );
        }
        if (params.count("servicePassword")) {
            if ( badOption != -1 ) {
                log() << "--servicePassword cannot be used with --" << disallowedOptions[badOption] << endl;
                ::exit( EXIT_BADOPTIONS );
            }
            windowsServicePassword = toWideString( params[ "servicePassword" ].as<string>().c_str() );
        }

        if ( reinstallService ) {
            ServiceController::removeService( windowsServiceName );
        }
        if ( installService || reinstallService ) {
            if ( !ServiceController::installService(
                        windowsServiceName,
                        windowsServiceDisplayName,
                        windowsServiceDescription,
                        windowsServiceUser,
                        windowsServicePassword,
                        dbpath,
                        argc,
                        argv )
            ) {
                dbexit( EXIT_NTSERVICE_ERROR );
            }
            dbexit( EXIT_CLEAN );
        }
        else if ( removeService ) {
            if ( !ServiceController::removeService( windowsServiceName ) ) {
                dbexit( EXIT_NTSERVICE_ERROR );
            }
            dbexit( EXIT_CLEAN );
        }
        else if ( startService ) {
            if ( !ServiceController::startService( windowsServiceName , mongo::initService ) ) {
                dbexit( EXIT_NTSERVICE_ERROR );
            }
            return true;
        }
        return false;
    }

    bool ServiceController::installService(
            const wstring& serviceName,
            const wstring& displayName,
            const wstring& serviceDesc,
            const wstring& serviceUser,
            const wstring& servicePassword,
            const std::string dbpath,
            int argc,
            char* argv[]
    ) {
        log() << "Trying to install Windows service '" << toUtf8String(serviceName) << "'" << endl;

        stringstream commandLine;

        char exePath[1024];
        GetModuleFileNameA( NULL, exePath, sizeof exePath );
        commandLine << '"' << exePath << "\" ";

        for ( int i = 1; i < argc; i++ ) {
            std::string arg( argv[ i ] );
            // replace install command to indicate process is being started as a service
            if ( arg == "--install" || arg == "--reinstall" ) {
                arg = "--service";
            }
            else if ( arg == "--dbpath" && i + 1 < argc ) {
                commandLine << arg << " \"" << dbpath << "\" ";
                i++;
                continue;
            }
            else if ( arg == "--logpath" && i + 1 < argc ) {
                commandLine << arg << " \"" << argv[i+1] << "\" ";
                i++;
                continue;
            }
            else if ( arg == "-f" && i + 1 < argc ) {
                commandLine << arg << " \"" << argv[i+1] << "\" ";
                i++;
                continue;
            }
            else if ( arg == "--config" && i + 1 < argc ) {
                commandLine << arg << " \"" << argv[i+1] << "\" ";
                i++;
                continue;
            }
            else if ( arg == "--pidfilepath" && i + 1 < argc ) {
                commandLine << arg << " \"" << argv[i+1] << "\" ";
                i++;
                continue;
            }
            else if ( arg == "--repairpath" && i + 1 < argc ) {
                commandLine << arg << " \"" << argv[i+1] << "\" ";
                i++;
                continue;
            }
            else if ( arg == "--keyfile" && i + 1 < argc ) {
                commandLine << arg << " \"" << argv[i+1] << "\" ";
                i++;
                continue;
            }
            else if ( arg.length() > 9 && arg.substr(0, 9) == "--service" ) {
                // Strip off --service(Name|User|Password) arguments
                i++;
                continue;
            }
            commandLine << arg << " ";
        }

        SC_HANDLE schSCManager = ::OpenSCManager( NULL, NULL, SC_MANAGER_ALL_ACCESS );
        if ( schSCManager == NULL ) {
            DWORD err = ::GetLastError();
            log() << "Error connecting to the Service Control Manager: " << GetWinErrMsg(err) << endl;
            return false;
        }

        // Make sure service doesn't already exist.
        // TODO: Check to see if service is in "Deleting" status, suggest the user close down Services MMC snap-ins.
        SC_HANDLE schService = ::OpenService( schSCManager, serviceName.c_str(), SERVICE_ALL_ACCESS );
        if ( schService != NULL ) {
            log() << "There is already a service named '" << toUtf8String(serviceName) << "', aborting" << endl;
            ::CloseServiceHandle( schService );
            ::CloseServiceHandle( schSCManager );
            return false;
        }
        std::basic_ostringstream< TCHAR > commandLineWide;
        commandLineWide << commandLine.str().c_str();

        // create new service
        schService = ::CreateService(
                schSCManager,                   // Service Control Manager handle
                serviceName.c_str(),            // service name
                displayName.c_str(),            // service display name
                SERVICE_ALL_ACCESS,             // desired access
                SERVICE_WIN32_OWN_PROCESS,      // service type
                SERVICE_AUTO_START,             // start type
                SERVICE_ERROR_NORMAL,           // error control
                commandLineWide.str().c_str(),  // command line
                NULL,                           // load order group
                NULL,                           // tag id
                L"\0\0",                        // dependencies
                NULL,                           // user account
                NULL );                         // user account password
        if ( schService == NULL ) {
            DWORD err = ::GetLastError();
            log() << "Error creating service: " << GetWinErrMsg(err) << endl;
            ::CloseServiceHandle( schSCManager );
            return false;
        }

        log() << "Service '" << toUtf8String(serviceName) << "' (" << toUtf8String(displayName) <<
                ") installed with command line '" << commandLine.str() << "'" << endl;
        string typeableName( ( serviceName.find(L' ') != wstring::npos ) ?
                             "\"" + toUtf8String(serviceName) + "\""     :
                             toUtf8String(serviceName) );
        log() << "Service can be started from the command line with 'net start " << typeableName << "'" << endl;

        bool serviceInstalled;

        // TODO: If neccessary grant user "Login as a Service" permission.
        if ( !serviceUser.empty() ) {
            wstring actualServiceUser;
            if ( serviceUser.find(L"\\") == string::npos ) {
                actualServiceUser = L".\\" + serviceUser;
            }
            else {
                actualServiceUser = serviceUser;
            }

            log() << "Setting service login credentials for user: " << toUtf8String(actualServiceUser) << endl;
            serviceInstalled = ::ChangeServiceConfig(
                    schService,                 // service handle
                    SERVICE_NO_CHANGE,          // service type
                    SERVICE_NO_CHANGE,          // start type
                    SERVICE_NO_CHANGE,          // error control
                    NULL,                       // path
                    NULL,                       // load order group
                    NULL,                       // tag id
                    NULL,                       // dependencies
                    actualServiceUser.c_str(),  // user account
                    servicePassword.c_str(),    // user account password
                    NULL );                     // service display name
            if ( !serviceInstalled ) {
                log() << "Setting service login failed, service has 'LocalService' permissions" << endl;
            }
        }

        // set the service description
        SERVICE_DESCRIPTION serviceDescription;
        serviceDescription.lpDescription = (LPTSTR)serviceDesc.c_str();
        serviceInstalled = ::ChangeServiceConfig2( schService, SERVICE_CONFIG_DESCRIPTION, &serviceDescription );

#if 1
        if ( ! serviceInstalled ) {
#else
        // This code sets the mongod service to auto-restart, forever.
        // This might be a fine thing to do except that when mongod or Windows has a crash, the mongo.lock
        // file is still around, so any attempt at a restart will immediately fail.  With auto-restart, we
        // go into a loop, crashing and restarting, crashing and restarting, until someone comes in and
        // disables the service or deletes the mongod.lock file.
        //
        // I'm leaving the old code here for now in case we solve this and are able to turn SC_ACTION_RESTART
        // back on.
        //
        if ( serviceInstalled ) {
            SC_ACTION aActions[ 3 ] = { { SC_ACTION_RESTART, 0 }, { SC_ACTION_RESTART, 0 }, { SC_ACTION_RESTART, 0 } };

            SERVICE_FAILURE_ACTIONS serviceFailure;
            ZeroMemory( &serviceFailure, sizeof( SERVICE_FAILURE_ACTIONS ) );
            serviceFailure.cActions = 3;
            serviceFailure.lpsaActions = aActions;

            // set service recovery options
            serviceInstalled = ::ChangeServiceConfig2( schService, SERVICE_CONFIG_FAILURE_ACTIONS, &serviceFailure );

        }
        else {
#endif
            log() << "Could not set service description. Check the Windows Event Log for more details." << endl;
        }

        ::CloseServiceHandle( schService );
        ::CloseServiceHandle( schSCManager );

        return serviceInstalled;
    }

    bool ServiceController::removeService( const wstring& serviceName ) {
        log() << "Trying to remove Windows service '" << toUtf8String(serviceName) << "'" << endl;

        SC_HANDLE schSCManager = ::OpenSCManager( NULL, NULL, SC_MANAGER_ALL_ACCESS );
        if ( schSCManager == NULL ) {
            DWORD err = ::GetLastError();
            log() << "Error connecting to the Service Control Manager: " << GetWinErrMsg(err) << endl;
            return false;
        }

        SC_HANDLE schService = ::OpenService( schSCManager, serviceName.c_str(), SERVICE_ALL_ACCESS );
        if ( schService == NULL ) {
            log() << "Could not find a service named '" << toUtf8String(serviceName) << "' to remove" << endl;
            ::CloseServiceHandle( schSCManager );
            return false;
        }

        SERVICE_STATUS serviceStatus;

        // stop service if its running
        if ( ::ControlService( schService, SERVICE_CONTROL_STOP, &serviceStatus ) ) {
            log() << "Service " << toUtf8String(serviceName) << " is currently running, stopping service" << endl;
            while ( ::QueryServiceStatus( schService, &serviceStatus ) ) {
                if ( serviceStatus.dwCurrentState == SERVICE_STOP_PENDING ) {
                    Sleep( 1000 );
                }
                else { break; }
            }
            log() << "Service '" << toUtf8String(serviceName) << "' stopped" << endl;
        }

        bool serviceRemoved = ::DeleteService( schService );

        ::CloseServiceHandle( schService );
        ::CloseServiceHandle( schSCManager );

        if (serviceRemoved) {
            log() << "Service '" << toUtf8String(serviceName) << "' removed" << endl;
        }
        else {
            log() << "Failed to remove service '" << toUtf8String(serviceName) << "'" << endl;
        }

        return serviceRemoved;
    }

    bool ServiceController::startService( const wstring& serviceName, ServiceCallback startService ) {
        _serviceName = serviceName;
        _serviceCallback = startService;

        SERVICE_TABLE_ENTRY dispTable[] = {
            { (LPTSTR)serviceName.c_str(), (LPSERVICE_MAIN_FUNCTION)ServiceController::initService },
            { NULL, NULL }
        };

        log() << "Trying to start Windows service '" << toUtf8String(serviceName) << "'" << endl;
        return StartServiceCtrlDispatcher( dispTable );
    }

    bool ServiceController::reportStatus( DWORD reportState, DWORD waitHint ) {
        if ( _statusHandle == NULL )
            return false;

        static DWORD checkPoint = 1;

        SERVICE_STATUS ssStatus;

        DWORD dwControlsAccepted;
        switch ( reportState ) {
        case SERVICE_START_PENDING:
        case SERVICE_STOP_PENDING:
        case SERVICE_STOPPED:
            dwControlsAccepted = 0;
            break;
        default:
            dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
            break;
        }

        ssStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        ssStatus.dwServiceSpecificExitCode = 0;
        ssStatus.dwControlsAccepted = dwControlsAccepted;
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
        dbexit( EXIT_CLEAN );

        reportStatus( SERVICE_STOPPED );
    }

    static void serviceShutdown( const char* controlCodeName ) {
        Client::initThread( "serviceShutdown" );
        log() << "got " << controlCodeName << " request from Windows Service Control Manager, " <<
            ( inShutdown() ? "already in shutdown" : "will terminate after current cmd ends" ) << endl;
        ServiceController::reportStatus( SERVICE_STOP_PENDING );
        if ( ! inShutdown() ) {
            // TODO: SERVER-5703, separate the "cleanup for shutdown" functionality from
            // the "terminate process" functionality in exitCleanly.
            exitCleanly( EXIT_WINDOWS_SERVICE_STOP );
            ServiceController::reportStatus( SERVICE_STOPPED );
        }
    }

    void WINAPI ServiceController::serviceCtrl( DWORD ctrlCode ) {
        switch ( ctrlCode ) {
        case SERVICE_CONTROL_STOP:
            serviceShutdown( "SERVICE_CONTROL_STOP" );
            break;
        case SERVICE_CONTROL_SHUTDOWN:
            serviceShutdown( "SERVICE_CONTROL_SHUTDOWN" );
            break;
        }
    }

} // namespace mongo

#endif
