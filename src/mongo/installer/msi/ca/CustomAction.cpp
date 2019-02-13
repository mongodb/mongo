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

/**
 * Note: This file is included in the install package and used during installation.
 *
 * It supposed to be linked to a static C-runtime, and should not depend on other MongoDB
 * components.
 */

#define WIN32_LEAN_AND_MEAN  // Exclude rarely-used stuff from Windows headers

#include "targetver.h"

// clang-format off
#include <windows.h>
#include <msiquery.h>
// clang-format on

#include <memory>
#include <string>
#include <vector>

#include "mongo/util/scopeguard.h"

// UpdateMongoYAML CustomAction Constants
constexpr wchar_t kBIN[] = L"BIN";
constexpr wchar_t kMongoDataPath[] = L"MONGO_DATA_PATH";
constexpr wchar_t kMongoLogPath[] = L"MONGO_LOG_PATH";

// YAML Subsitution Constants
constexpr char kMongoDataPathYaml[] = "%MONGO_DATA_PATH%";
constexpr char kMongoLogPathYaml[] = "%MONGO_LOG_PATH%";

// Service Account Constants - from Installer_64.wxs
constexpr wchar_t kMongoServiceAccountName[] = L"MONGO_SERVICE_ACCOUNT_NAME";
constexpr wchar_t kMongoServiceAccountPassword[] = L"MONGO_SERVICE_ACCOUNT_PASSWORD";
constexpr wchar_t kMongoServiceAccountDomain[] = L"MONGO_SERVICE_ACCOUNT_DOMAIN";

/**
 * Log a messge to MSIExec's log file.
 *
 * Does not work for immediately executed custom actions.
 */
HRESULT LogMessage(MSIHANDLE hInstall, INSTALLMESSAGE eMessageType, char const* format, ...) {
    va_list args;
    const size_t bufSize = 4096;
    char buf[bufSize];

    va_start(args, format);
    vsnprintf_s(buf, bufSize, format, args);
    va_end(args);

    PMSIHANDLE hRecord = ::MsiCreateRecord(1);

    if (NULL != hRecord) {
        HRESULT hr = ::MsiRecordSetStringA(hRecord, 0, buf);
        if (SUCCEEDED(hr)) {
            return ::MsiProcessMessage(hInstall, eMessageType, hRecord);
        }

        return hr;
    }

    return E_FAIL;
}

/**
 * Replace original with replacement with a string. Logs a warning if original is not found.
 */
std::string do_replace(MSIHANDLE hInstall,
                       std::string source,
                       std::string original,
                       std::string replacement) {
    int pos = source.find(original, 0);

    if (pos == std::string::npos) {
        LogMessage(hInstall,
                   INSTALLMESSAGE_INFO,
                   "Failed to find '%s' in '%s'",
                   original.c_str(),
                   source.c_str());
        return source;
    }

    return source.replace(pos, original.length(), replacement);
}

/**
 * Get a property from MSI
 */
HRESULT GetProperty(MSIHANDLE hInstall, const wchar_t* pwszName, std::wstring* outString) {
    DWORD size = 0;
    WCHAR emptyString[1] = L"";
    *outString = std::wstring();

    UINT ret = MsiGetPropertyW(hInstall, pwszName, emptyString, &size);

    if (ret != ERROR_MORE_DATA) {
        LogMessage(hInstall,
                   INSTALLMESSAGE_WARNING,
                   "Received UINT %x during GetProperty size check",
                   ret);
        return E_FAIL;
    }

    ++size;  // bump for null terminator

    std::unique_ptr<wchar_t[]> buf(new wchar_t[size]);

    ret = MsiGetPropertyW(hInstall, pwszName, buf.get(), &size);
    if (ret != ERROR_SUCCESS) {
        LogMessage(hInstall, INSTALLMESSAGE_WARNING, "Received UINT %x during GetProperty", ret);
        return E_FAIL;
    }

    *outString = std::wstring(buf.get());

    return S_OK;
}

std::string toUtf8String(MSIHANDLE hInstall, const std::wstring& wide) {
    if (wide.size() == 0)
        return "";

    // Calculate necessary buffer size
    int len = ::WideCharToMultiByte(
        CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), NULL, 0, NULL, NULL);

    // Perform actual conversion
    if (len > 0) {
        std::vector<char> buffer(len);
        len = ::WideCharToMultiByte(CP_UTF8,
                                    0,
                                    wide.c_str(),
                                    static_cast<int>(wide.size()),
                                    &buffer[0],
                                    static_cast<int>(buffer.size()),
                                    NULL,
                                    NULL);
        if (len > 0) {
            return std::string(&buffer[0], buffer.size());
        }
    }

    LogMessage(hInstall, INSTALLMESSAGE_WARNING, " Failed to convert string to UTF-8");
    return "";
}

