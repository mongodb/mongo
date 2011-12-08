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
#include "winutil.h"
#include "text.h"
#include <direct.h>

#if defined(_WIN32)

namespace mongo {

    void shutdownServer();

    SERVICE_STATUS_HANDLE ServiceController::_statusHandle = NULL;
    std::wstring ServiceController::_serviceName;
    ServiceCallback ServiceController::_serviceCallback = NULL;

    ServiceController::ServiceController() {}

    bool initService();

    // returns true if the service is started.
    bool serviceParamsCheck( boost::program_options::variables_map& params, const std::string dbpath, int argc, char* argv[] ) {
        bool installService = false;
        bool removeService = false;
        bool reinstallService = false;
        bool startService = false;

        std::wstring windowsServiceName = L"MongoDB";
        std::wstring windowsServiceDisplayName = L"Mongo DB";
        std::wstring windowsServiceDescription = L"Mongo DB Server";
        std::wstring windowsServiceUser = L"";
        std::wstring windowsServicePassword = L"";

        if (params.count("install")) {
            if ( ! params.count( "logpath" ) ) {
                cerr << "--install has to be used with --logpath" << endl;
                ::exit(-1);
            }
            installService = true;
        }
        if (params.count("reinstall")) {
            if ( ! params.count( "logpath" ) ) {
                cerr << "--reinstall has to be used with --logpath" << endl;
                ::exit(-1);
            }
            reinstallService = true;
        }
        if (params.count("remove")) {
            removeService = true;
        }
        if (params.count("service")) {
            startService = true;
        }

        if (params.count("serviceName")) {
            string x = params["serviceName"].as<string>();
            windowsServiceName = wstring(x.size(),L' ');
            for ( size_t i=0; i<x.size(); i++) {
                windowsServiceName[i] = x[i];
            }
        }
        if (params.count("serviceDisplayName")) {
            string x = params["serviceDisplayName"].as<string>();
            windowsServiceDisplayName = wstring(x.size(),L' ');
            for ( size_t i=0; i<x.size(); i++) {
                windowsServiceDisplayName[i] = x[i];
            }
        }
        if (params.count("serviceDescription")) {
            string x = params["serviceDescription"].as<string>();
            windowsServiceDescription = wstring(x.size(),L' ');
            for ( size_t i=0; i<x.size(); i++) {
                windowsServiceDescription[i] = x[i];
            }
        }
        if (params.count("serviceUser")) {
            string x = params["serviceUser"].as<string>();
            windowsServiceUser = wstring(x.size(),L' ');
            for ( size_t i=0; i<x.size(); i++) {
                windowsServiceUser[i] = x[i];
            }
        }
        if (params.count("servicePassword")) {
            string x = params["servicePassword"].as<string>();
            windowsServicePassword = wstring(x.size(),L' ');
            for ( size_t i=0; i<x.size(); i++) {
                windowsServicePassword[i] = x[i];
            }
        }

        if ( reinstallService ) {
            ServiceController::removeService( windowsServiceName );
        }
        if ( installService || reinstallService ) {
            if ( !ServiceController::installService( windowsServiceName , windowsServiceDisplayName, windowsServiceDescription, windowsServiceUser, windowsServicePassword, dbpath, argc, argv ) )
                dbexit( EXIT_NTSERVICE_ERROR );
            dbexit( EXIT_CLEAN );
        }
        else if ( removeService ) {
            if ( !ServiceController::removeService( windowsServiceName ) )
                dbexit( EXIT_NTSERVICE_ERROR );
            dbexit( EXIT_CLEAN );
        }
        else if ( startService ) {
            if ( !ServiceController::startService( windowsServiceName , mongo::initService ) )
                dbexit( EXIT_NTSERVICE_ERROR );
            return true;
        }
        return false;
    }

