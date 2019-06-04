/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   event_log_registry.hpp
 * \author Andrey Semashev
 * \date   16.11.2008
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#ifndef BOOST_LOG_WINDOWS_EVENT_LOG_REGISTRY_HPP_INCLUDED_
#define BOOST_LOG_WINDOWS_EVENT_LOG_REGISTRY_HPP_INCLUDED_

#include <boost/log/detail/config.hpp>
#include <cwchar>
#include <cstring>
#include <string>
#include <sstream>
#include <stdexcept>
#include <boost/version.hpp>
#include <boost/optional/optional.hpp>
#include <boost/log/detail/code_conversion.hpp>
#include <boost/log/exceptions.hpp>
#include <windows.h>
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace sinks {

namespace aux {

// MSVC versions up to 2008 (or their Platform SDKs, to be more precise) don't define LSTATUS.
// Perhaps, that is also the case for MinGW and Cygwin (untested).
typedef DWORD LSTATUS;

// Max registry string size, in characters (for security reasons)
const DWORD max_string_size = 64u * 1024u;

//! Helper traits to integrate with WinAPI
template< typename CharT >
struct registry_traits;

#ifdef BOOST_LOG_USE_CHAR
template< >
struct registry_traits< char >
{
    static std::string make_event_log_key(std::string const& log_name, std::string const& source_name)
    {
        return "SYSTEM\\CurrentControlSet\\Services\\EventLog\\" + log_name + "\\" + source_name;
    }

    static std::string make_default_log_name()
    {
        return "Application";
    }

    static std::string make_default_source_name()
    {
        char buf[MAX_PATH];
        DWORD size = GetModuleFileNameA(NULL, buf, sizeof(buf) / sizeof(*buf));

        std::string source_name(buf, buf + size);
        if (source_name.empty())
        {
            // In case of error we provide artificial application name
            std::ostringstream strm;
            strm << "Boost.Log "
                << static_cast< unsigned int >(BOOST_VERSION / 100000)
                << "."
                << static_cast< unsigned int >(BOOST_VERSION / 100 % 1000)
                << "."
                << static_cast< unsigned int >(BOOST_VERSION % 100);
            source_name = strm.str();
        }
        else
        {
            // Cut off the path and extension
            std::size_t backslash_pos = source_name.rfind('\\');
            if (backslash_pos == std::string::npos || backslash_pos >= source_name.size() - 1)
                backslash_pos = 0;
            else
                ++backslash_pos;
            std::size_t dot_pos = source_name.rfind('.');
            if (dot_pos == std::string::npos || dot_pos < backslash_pos)
                dot_pos = source_name.size();
            source_name = source_name.substr(backslash_pos, dot_pos - backslash_pos);
        }

        return source_name;
    }

    static LSTATUS create_key(
        HKEY hKey,
        const char* lpSubKey,
        DWORD Reserved,
        char* lpClass,
        DWORD dwOptions,
        REGSAM samDesired,
        LPSECURITY_ATTRIBUTES lpSecurityAttributes,
        PHKEY phkResult,
        LPDWORD lpdwDisposition)
    {
        return RegCreateKeyExA(hKey, lpSubKey, Reserved, lpClass, dwOptions, samDesired, lpSecurityAttributes, phkResult, lpdwDisposition);
    }

    static LSTATUS open_key(
        HKEY hKey,
        const char* lpSubKey,
        DWORD dwOptions,
        REGSAM samDesired,
        PHKEY phkResult)
    {
        return RegOpenKeyExA(hKey, lpSubKey, dwOptions, samDesired, phkResult);
    }

    static LSTATUS set_value(
        HKEY hKey,
        const char* lpValueName,
        DWORD Reserved,
        DWORD dwType,
        const BYTE* lpData,
        DWORD cbData)
    {
        return RegSetValueExA(hKey, lpValueName, Reserved, dwType, lpData, cbData);
    }

    static LSTATUS get_value(HKEY hKey, const char* lpValueName, DWORD& value)
    {
        DWORD type = REG_NONE, size = sizeof(value);
        LSTATUS res = RegQueryValueExA(hKey, lpValueName, NULL, &type, reinterpret_cast< LPBYTE >(&value), &size);
        if (res == ERROR_SUCCESS && type != REG_DWORD && type != REG_BINARY)
            res = ERROR_INVALID_DATA;
        return res;
    }

    static LSTATUS get_value(HKEY hKey, const char* lpValueName, std::string& value)
    {
        DWORD type = REG_NONE, size = 0;
        LSTATUS res = RegQueryValueExA(hKey, lpValueName, NULL, &type, NULL, &size);
        if (res == ERROR_SUCCESS && ((type != REG_EXPAND_SZ && type != REG_SZ) || size > max_string_size))
            return ERROR_INVALID_DATA;
        if (size == 0)
            return res;

        value.resize(size);
        res = RegQueryValueExA(hKey, lpValueName, NULL, &type, reinterpret_cast< LPBYTE >(&value[0]), &size);
        value.resize(std::strlen(value.c_str())); // remove extra terminating zero

        return res;
    }

    static const char* get_event_message_file_param_name() { return "EventMessageFile"; }
    static const char* get_category_message_file_param_name() { return "CategoryMessageFile"; }
    static const char* get_category_count_param_name() { return "CategoryCount"; }
    static const char* get_types_supported_param_name() { return "TypesSupported"; }
};
#endif // BOOST_LOG_USE_CHAR

#ifdef BOOST_LOG_USE_WCHAR_T
template< >
struct registry_traits< wchar_t >
{
    static std::wstring make_event_log_key(std::wstring const& log_name, std::wstring const& source_name)
    {
        return L"SYSTEM\\CurrentControlSet\\Services\\EventLog\\" + log_name + L"\\" + source_name;
    }

    static std::wstring make_default_log_name()
    {
        return L"Application";
    }

    static std::wstring make_default_source_name()
    {
        wchar_t buf[MAX_PATH];
        DWORD size = GetModuleFileNameW(NULL, buf, sizeof(buf) / sizeof(*buf));

        std::wstring source_name(buf, buf + size);
        if (source_name.empty())
        {
            // In case of error we provide artificial application name
            std::wostringstream strm;
            strm << L"Boost.Log "
                << static_cast< unsigned int >(BOOST_VERSION / 100000)
                << L"."
                << static_cast< unsigned int >(BOOST_VERSION / 100 % 1000)
                << L"."
                << static_cast< unsigned int >(BOOST_VERSION % 100);
            source_name = strm.str();
        }
        else
        {
            // Cut off the path and extension
            std::size_t backslash_pos = source_name.rfind(L'\\');
            if (backslash_pos == std::wstring::npos || backslash_pos >= source_name.size() - 1)
                backslash_pos = 0;
            else
                ++backslash_pos;
            std::size_t dot_pos = source_name.rfind(L'.');
            if (dot_pos == std::wstring::npos || dot_pos < backslash_pos)
                dot_pos = source_name.size();
            source_name = source_name.substr(backslash_pos, dot_pos - backslash_pos);
        }

        return source_name;
    }

    static LSTATUS create_key(
        HKEY hKey,
        const wchar_t* lpSubKey,
        DWORD Reserved,
        wchar_t* lpClass,
        DWORD dwOptions,
        REGSAM samDesired,
        LPSECURITY_ATTRIBUTES lpSecurityAttributes,
        PHKEY phkResult,
        LPDWORD lpdwDisposition)
    {
        return RegCreateKeyExW(hKey, lpSubKey, Reserved, lpClass, dwOptions, samDesired, lpSecurityAttributes, phkResult, lpdwDisposition);
    }

    static LSTATUS open_key(
        HKEY hKey,
        const wchar_t* lpSubKey,
        DWORD dwOptions,
        REGSAM samDesired,
        PHKEY phkResult)
    {
        return RegOpenKeyExW(hKey, lpSubKey, dwOptions, samDesired, phkResult);
    }

    static LSTATUS set_value(
        HKEY hKey,
        const wchar_t* lpValueName,
        DWORD Reserved,
        DWORD dwType,
        const BYTE* lpData,
        DWORD cbData)
    {
        return RegSetValueExW(hKey, lpValueName, Reserved, dwType, lpData, cbData);
    }

    static LSTATUS get_value(HKEY hKey, const wchar_t* lpValueName, DWORD& value)
    {
        DWORD type = REG_NONE, size = sizeof(value);
        LSTATUS res = RegQueryValueExW(hKey, lpValueName, NULL, &type, reinterpret_cast< LPBYTE >(&value), &size);
        if (res == ERROR_SUCCESS && type != REG_DWORD && type != REG_BINARY)
            res = ERROR_INVALID_DATA;
        return res;
    }

    static LSTATUS get_value(HKEY hKey, const wchar_t* lpValueName, std::wstring& value)
    {
        DWORD type = REG_NONE, size = 0;
        LSTATUS res = RegQueryValueExW(hKey, lpValueName, NULL, &type, NULL, &size);
        size /= sizeof(wchar_t);
        if (res == ERROR_SUCCESS && ((type != REG_EXPAND_SZ && type != REG_SZ) || size > max_string_size))
            return ERROR_INVALID_DATA;
        if (size == 0)
            return res;

        value.resize(size);
        res = RegQueryValueExW(hKey, lpValueName, NULL, &type, reinterpret_cast< LPBYTE >(&value[0]), &size);
        value.resize(std::wcslen(value.c_str())); // remove extra terminating zero

        return res;
    }

    static const wchar_t* get_event_message_file_param_name() { return L"EventMessageFile"; }
    static const wchar_t* get_category_message_file_param_name() { return L"CategoryMessageFile"; }
    static const wchar_t* get_category_count_param_name() { return L"CategoryCount"; }
    static const wchar_t* get_types_supported_param_name() { return L"TypesSupported"; }

};
#endif // BOOST_LOG_USE_WCHAR_T

//! The structure with parameters that have to be registered in the event log registry key
template< typename CharT >
struct registry_params
{
    typedef std::basic_string< CharT > string_type;

    optional< string_type > event_message_file;
    optional< string_type > category_message_file;
    optional< DWORD > category_count;
    optional< DWORD > types_supported;
};

//! A simple guard that closes the registry key on destruction
struct auto_hkey_close
{
    explicit auto_hkey_close(HKEY hk) : hk_(hk) {}
    ~auto_hkey_close() { RegCloseKey(hk_); }

private:
    HKEY hk_;
};

//! The function checks if the event log is already registered
template< typename CharT >
bool verify_event_log_registry(std::basic_string< CharT > const& reg_key, bool force, registry_params< CharT > const& params)
{
    typedef std::basic_string< CharT > string_type;
    typedef registry_traits< CharT > registry;

    // Open the key
    HKEY hkey = 0;
    LSTATUS res = registry::open_key(
        HKEY_LOCAL_MACHINE,
        reg_key.c_str(),
        REG_OPTION_NON_VOLATILE,
        KEY_READ,
        &hkey);
    if (res != ERROR_SUCCESS)
        return false;

    auto_hkey_close hkey_guard(hkey);

    if (force)
    {
        // Verify key values
        if (!!params.event_message_file)
        {
            string_type module_name;
            res = registry::get_value(hkey, registry::get_event_message_file_param_name(), module_name);
            if (res != ERROR_SUCCESS || module_name != params.event_message_file.get())
                return false;
        }

        if (!!params.category_message_file)
        {
            string_type module_name;
            res = registry::get_value(hkey, registry::get_category_message_file_param_name(), module_name);
            if (res != ERROR_SUCCESS || module_name != params.category_message_file.get())
                return false;
        }

        if (!!params.category_count)
        {
            // Set number of categories
            DWORD category_count = 0;
            res = registry::get_value(hkey, registry::get_category_count_param_name(), category_count);
            if (res != ERROR_SUCCESS || category_count != params.category_count.get())
                return false;
        }

        if (!!params.types_supported)
        {
            // Set the supported event types
            DWORD event_types = 0;
            res = registry::get_value(hkey, registry::get_types_supported_param_name(), event_types);
            if (res != ERROR_SUCCESS || event_types != params.types_supported.get())
                return false;
        }
    }

    return true;
}

//! The function initializes the event log registry key
template< typename CharT >
void init_event_log_registry(
    std::basic_string< CharT > const& log_name,
    std::basic_string< CharT > const& source_name,
    bool force,
    registry_params< CharT > const& params)
{
    typedef std::basic_string< CharT > string_type;
    typedef registry_traits< CharT > registry;
    // Registry key name that contains log description
    string_type reg_key = registry::make_event_log_key(log_name, source_name);

    // First check the registry keys and values in read-only mode.
    // This allows to avoid UAC asking for elevated permissions to modify HKLM registry when no modification is actually needed.
    if (verify_event_log_registry(reg_key, force, params))
        return;

    // Create or open the key
    HKEY hkey = 0;
    DWORD disposition = 0;
    LSTATUS res = registry::create_key(
        HKEY_LOCAL_MACHINE,
        reg_key.c_str(),
        0,
        NULL,
        REG_OPTION_NON_VOLATILE,
        KEY_WRITE,
        NULL,
        &hkey,
        &disposition);
    if (res != ERROR_SUCCESS)
        BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Could not create registry key for the event log", (res));

    auto_hkey_close hkey_guard(hkey);

    if (disposition != REG_OPENED_EXISTING_KEY || force)
    {
        // Fill registry values
        if (!!params.event_message_file)
        {
            // Set the module file name that contains event resources
            string_type const& module_name = params.event_message_file.get();
            res = registry::set_value(
                hkey,
                registry::get_event_message_file_param_name(),
                0,
                REG_EXPAND_SZ,
                reinterpret_cast< LPBYTE >(const_cast< CharT* >(module_name.c_str())),
                static_cast< DWORD >((module_name.size() + 1) * sizeof(CharT)));
            if (res != ERROR_SUCCESS)
            {
                BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Could not create registry value "
                    + log::aux::to_narrow(string_type(registry::get_event_message_file_param_name())), (res));
            }
        }

        if (!!params.category_message_file)
        {
            // Set the module file name that contains event category resources
            string_type const& module_name = params.category_message_file.get();
            res = registry::set_value(
                hkey,
                registry::get_category_message_file_param_name(),
                0,
                REG_SZ,
                reinterpret_cast< LPBYTE >(const_cast< CharT* >(module_name.c_str())),
                static_cast< DWORD >((module_name.size() + 1) * sizeof(CharT)));
            if (res != ERROR_SUCCESS)
            {
                BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Could not create registry value "
                    + log::aux::to_narrow(string_type(registry::get_category_message_file_param_name())), (res));
            }
        }

        if (!!params.category_count)
        {
            // Set number of categories
            DWORD category_count = params.category_count.get();
            res = registry::set_value(
                hkey,
                registry::get_category_count_param_name(),
                0,
                REG_DWORD,
                reinterpret_cast< LPBYTE >(&category_count),
                static_cast< DWORD >(sizeof(category_count)));
            if (res != ERROR_SUCCESS)
            {
                BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Could not create registry value "
                    + log::aux::to_narrow(string_type(registry::get_category_count_param_name())), (res));
            }
        }

        if (!!params.types_supported)
        {
            // Set the supported event types
            DWORD event_types = params.types_supported.get();
            res = registry::set_value(
                hkey,
                registry::get_types_supported_param_name(),
                0,
                REG_DWORD,
                reinterpret_cast< LPBYTE >(&event_types),
                static_cast< DWORD >(sizeof(event_types)));
            if (res != ERROR_SUCCESS)
            {
                BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Could not create registry value "
                    + log::aux::to_narrow(string_type(registry::get_types_supported_param_name())), (res));
            }
        }
    }
}

} // namespace aux

} // namespace sinks

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // BOOST_LOG_WINDOWS_EVENT_LOG_REGISTRY_HPP_INCLUDED_