// Support macros
//
#define CHECKHR_AND_LOG(hr, ...)                                                 \
                                                                                 \
    if (!SUCCEEDED(hr)) {                                                        \
        LogMessage(hInstall, INSTALLMESSAGE_WARNING, "Received HRESULT %x", hr); \
        LogMessage(hInstall, INSTALLMESSAGE_WARNING, __VA_ARGS__);               \
        goto Exit;                                                               \
    }

#define CHECKGLE_AND_LOG(...)                                                           \
                                                                                        \
    {                                                                                   \
        LONG _gle = GetLastError();                                                     \
        LogMessage(hInstall, INSTALLMESSAGE_WARNING, "Received GetLastError %x", _gle); \
        LogMessage(hInstall, INSTALLMESSAGE_WARNING, __VA_ARGS__);                      \
        hr = E_FAIL;                                                                    \
        goto Exit;                                                                      \
    }

#define CHECKUINT_AND_LOG(x)                                                                   \
                                                                                               \
    {                                                                                          \
        UINT _retUINT = (x);                                                                   \
        if (_retUINT != ERROR_SUCCESS) {                                                       \
            LogMessage(                                                                        \
                hInstall, INSTALLMESSAGE_WARNING, "Received Return %x from %s", _retUINT, #x); \
            hr = E_FAIL;                                                                       \
            goto Exit;                                                                         \
        }                                                                                      \
    }

#define LOG_INFO(...) \
                      \
    { LogMessage(hInstall, INSTALLMESSAGE_INFO, __VA_ARGS__); }

/**
 * UpdateMongoYAML - MSI custom action entry point
 *
 * Transforms a template yaml file into a file contain data and log directory of user's choosing.
 *
 * TODO: ACL directories
 */
extern "C" UINT __stdcall UpdateMongoYAML(MSIHANDLE hInstall) {
    HRESULT hr = S_OK;

    try {
        std::wstring customData;
        hr = GetProperty(hInstall, L"CustomActionData", &customData);
        CHECKHR_AND_LOG(hr, "Failed to get CustomActionData property");

        LOG_INFO("CA - Custom Data = %ls", customData.c_str());

        std::wstring binPath;
        std::wstring dataDir;
        std::wstring logDir;

        int start = 0;
        while (true) {
            int pos = customData.find(';', start);
            if (pos == std::wstring::npos) {
                pos = customData.size();
            }

            std::wstring term = customData.substr(start, pos - start - 1);
            int equals = term.find('=');
            if (equals == std::wstring::npos) {
                LOG_INFO("CA - Error searching = %ls", term.c_str());
            }

            std::wstring keyword = term.substr(0, equals);
            std::wstring value = term.substr(equals + 1);

            if (keyword == kBIN) {
                binPath = value;
            } else if (keyword == kMongoDataPath) {
                dataDir = value;
            } else if (keyword == kMongoLogPath) {
                logDir = value;
            }

            if (pos == customData.size()) {
                break;
            }

            start = pos + 1;
        }

        std::wstring YamlFile(binPath);
        YamlFile += L"\\mongod.cfg";

        LOG_INFO("CA - BIN = %ls", binPath.c_str());
        LOG_INFO("CA - MONGO_DATA_PATH = %ls", dataDir.c_str());
        LOG_INFO("CA - MONGO_LOG_PATH = %ls", logDir.c_str());
        LOG_INFO("CA - YAML_FILE = %ls", YamlFile.c_str());

        long gle = GetFileAttributesW(YamlFile.c_str());
        if (gle == INVALID_FILE_ATTRIBUTES) {
            CHECKGLE_AND_LOG("Failed to find yaml file");
        }

        HANDLE hFile = CreateFileW(YamlFile.c_str(),
                                   (GENERIC_READ | GENERIC_WRITE),
                                   0,
                                   NULL,
                                   OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL,
                                   NULL);
        if (hFile == INVALID_HANDLE_VALUE) {
            CHECKGLE_AND_LOG("Failed to open yaml file");
        }

        const auto handleGuard = mongo::makeGuard([&] { CloseHandle(hFile); });

        LARGE_INTEGER fileSize;
        if (GetFileSizeEx(hFile, &fileSize) == 0) {
            CHECKGLE_AND_LOG("Failed to get size of yaml file");
        }

        LOG_INFO("CA - Allocating - %lld bytes", fileSize.QuadPart);

        size_t bufSize = static_cast<size_t>(fileSize.QuadPart + 1);

        std::unique_ptr<char> buf(new char[bufSize]);

        LOG_INFO("CA - Reading file - %d bytes", bufSize);

        DWORD read;
        if (!ReadFile(hFile, (void*)buf.get(), bufSize, &read, NULL)) {
            CHECKGLE_AND_LOG("Failed to read yaml file");
        }

        buf.get()[read] = '\0';

        LOG_INFO("CA - Reading file - '%s'", buf.get());

        std::string str(buf.get());

        // Do the string subsitutions
        str = do_replace(hInstall, str, kMongoDataPathYaml, toUtf8String(hInstall, dataDir));
        str = do_replace(hInstall, str, kMongoLogPathYaml, toUtf8String(hInstall, logDir));

        LOG_INFO("CA - Writing file - '%s'", buf.get());

        SetFilePointer(hFile, 0, 0, SEEK_SET);

        DWORD written;
        if (!WriteFile(hFile, str.c_str(), str.length(), &written, NULL)) {
            CHECKGLE_AND_LOG("Failed to write yaml file");
        }
    } catch (const std::exception& e) {
        CHECKHR_AND_LOG(E_FAIL, "Caught C++ exception %s", e.what());
    } catch (...) {
        CHECKHR_AND_LOG(E_FAIL, "Caught exception");
    }

Exit:
    return SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
}

/**
 * ValidateServiceLogon - MSI custom action entry point.
 *
 * Validates a (domain, user, password) tuple is suitable is correct and valid for a service login.
 */
extern "C" UINT __stdcall ValidateServiceLogon(MSIHANDLE hInstall) {
    HRESULT hr = S_OK;

    /**
    // Debugging Hook:
    // The only way to find the process to attach is via PID in this Message Box.
    char buf[256];
    sprintf(&buf[0], "Validating %d", GetCurrentProcessId());

    MessageBoxA(NULL, &buf[0], "FOOBAR", MB_OK | MB_ICONEXCLAMATION | MB_SETFOREGROUND);
    */

    try {
        std::wstring userName;
        std::wstring password;
        std::wstring domain;

        hr = GetProperty(hInstall, kMongoServiceAccountName, &userName);
        CHECKHR_AND_LOG(hr, "Failed to get MONGO_SERVICE_ACCOUNT_NAME property");

        hr = GetProperty(hInstall, kMongoServiceAccountPassword, &password);
        CHECKHR_AND_LOG(hr, "Failed to get MONGO_SERVICE_ACCOUNT_PASSWORD property");

        hr = GetProperty(hInstall, kMongoServiceAccountDomain, &domain);
        CHECKHR_AND_LOG(hr, "Failed to get MONGO_SERVICE_ACCOUNT_DOMAIN property");

        // Check if the user name and password is valid, and the user has the "Log on as a service"
        // privilege.
        HANDLE hLogonToken;
        BOOL ret = LogonUserW(userName.c_str(),
                              domain.c_str(),
                              password.c_str(),
                              LOGON32_LOGON_SERVICE,
                              LOGON32_PROVIDER_DEFAULT,
                              &hLogonToken);
        if (ret) {
            // User name and password is right
            CloseHandle(hLogonToken);
            CHECKUINT_AND_LOG(MsiSetPropertyA(hInstall, "MONGO_SERVICE_ACCOUNT_VALID", "1"));
            return ERROR_SUCCESS;
        }

        DWORD gle = GetLastError();
        if (gle == ERROR_LOGON_TYPE_NOT_GRANTED) {
            // Check if the user can logon interactive since we have the right user name and
            // password but the wrong privileges. We will grant the right privilege later in setup.

            ret = LogonUserW(userName.c_str(),
                             domain.c_str(),
                             password.c_str(),
                             LOGON32_LOGON_INTERACTIVE,
                             LOGON32_PROVIDER_DEFAULT,
                             &hLogonToken);
            if (ret) {
                // User name and password is right
                CloseHandle(hLogonToken);
                CHECKUINT_AND_LOG(MsiSetPropertyA(hInstall, "MONGO_SERVICE_ACCOUNT_VALID", "1"));
                return ERROR_SUCCESS;
            }
        } else if (gle != ERROR_LOGON_FAILURE) {
            CHECKGLE_AND_LOG("Could not logon user");
        }

        // User name and/or password is wrong
        CHECKUINT_AND_LOG(MsiSetPropertyA(hInstall, "MONGO_SERVICE_ACCOUNT_VALID", "0"));

    } catch (const std::exception& e) {
        CHECKHR_AND_LOG(E_FAIL, "Caught C++ exception %s", e.what());
    } catch (...) {
        CHECKHR_AND_LOG(E_FAIL, "Caught exception");
    }

Exit:
    return SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
}

// DllMain - Initialize and cleanup WiX custom action utils.
extern "C" BOOL WINAPI DllMain(__in HINSTANCE hInst, __in ULONG ulReason, __in LPVOID) {
    switch (ulReason) {
        case DLL_PROCESS_ATTACH:
            break;

        case DLL_PROCESS_DETACH:
            break;
    }

    return TRUE;
}