    bool ServiceController::installService( const std::wstring& serviceName, const std::wstring& displayName, const std::wstring& serviceDesc, const std::wstring& serviceUser, const std::wstring& servicePassword, const std::string dbpath, int argc, char* argv[] ) {
        assert(argc >= 1);

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
                commandLine << arg << "  \"" << dbpath << "\"  ";
                i++;
                continue;
            }
            else if ( arg == "--logpath" && i + 1 < argc ) {
                commandLine << arg << "  \"" << argv[i+1] << "\"  ";
                i++;
                continue;
            }
            else if ( arg == "-f" && i + 1 < argc ) {
                commandLine << arg << "  \"" << argv[i+1] << "\"  ";
                i++;
                continue;
            }
            else if ( arg == "--config" && i + 1 < argc ) {
                commandLine << arg << "  \"" << argv[i+1] << "\"  ";
                i++;
                continue;
            }
            else if ( arg == "--pidfilepath" && i + 1 < argc ) {
                commandLine << arg << "  \"" << argv[i+1] << "\"  ";
                i++;
                continue;
            }
            else if ( arg == "--repairpath" && i + 1 < argc ) {
                commandLine << arg << "  \"" << argv[i+1] << "\"  ";
                i++;
                continue;
            }
            else if ( arg == "--keyfile" && i + 1 < argc ) {
                commandLine << arg << "  \"" << argv[i+1] << "\"  ";
                i++;
                continue;
            }
            else if ( arg.length() > 9 && arg.substr(0, 9) == "--service" ) {
                // Strip off --service(Name|User|Password) arguments
                i++;
                continue;
            }
            commandLine << arg << "  ";
        }

        SC_HANDLE schSCManager = ::OpenSCManager( NULL, NULL, SC_MANAGER_ALL_ACCESS );
        if ( schSCManager == NULL ) {
            DWORD err = ::GetLastError();
            cerr << "Error connecting to the Service Control Manager: " << GetWinErrMsg(err) << endl;
            return false;
        }

        // Make sure servise doesn't already exist.
        // TODO: Check to see if service is in "Deleting" status, suggest the user close down Services MMC snap-ins.
        SC_HANDLE schService = ::OpenService( schSCManager, serviceName.c_str(), SERVICE_ALL_ACCESS );
        if ( schService != NULL ) {
            cerr << "There is already a service named " << toUtf8String(serviceName) << ". Aborting" << endl;
            ::CloseServiceHandle( schService );
            ::CloseServiceHandle( schSCManager );
            return false;
        }
        std::basic_ostringstream< TCHAR > commandLineWide;
        commandLineWide << commandLine.str().c_str();

        cerr << "Creating service " << toUtf8String(serviceName) << "." << endl;

        // create new service
        schService = ::CreateService( schSCManager, serviceName.c_str(), displayName.c_str(),
                                      SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                      SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                      commandLineWide.str().c_str(), NULL, NULL, L"\0\0", NULL, NULL );
        if ( schService == NULL ) {
            DWORD err = ::GetLastError();
            cerr << "Error creating service: " << GetWinErrMsg(err) << endl;
            ::CloseServiceHandle( schSCManager );
            return false;
        }

        cerr << "Service creation successful." << endl;
        cerr << "Service can be started from the command line via 'net start \"" << toUtf8String(serviceName) << "\"'." << endl;

        bool serviceInstalled;

        // TODO: If neccessary grant user "Login as a Service" permission.
        if ( !serviceUser.empty() ) {
            std::wstring actualServiceUser;
            if ( serviceUser.find(L"\\") == string::npos ) {
                actualServiceUser = L".\\" + serviceUser;
            }
            else {
                actualServiceUser = serviceUser;
            }

            cerr << "Setting service login credentials. User: " << toUtf8String(actualServiceUser) << endl;
            serviceInstalled = ::ChangeServiceConfig( schService, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE, NULL, NULL, NULL, NULL, actualServiceUser.c_str(), servicePassword.c_str(), NULL );
            if ( !serviceInstalled ) {
                cerr << "Setting service login failed. Service has 'LocalService' permissions." << endl;
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
            cerr << "Could not set service description. Check the event log for more details." << endl;
        }

        ::CloseServiceHandle( schService );
        ::CloseServiceHandle( schSCManager );

        return serviceInstalled;
    }

    bool ServiceController::removeService( const std::wstring& serviceName ) {
        SC_HANDLE schSCManager = ::OpenSCManager( NULL, NULL, SC_MANAGER_ALL_ACCESS );
        if ( schSCManager == NULL ) {
            DWORD err = ::GetLastError();
            cerr << "Error connecting to the Service Control Manager: " << GetWinErrMsg(err) << endl;
            return false;
        }

        SC_HANDLE schService = ::OpenService( schSCManager, serviceName.c_str(), SERVICE_ALL_ACCESS );
        if ( schService == NULL ) {
            cerr << "Could not find a service named " << toUtf8String(serviceName) << " to uninstall." << endl;
            ::CloseServiceHandle( schSCManager );
            return false;
        }

        SERVICE_STATUS serviceStatus;

        // stop service if its running
        if ( ::ControlService( schService, SERVICE_CONTROL_STOP, &serviceStatus ) ) {
            cerr << "Service " << toUtf8String(serviceName) << " is currently running. Stopping service." << endl;
            while ( ::QueryServiceStatus( schService, &serviceStatus ) ) {
                if ( serviceStatus.dwCurrentState == SERVICE_STOP_PENDING ) {
                    Sleep( 1000 );
                }
                else { break; }
            }
            cerr << "Service stopped." << endl;
        }

        cerr << "Deleting service " << toUtf8String(serviceName) << "." << endl;
        bool serviceRemoved = ::DeleteService( schService );

        ::CloseServiceHandle( schService );
        ::CloseServiceHandle( schSCManager );

        if (serviceRemoved) {
            cerr << "Service deleted successfully." << endl;
        }
        else {
            cerr << "Failed to delete service." << endl;
        }

        return serviceRemoved;
    }

    bool ServiceController::startService( const std::wstring& serviceName, ServiceCallback startService ) {
        _serviceName = serviceName;
        _serviceCallback = startService;

        SERVICE_TABLE_ENTRY dispTable[] = {
            { (LPTSTR)serviceName.c_str(), (LPSERVICE_MAIN_FUNCTION)ServiceController::initService },
            { NULL, NULL }
        };

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
        log() << "got " << controlCodeName << " request from Windows Service Controller, " <<
            ( inShutdown() ? "already in shutdown" : "will terminate after current cmd ends" ) << endl;
        ServiceController::reportStatus( SERVICE_STOP_PENDING );
        if ( ! inShutdown() ) {
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
