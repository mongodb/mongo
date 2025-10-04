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


#if defined(_WIN32)


#include "mongo/util/ntservice.h"

#include "mongo/logv2/log.h"
#include "mongo/stdx/chrono.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/signal_handlers.h"
#include "mongo/util/text.h"  // IWYU pragma: keep
#include "mongo/util/winutil.h"

#include <boost/range/size.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

using std::string;
using std::wstring;

namespace mongo {

namespace ntservice {
namespace {
bool _startService = false;
SERVICE_STATUS_HANDLE _statusHandle = nullptr;
wstring _serviceName;
ServiceCallback _serviceCallback = nullptr;
}  // namespace

static void installServiceOrDie(const wstring& serviceName,
                                const wstring& displayName,
                                const wstring& serviceDesc,
                                const wstring& serviceUser,
                                const wstring& servicePassword,
                                const std::vector<std::string>& argv,
                                const bool reinstall);

static void removeServiceOrDie(const wstring& serviceName);

bool shouldStartService() {
    return _startService;
}

static DWORD WINAPI serviceCtrl(DWORD dwControl,
                                DWORD dwEventType,
                                LPVOID lpEventData,
                                LPVOID lpContext);

void configureService(ServiceCallback serviceCallback,
                      const moe::Environment& params,
                      const NtServiceDefaultStrings& defaultStrings,
                      const std::vector<std::string>& disallowedOptions,
                      const std::vector<std::string>& argv) {
    bool installService = false;
    bool removeService = false;
    bool reinstallService = false;

    _serviceCallback = serviceCallback;

    int badOption = -1;
    for (size_t i = 0; i < disallowedOptions.size(); ++i) {
        if (params.count(disallowedOptions[i])) {
            badOption = i;
            break;
        }
    }

    _serviceName = defaultStrings.serviceName;
    wstring windowsServiceDisplayName(defaultStrings.displayName);
    wstring windowsServiceDescription(defaultStrings.serviceDescription);
    wstring windowsServiceUser;
    wstring windowsServicePassword;

    if (params.count("install")) {
        if (badOption != -1) {
            LOGV2(23287,
                  "--install cannot be used with option",
                  "option"_attr = disallowedOptions[badOption]);
            quickExit(ExitCode::badOptions);
        }
        if (!params.count("systemLog.destination") ||
            params["systemLog.destination"].as<std::string>() != "file") {
            LOGV2(23288, "--install has to be used with a log file for server output");
            quickExit(ExitCode::badOptions);
        }
        installService = true;
    }
    if (params.count("reinstall")) {
        if (badOption != -1) {
            LOGV2(23289,
                  "--reinstall cannot be used with option",
                  "option"_attr = disallowedOptions[badOption]);
            quickExit(ExitCode::badOptions);
        }
        if (!params.count("systemLog.destination") ||
            params["systemLog.destination"].as<std::string>() != "file") {
            LOGV2(23290, "--reinstall has to be used with a log file for server output");
            quickExit(ExitCode::badOptions);
        }
        reinstallService = true;
    }
    if (params.count("remove")) {
        if (badOption != -1) {
            LOGV2(23291,
                  "--remove cannot be used with option",
                  "option"_attr = disallowedOptions[badOption]);
            quickExit(ExitCode::badOptions);
        }
        removeService = true;
    }
    if (params.count("service")) {
        if (badOption != -1) {
            LOGV2(23292,
                  "--service cannot be used with option",
                  "option"_attr = disallowedOptions[badOption]);
            quickExit(ExitCode::badOptions);
        }
        _startService = true;
    }

    if (params.count("processManagement.windowsService.serviceName")) {
        if (badOption != -1) {
            LOGV2(23293,
                  "--serviceName cannot be used with option",
                  "option"_attr = disallowedOptions[badOption]);
            quickExit(ExitCode::badOptions);
        }
        _serviceName = toWideString(
            params["processManagement.windowsService.serviceName"].as<string>().c_str());
    }
    if (params.count("processManagement.windowsService.displayName")) {
        if (badOption != -1) {
            LOGV2(23294,
                  "--serviceDisplayName cannot be used with option",
                  "option"_attr = disallowedOptions[badOption]);
            quickExit(ExitCode::badOptions);
        }
        windowsServiceDisplayName = toWideString(
            params["processManagement.windowsService.displayName"].as<string>().c_str());
    }
    if (params.count("processManagement.windowsService.description")) {
        if (badOption != -1) {
            LOGV2(23295,
                  "--serviceDescription cannot be used with option",
                  "option"_attr = disallowedOptions[badOption]);
            quickExit(ExitCode::badOptions);
        }
        windowsServiceDescription = toWideString(
            params["processManagement.windowsService.description"].as<string>().c_str());
    }
    if (params.count("processManagement.windowsService.serviceUser")) {
        if (badOption != -1) {
            LOGV2(23296,
                  "--serviceUser cannot be used with option",
                  "option"_attr = disallowedOptions[badOption]);
            quickExit(ExitCode::badOptions);
        }
        windowsServiceUser = toWideString(
            params["processManagement.windowsService.serviceUser"].as<string>().c_str());
    }
    if (params.count("processManagement.windowsService.servicePassword")) {
        if (badOption != -1) {
            LOGV2(23297,
                  "--servicePassword cannot be used with option",
                  "option"_attr = disallowedOptions[badOption]);
            quickExit(ExitCode::badOptions);
        }
        windowsServicePassword = toWideString(
            params["processManagement.windowsService.servicePassword"].as<string>().c_str());
    }

    if (installService || reinstallService) {
        if (reinstallService) {
            removeServiceOrDie(_serviceName);
        }

        installServiceOrDie(_serviceName,
                            windowsServiceDisplayName,
                            windowsServiceDescription,
                            windowsServiceUser,
                            windowsServicePassword,
                            argv,
                            reinstallService);
        quickExit(ExitCode::clean);
    } else if (removeService) {
        removeServiceOrDie(_serviceName);
        quickExit(ExitCode::clean);
    }
}

// This implementation assumes that inputArgv was a valid argv to mongod.  That is, it assumes
// that options that take arguments received them, and options that do not take arguments did
// not.
std::vector<std::string> constructServiceArgv(const std::vector<std::string>& inputArgv) {
    static const char* const optionsWithoutArgumentsToStrip[] = {
        "-install", "--install", "-reinstall", "--reinstall", "-service", "--service"};

    // Pointer to just past the end of optionsWithoutArgumentsToStrip, for use as an "end"
    // iterator.
    static const char* const* const optionsWithoutArgumentsToStripEnd =
        optionsWithoutArgumentsToStrip + boost::size(optionsWithoutArgumentsToStrip);

    static const char* const optionsWithArgumentsToStrip[] = {"-serviceName",
                                                              "--serviceName",
                                                              "-serviceUser",
                                                              "--serviceUser",
                                                              "-servicePassword",
                                                              "--servicePassword",
                                                              "-serviceDescription",
                                                              "--serviceDescription",
                                                              "-serviceDisplayName",
                                                              "--serviceDisplayName"};

    // Pointer to just past the end of optionsWithArgumentsToStrip, for use as an "end"
    // iterator.
    static const char* const* const optionsWithArgumentsToStripEnd =
        optionsWithArgumentsToStrip + boost::size(optionsWithArgumentsToStrip);

    std::vector<std::string> result;
    for (std::vector<std::string>::const_iterator iter = inputArgv.begin(), end = inputArgv.end();
         iter != end;
         ++iter) {
        if (optionsWithoutArgumentsToStripEnd !=
            std::find(optionsWithoutArgumentsToStrip, optionsWithoutArgumentsToStripEnd, *iter)) {
            // The current element of inputArgv is an option that we wish to strip, that takes
            // no arguments.  Skip adding it to "result".
            continue;
        }

        StringData name;
        StringData value;
        bool foundEqualSign = str::splitOn(*iter, '=', name, value);
        if (!foundEqualSign)
            name = *iter;
        if (optionsWithArgumentsToStripEnd !=
            std::find(optionsWithArgumentsToStrip, optionsWithArgumentsToStripEnd, name)) {
            // The current element, and maybe the next one, form an option and its argument.
            // Skip adding them to "result".
            if (!foundEqualSign) {
                // The next argv value must be the argument to the parameter, so strip it.
                ++iter;
            }
            continue;
        }

        result.push_back(*iter);
    }

    result.push_back("--service");  // Service command lines all contain "--service".
    return result;
}

void installServiceOrDie(const wstring& serviceName,
                         const wstring& displayName,
                         const wstring& serviceDesc,
                         const wstring& serviceUser,
                         const wstring& servicePassword,
                         const std::vector<std::string>& argv,
                         const bool reinstall) {
    LOGV2(
        23298, "Trying to install Windows service", "serviceName"_attr = toUtf8String(serviceName));

    std::vector<std::string> serviceArgv = constructServiceArgv(argv);

    char exePath[1024];
    GetModuleFileNameA(nullptr, exePath, sizeof exePath);
    serviceArgv.at(0) = exePath;

    std::string commandLine = constructUtf8WindowsCommandLine(serviceArgv);

    SC_HANDLE schSCManager = ::OpenSCManager(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (schSCManager == nullptr) {
        DWORD err = ::GetLastError();
        LOGV2(23299,
              "Error connecting to the Service Control Manager",
              "__error__"_attr = windows::GetErrMsg(err));
        quickExit(ExitCode::ntServiceError);
    }

    SC_HANDLE schService = nullptr;
    int retryCount = 10;

    while (true) {
        // Make sure service doesn't already exist.
        // TODO: Check to see if service is in "Deleting" status, suggest the user close down
        // Services MMC snap-ins.
        schService = ::OpenService(schSCManager, serviceName.c_str(), SERVICE_ALL_ACCESS);
        if (schService != nullptr) {
            LOGV2(23300,
                  "There is already a service witht the same name, retrying",
                  "serviceName"_attr = toUtf8String(serviceName),
                  "retrying"_attr = (retryCount > 0 ? true : false));
            ::CloseServiceHandle(schService);

            // If we are reinstalling the service, but SCM thinks it is installed, then wait
            // and try again
            if (--retryCount > 0 && reinstall) {
                sleepmillis(500);
                continue;
            }

            ::CloseServiceHandle(schSCManager);
            quickExit(ExitCode::ntServiceError);
        } else {
            break;
        }
    }

    std::wstring commandLineWide = toWideString(commandLine.c_str());

    // create new service
    schService = ::CreateServiceW(schSCManager,               // Service Control Manager handle
                                  serviceName.c_str(),        // service name
                                  displayName.c_str(),        // service display name
                                  SERVICE_ALL_ACCESS,         // desired access
                                  SERVICE_WIN32_OWN_PROCESS,  // service type
                                  SERVICE_AUTO_START,         // start type
                                  SERVICE_ERROR_NORMAL,       // error control
                                  commandLineWide.c_str(),    // command line
                                  nullptr,                    // load order group
                                  nullptr,                    // tag id
                                  L"\0\0",                    // dependencies
                                  nullptr,                    // user account
                                  nullptr);                   // user account password
    if (schService == nullptr) {
        DWORD err = ::GetLastError();
        LOGV2(23301, "Error creating service", "__error__"_attr = windows::GetErrMsg(err));
        ::CloseServiceHandle(schSCManager);
        quickExit(ExitCode::ntServiceError);
    }

    LOGV2(23302,
          "Service installed",
          "serviceName"_attr = toUtf8String(serviceName),
          "displayName"_attr = toUtf8String(displayName),
          "commandLine"_attr = commandLine);

    string typeableName((serviceName.find(L' ') != wstring::npos)
                            ? "\"" + toUtf8String(serviceName) + "\""
                            : toUtf8String(serviceName));
    LOGV2(23303,
          "Service can be started from the command line with 'net start <serviceName>'",
          "serviceName"_attr = typeableName);

    bool serviceInstalled;

    // TODO: If necessary grant user "Login as a Service" permission.
    if (!serviceUser.empty()) {
        wstring actualServiceUser;
        if (serviceUser.find(L"\\") == string::npos) {
            actualServiceUser = L".\\" + serviceUser;
        } else {
            actualServiceUser = serviceUser;
        }

        LOGV2(23304,
              "Setting service login credentials for user",
              "user"_attr = toUtf8String(actualServiceUser));

        serviceInstalled = ::ChangeServiceConfig(schService,                 // service handle
                                                 SERVICE_NO_CHANGE,          // service type
                                                 SERVICE_NO_CHANGE,          // start type
                                                 SERVICE_NO_CHANGE,          // error control
                                                 nullptr,                    // path
                                                 nullptr,                    // load order group
                                                 nullptr,                    // tag id
                                                 nullptr,                    // dependencies
                                                 actualServiceUser.c_str(),  // user account
                                                 servicePassword.c_str(),  // user account password
                                                 nullptr);                 // service display name
        if (!serviceInstalled) {
            LOGV2(23305, "Setting service login failed, service has 'LocalService' permissions");
        }
    }

    // set the service description
    SERVICE_DESCRIPTION serviceDescription;
    serviceDescription.lpDescription = (LPTSTR)serviceDesc.c_str();
    serviceInstalled =
        ::ChangeServiceConfig2(schService, SERVICE_CONFIG_DESCRIPTION, &serviceDescription);

#if 1
    if (!serviceInstalled) {
#else
    // This code sets the mongod service to auto-restart, forever. This might be a fine thing to do
    // except that when mongod or Windows has a crash, the mongo.lock file is still around, so any
    // attempt at a restart will immediately fail.  With auto-restart, we go into a loop, crashing
    // and restarting, crashing and restarting, until someone comes in and disables the service or
    // deletes the mongod.lock file.
    //
    // I'm leaving the old code here for now in case we solve this and are able to turn
    // SC_ACTION_RESTART
    // back on.
    //
    if (serviceInstalled) {
        SC_ACTION aActions[3] = {
            {SC_ACTION_RESTART, 0}, {SC_ACTION_RESTART, 0}, {SC_ACTION_RESTART, 0}};

        SERVICE_FAILURE_ACTIONS serviceFailure;
        ZeroMemory(&serviceFailure, sizeof(SERVICE_FAILURE_ACTIONS));
        serviceFailure.cActions = 3;
        serviceFailure.lpsaActions = aActions;

        // set service recovery options
        serviceInstalled =
            ::ChangeServiceConfig2(schService, SERVICE_CONFIG_FAILURE_ACTIONS, &serviceFailure);

    } else {
#endif
        LOGV2(23306,
              "Could not set service description. Check the Windows Event Log for more details.");
    }

    // Set the pre-shutdown notification with a timeout of 10 minutes.
    // Windows will either wait for us to finish with SERVICE_STOPPED or it will timeout, whichever
    // is first.
    SERVICE_PRESHUTDOWN_INFO servicePreshutdownInfo;
    servicePreshutdownInfo.dwPreshutdownTimeout = 10 * 60 * 1000;  // 10 minutes

    BOOL ret = ::ChangeServiceConfig2(
        schService, SERVICE_CONFIG_PRESHUTDOWN_INFO, &servicePreshutdownInfo);
    if (!ret) {
        auto ec = lastSystemError();
        LOGV2_ERROR(23317,
                    "Failed to set timeout for pre-shutdown notification",
                    "__error__"_attr = errorMessage(ec));
        serviceInstalled = false;
    }

    ::CloseServiceHandle(schService);
    ::CloseServiceHandle(schSCManager);

    if (!serviceInstalled)
        quickExit(ExitCode::ntServiceError);
}

void removeServiceOrDie(const wstring& serviceName) {
    LOGV2(23307, "Trying to remove Windows service", "name"_attr = toUtf8String(serviceName));

    SC_HANDLE schSCManager = ::OpenSCManager(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (schSCManager == nullptr) {
        DWORD err = ::GetLastError();
        LOGV2(23308,
              "Error connecting to the Service Control Manager",
              "__error__"_attr = windows::GetErrMsg(err));
        quickExit(ExitCode::ntServiceError);
    }

    SC_HANDLE schService = ::OpenService(schSCManager, serviceName.c_str(), SERVICE_ALL_ACCESS);
    if (schService == nullptr) {
        LOGV2(23309,
              "Could not find a service named to remove",
              "serviceName"_attr = toUtf8String(serviceName));
        ::CloseServiceHandle(schSCManager);
        quickExit(ExitCode::ntServiceError);
    }

    SERVICE_STATUS serviceStatus;

    // stop service if its running
    if (::ControlService(schService, SERVICE_CONTROL_STOP, &serviceStatus)) {
        LOGV2(23310,
              "Service is currently running, stopping service",
              "serviceName"_attr = toUtf8String(serviceName));
        while (::QueryServiceStatus(schService, &serviceStatus)) {
            if (serviceStatus.dwCurrentState == SERVICE_STOP_PENDING) {
                Sleep(1000);
            } else {
                break;
            }
        }
        LOGV2(23311, "Service stopped", "serviceName"_attr = toUtf8String(serviceName));
    }

    bool serviceRemoved = ::DeleteService(schService);

    ::CloseServiceHandle(schService);
    ::CloseServiceHandle(schSCManager);

    if (serviceRemoved) {
        LOGV2(23312, "Service removed", "serviceName"_attr = toUtf8String(serviceName));
    } else {
        LOGV2(23313, "Failed to remove service", "serviceName"_attr = toUtf8String(serviceName));
    }

    if (!serviceRemoved)
        quickExit(ExitCode::ntServiceError);
}

bool reportStatus(DWORD reportState, DWORD waitHint, DWORD exitCode) {
    if (_statusHandle == nullptr)
        return false;

    static DWORD checkPoint = 1;

    SERVICE_STATUS ssStatus;

    DWORD dwControlsAccepted;
    switch (reportState) {
        case SERVICE_START_PENDING:
        case SERVICE_STOP_PENDING:
        case SERVICE_STOPPED:
            dwControlsAccepted = 0;
            break;
        default:
            dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PRESHUTDOWN;
            break;
    }

    ssStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    ssStatus.dwServiceSpecificExitCode = exitCode;
    ssStatus.dwControlsAccepted = dwControlsAccepted;
    ssStatus.dwCurrentState = reportState;

    // Only report ERROR_SERVICE_SPECIFIC_ERROR when the exit is not clean
    if (reportState == SERVICE_STOPPED && exitCode != static_cast<DWORD>(ExitCode::clean))
        ssStatus.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
    else
        ssStatus.dwWin32ExitCode = NO_ERROR;

    ssStatus.dwWaitHint = waitHint;
    ssStatus.dwCheckPoint =
        (reportState == SERVICE_RUNNING || reportState == SERVICE_STOPPED) ? 0 : checkPoint++;

    return SetServiceStatus(_statusHandle, &ssStatus);
}

// Minimum of time we tell Windows to wait before we are guilty of a hung shutdown
const int kStopWaitHintMillis = 30000;

// Run shutdownNoTerminate on a separate thread so we can report progress to Windows
// Note: Windows may still kill us for taking too long,
// On client OSes, SERVICE_CONTROL_SHUTDOWN has a 5 second timeout configured in
// HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control
static void serviceStop() {
    stdx::packaged_task<void()> shutdownNoTerminateTask([] {
        setThreadName("serviceStopWorker");
        // Stop the process
        shutdownNoTerminate();
        return true;
    });
    stdx::future<void> exitedCleanly = shutdownNoTerminateTask.get_future();

    // Launch the packaged task in a thread. We needn't ever join it,
    // so it doesn't even need a name.
    stdx::thread(std::move(shutdownNoTerminateTask)).detach();

    const auto timeout = Milliseconds(kStopWaitHintMillis / 2);

    // We periodically check if we are done exiting by polling at half of each wait interval
    while (exitedCleanly.wait_for(timeout.toSystemDuration()) != stdx::future_status::ready) {
        reportStatus(SERVICE_STOP_PENDING, kStopWaitHintMillis);
        LOGV2(23314, "Service Stop is waiting for storage engine to finish shutdown");
    }
}

static void WINAPI initService(DWORD argc, LPTSTR* argv) {
    _statusHandle = RegisterServiceCtrlHandlerEx(_serviceName.c_str(), serviceCtrl, nullptr);
    if (!_statusHandle)
        return;

    reportStatus(SERVICE_START_PENDING, 1000);

    ExitCode exitCode = _serviceCallback();

    // During clean shutdown, ie NT SCM signals us, _serviceCallback returns here
    // as part of the listener loop terminating.
    // exitCleanly is supposed to return. If it blocks, some other thread must be exiting.
    //
    serviceStop();

    reportStatus(SERVICE_STOPPED, 0, static_cast<DWORD>(exitCode));
}

static void serviceShutdown(const char* controlCodeName) {
    setThreadName("serviceShutdown");

    LOGV2(23315,
          "Received request from Windows Service Control Manager",
          "code"_attr = controlCodeName,
          "inShutdown"_attr = (globalInShutdownDeprecated() ? "true" : "false"));

    reportStatus(SERVICE_STOP_PENDING, kStopWaitHintMillis);

    // Note: This triggers _serviceCallback, ie  ServiceMain,
    // to stop by setting globalInShutdownDeprecated() == true
    shutdownNoTerminate();

    // Note: we will report exit status in initService
}

static DWORD WINAPI serviceCtrl(DWORD dwControl,
                                DWORD dwEventType,
                                LPVOID lpEventData,
                                LPVOID lpContext) {
    switch (dwControl) {
        case SERVICE_CONTROL_INTERROGATE:
            // Return NO_ERROR per MSDN even though we do nothing for this control code.
            return NO_ERROR;
        case SERVICE_CONTROL_STOP:
            serviceShutdown("SERVICE_CONTROL_STOP");
            // Return NO_ERROR since we handle the STOP
            return NO_ERROR;
        case SERVICE_CONTROL_PRESHUTDOWN:
            serviceShutdown("SERVICE_CONTROL_PRESHUTDOWN");
            // Return NO_ERROR since we handle the PRESHUTDOWN
            return NO_ERROR;
    }

    // Return ERROR_CALL_NOT_IMPLEMENTED as the default
    return ERROR_CALL_NOT_IMPLEMENTED;
}

void startService() {
    fassert(16454, _startService);

    // Remove the Control-C handler so that we properly process SERVICE_CONTROL_SHUTDOWN
    // via the service handler instead of CTRL_SHUTDOWN_EVENT via the Control-C Handler
    removeControlCHandler();

    SERVICE_TABLE_ENTRYW dispTable[] = {
        {const_cast<LPWSTR>(_serviceName.c_str()), (LPSERVICE_MAIN_FUNCTION)initService},
        {nullptr, nullptr}};

    LOGV2(
        23316, "Trying to start Windows service", "serviceName"_attr = toUtf8String(_serviceName));
    if (StartServiceCtrlDispatcherW(dispTable)) {
        quickExit(ExitCode::clean);
    } else {
        ::exit(static_cast<int>(ExitCode::ntServiceError));
    }
}

}  // namespace ntservice
}  // namespace mongo

#endif
