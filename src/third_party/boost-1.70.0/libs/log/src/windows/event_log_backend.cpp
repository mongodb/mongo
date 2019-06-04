/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   event_log_backend.cpp
 * \author Andrey Semashev
 * \date   07.11.2008
 *
 * \brief  A logging sink backend that uses Windows NT event log API
 *         for signalling application events.
 */

#ifndef BOOST_LOG_WITHOUT_EVENT_LOG

#include <boost/log/detail/config.hpp>
#include <string>
#include <vector>
#include <ostream>
#include <stdexcept>
#include <boost/scoped_array.hpp>
#include <boost/system/windows_error.hpp>
#include <boost/log/exceptions.hpp>
#include <boost/log/sinks/event_log_backend.hpp>
#include <boost/log/sinks/event_log_constants.hpp>
#include <boost/log/utility/once_block.hpp>
#include <boost/log/detail/cleanup_scope_guard.hpp>
#include <boost/log/detail/attachable_sstream_buf.hpp>
#include <boost/log/detail/code_conversion.hpp>
#include <boost/log/utility/formatting_ostream.hpp>
#include <windows.h>
#include <psapi.h>
#include "unique_ptr.hpp"
#include "windows/event_log_registry.hpp"
#include "windows/simple_event_log.h"
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace sinks {

namespace event_log {

    //! The function constructs log record level from an integer
    BOOST_LOG_API event_type make_event_type(unsigned short lev)
    {
        switch (lev)
        {
        case success: return success;
        case warning: return warning;
        case error: return error;
        default:
            BOOST_THROW_EXCEPTION(std::out_of_range("Windows NT event type is out of range"));
            BOOST_LOG_UNREACHABLE_RETURN(info);
        case info: return info;
        }
    }

} // namespace event_log

BOOST_LOG_ANONYMOUS_NAMESPACE {

#ifdef BOOST_LOG_USE_CHAR
    //! A simple forwarder to the ReportEvent API
    inline BOOL report_event(
        HANDLE hEventLog,
        WORD wType,
        WORD wCategory,
        DWORD dwEventID,
        PSID lpUserSid,
        WORD wNumStrings,
        DWORD dwDataSize,
        const char** lpStrings,
        LPVOID lpRawData)
    {
        return ReportEventA(hEventLog, wType, wCategory, dwEventID, lpUserSid, wNumStrings, dwDataSize, lpStrings, lpRawData);
    }
    //! A simple forwarder to the GetModuleFileName API
    inline DWORD get_module_file_name(HMODULE hModule, char* lpFilename, DWORD nSize)
    {
        return GetModuleFileNameA(hModule, lpFilename, nSize);
    }
    //! A simple forwarder to the RegisterEventSource API
    inline HANDLE register_event_source(const char* lpUNCServerName, const char* lpSourceName)
    {
        return RegisterEventSourceA(lpUNCServerName, lpSourceName);
    }
    //! The function completes default source name for the sink backend
    inline void complete_default_simple_event_log_source_name(std::string& name)
    {
        name += " simple event source";
    }
    //! The function completes default source name for the sink backend
    inline void complete_default_event_log_source_name(std::string& name)
    {
        name += " event source";
    }
#endif // BOOST_LOG_USE_CHAR

#ifdef BOOST_LOG_USE_WCHAR_T
    //! A simple forwarder to the ReportEvent API
    inline BOOL report_event(
        HANDLE hEventLog,
        WORD wType,
        WORD wCategory,
        DWORD dwEventID,
        PSID lpUserSid,
        WORD wNumStrings,
        DWORD dwDataSize,
        const wchar_t** lpStrings,
        LPVOID lpRawData)
    {
        return ReportEventW(hEventLog, wType, wCategory, dwEventID, lpUserSid, wNumStrings, dwDataSize, lpStrings, lpRawData);
    }
    //! A simple forwarder to the GetModuleFileName API
    inline DWORD get_module_file_name(HMODULE hModule, wchar_t* lpFilename, DWORD nSize)
    {
        return GetModuleFileNameW(hModule, lpFilename, nSize);
    }
    //! A simple forwarder to the RegisterEventSource API
    inline HANDLE register_event_source(const wchar_t* lpUNCServerName, const wchar_t* lpSourceName)
    {
        return RegisterEventSourceW(lpUNCServerName, lpSourceName);
    }
    //! The function completes default source name for the sink backend
    inline void complete_default_simple_event_log_source_name(std::wstring& name)
    {
        name += L" simple event source";
    }
    //! The function completes default source name for the sink backend
    inline void complete_default_event_log_source_name(std::wstring& name)
    {
        name += L" event source";
    }
#endif // BOOST_LOG_USE_WCHAR_T

    //! The function finds the handle for the current module
    void init_self_module_handle(HMODULE& handle)
    {
        // Acquire all modules of the current process
        HANDLE hProcess = GetCurrentProcess();
        std::vector< HMODULE > modules;
        DWORD module_count = 1024;
        do
        {
            modules.resize(module_count, HMODULE(0));
            BOOL res = EnumProcessModules(
                hProcess,
                &modules[0],
                static_cast< DWORD >(modules.size() * sizeof(HMODULE)),
                &module_count);
            module_count /= sizeof(HMODULE);

            if (!res)
            {
                DWORD err = GetLastError();
                BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Could not enumerate process modules", (err));
            }
        }
        while (module_count > modules.size());
        modules.resize(module_count, HMODULE(0));

        // Now find the current module among them
        void* p = (void*)&init_self_module_handle;
        for (std::size_t i = 0, n = modules.size(); i < n; ++i)
        {
            MODULEINFO info;
            if (!GetModuleInformation(hProcess, modules[i], &info, sizeof(info)))
            {
                DWORD err = GetLastError();
                BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Could not acquire module information", (err));
            }

            if (info.lpBaseOfDll <= p && (static_cast< unsigned char* >(info.lpBaseOfDll) + info.SizeOfImage) > p)
            {
                // Found it
                handle = modules[i];
                break;
            }
        }

        if (!handle)
            BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Could not find self module information", (boost::system::windows_error::invalid_handle));
    }

    //! Retrieves the full name of the current module (be that dll or exe)
    template< typename CharT >
    std::basic_string< CharT > get_current_module_name()
    {
        static HMODULE hSelfModule = 0;

        BOOST_LOG_ONCE_BLOCK()
        {
            init_self_module_handle(hSelfModule);
        }

        // Get the module file name
        CharT buf[MAX_PATH];
        DWORD size = get_module_file_name(hSelfModule, buf, sizeof(buf) / sizeof(*buf));
        if (size == 0)
        {
            DWORD err = GetLastError();
            BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Could not get module file name", (err));
        }

        return std::basic_string< CharT >(buf, buf + size);
    }

} // namespace

//////////////////////////////////////////////////////////////////////////
//  Simple event log backend implementation
//////////////////////////////////////////////////////////////////////////
//! Sink backend implementation
template< typename CharT >
struct basic_simple_event_log_backend< CharT >::implementation
{
    //! A handle for the registered event provider
    HANDLE m_SourceHandle;
    //! A level mapping functor
    event_type_mapper_type m_LevelMapper;

    implementation() : m_SourceHandle(0)
    {
    }
};

//! Default constructor. Registers event source Boost.Log <Boost version> in the Application log.
template< typename CharT >
BOOST_LOG_API basic_simple_event_log_backend< CharT >::basic_simple_event_log_backend()
{
    construct(log::aux::empty_arg_list());
}

//! Destructor
template< typename CharT >
BOOST_LOG_API basic_simple_event_log_backend< CharT >::~basic_simple_event_log_backend()
{
    DeregisterEventSource(m_pImpl->m_SourceHandle);
    delete m_pImpl;
}

//! Constructs backend implementation
template< typename CharT >
BOOST_LOG_API void basic_simple_event_log_backend< CharT >::construct(
    string_type const& target, string_type const& log_name, string_type const& source_name, event_log::registration_mode reg_mode)
{
    if (reg_mode != event_log::never)
    {
        aux::registry_params< char_type > reg_params;
        reg_params.event_message_file = get_current_module_name< char_type >();
        reg_params.types_supported = DWORD(
            EVENTLOG_SUCCESS |
            EVENTLOG_INFORMATION_TYPE |
            EVENTLOG_WARNING_TYPE |
            EVENTLOG_ERROR_TYPE);
        aux::init_event_log_registry(log_name, source_name, reg_mode == event_log::forced, reg_params);
    }

    log::aux::unique_ptr< implementation > p(new implementation());

    const char_type* target_unc = NULL;
    if (!target.empty())
        target_unc = target.c_str();

    HANDLE hSource = register_event_source(target_unc, source_name.c_str());
    if (!hSource)
    {
        const DWORD err = GetLastError();
        BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Could not register event source", (err));
    }

    p->m_SourceHandle = hSource;

    m_pImpl = p.release();
}

//! Returns default log name
template< typename CharT >
BOOST_LOG_API typename basic_simple_event_log_backend< CharT >::string_type
basic_simple_event_log_backend< CharT >::get_default_log_name()
{
    return aux::registry_traits< char_type >::make_default_log_name();
}

//! Returns default source name
template< typename CharT >
BOOST_LOG_API typename basic_simple_event_log_backend< CharT >::string_type
basic_simple_event_log_backend< CharT >::get_default_source_name()
{
    string_type source_name = aux::registry_traits< char_type >::make_default_source_name();
    complete_default_simple_event_log_source_name(source_name);
    return source_name;
}

//! The method installs the function object that maps application severity levels to WinAPI event types
template< typename CharT >
BOOST_LOG_API void basic_simple_event_log_backend< CharT >::set_event_type_mapper(event_type_mapper_type const& mapper)
{
    m_pImpl->m_LevelMapper = mapper;
}

//! The method puts the formatted message to the event log
template< typename CharT >
BOOST_LOG_API void basic_simple_event_log_backend< CharT >::consume(record_view const& rec, string_type const& formatted_message)
{
    const char_type* message = formatted_message.c_str();
    event_log::event_type evt_type = event_log::info;
    if (!m_pImpl->m_LevelMapper.empty())
        evt_type = m_pImpl->m_LevelMapper(rec);

    DWORD event_id;
    switch (evt_type)
    {
    case event_log::success:
        event_id = BOOST_LOG_MSG_DEBUG; break;
    case event_log::warning:
        event_id = BOOST_LOG_MSG_WARNING; break;
    case event_log::error:
        event_id = BOOST_LOG_MSG_ERROR; break;
    default:
        event_id = BOOST_LOG_MSG_INFO; break;
    }

    report_event(
        m_pImpl->m_SourceHandle,        // Event log handle.
        static_cast< WORD >(evt_type),  // Event type.
        0,                              // Event category.
        event_id,                       // Event identifier.
        NULL,                           // No user security identifier.
        1,                              // Number of substitution strings.
        0,                              // No data.
        &message,                       // Pointer to strings.
        NULL);                          // No data.
}

//////////////////////////////////////////////////////////////////////////
//  Customizable event log backend implementation
//////////////////////////////////////////////////////////////////////////
namespace event_log {

    template< typename CharT >
    class basic_event_composer< CharT >::insertion_composer
    {
    public:
        //! Function object result type
        typedef void result_type;

    private:
        //! The list of insertion composers (in backward order)
        typedef std::vector< formatter_type > formatters;

    private:
        //! The insertion string composers
        formatters m_Formatters;

    public:
        //! Default constructor
        insertion_composer() {}
        //! Composition operator
        void operator() (record_view const& rec, insertion_list& insertions) const
        {
            std::size_t size = m_Formatters.size();
            insertions.resize(size);
            for (std::size_t i = 0; i < size; ++i)
            {
                typename formatter_type::stream_type strm(insertions[i]);
                m_Formatters[i](rec, strm);
                strm.flush();
            }
        }
        //! Adds a new formatter to the list
        void add_formatter(formatter_type const& fmt)
        {
            m_Formatters.push_back(formatter_type(fmt));
        }
    };

    //! Default constructor
    template< typename CharT >
    basic_event_composer< CharT >::basic_event_composer(event_id_mapper_type const& id_mapper) :
        m_EventIDMapper(id_mapper)
    {
    }
    //! Copy constructor
    template< typename CharT >
    basic_event_composer< CharT >::basic_event_composer(basic_event_composer const& that) :
        m_EventIDMapper(that.m_EventIDMapper),
        m_EventMap(that.m_EventMap)
    {
    }
    //! Destructor
    template< typename CharT >
    basic_event_composer< CharT >::~basic_event_composer()
    {
    }

    //! Assignment
    template< typename CharT >
    basic_event_composer< CharT >& basic_event_composer< CharT >::operator= (basic_event_composer that)
    {
        swap(that);
        return *this;
    }
    //! Swapping
    template< typename CharT >
    void basic_event_composer< CharT >::swap(basic_event_composer& that)
    {
        m_EventIDMapper.swap(that.m_EventIDMapper);
        m_EventMap.swap(that.m_EventMap);
    }
    //! Creates a new entry for a message
    template< typename CharT >
    typename basic_event_composer< CharT >::event_map_reference
    basic_event_composer< CharT >::operator[] (event_id id)
    {
        return event_map_reference(id, *this);
    }
    //! Creates a new entry for a message
    template< typename CharT >
    typename basic_event_composer< CharT >::event_map_reference
    basic_event_composer< CharT >::operator[] (int id)
    {
        return event_map_reference(make_event_id(id), *this);
    }

    //! Event composition operator
    template< typename CharT >
    event_id basic_event_composer< CharT >::operator() (record_view const& rec, insertion_list& insertions) const
    {
        event_id id = m_EventIDMapper(rec);
        typename event_map::const_iterator it = m_EventMap.find(id);
        if (it != m_EventMap.end())
            it->second(rec, insertions);
        return id;
    }

    //! Adds a formatter to the insertion composers list
    template< typename CharT >
    typename basic_event_composer< CharT >::insertion_composer*
    basic_event_composer< CharT >::add_formatter(event_id id, insertion_composer* composer, formatter_type const& fmt)
    {
        if (!composer)
            composer = &m_EventMap[id];
        composer->add_formatter(fmt);
        return composer;
    }

#ifdef BOOST_LOG_USE_CHAR
    template class BOOST_LOG_API basic_event_composer< char >;
#endif
#ifdef BOOST_LOG_USE_WCHAR_T
    template class BOOST_LOG_API basic_event_composer< wchar_t >;
#endif

} // namespace event_log


//! Backend implementation
template< typename CharT >
struct basic_event_log_backend< CharT >::implementation
{
    //  NOTE: This order of data members is critical for MSVC 9.0 in debug mode,
    //        as it ICEs if boost::functions are not the first members. Doh!

    //! An event category mapper
    event_category_mapper_type m_CategoryMapper;
    //! A level mapping functor
    event_type_mapper_type m_LevelMapper;

    //! A handle for the registered event provider
    HANDLE m_SourceHandle;
    //! A functor that composes an event
    event_composer_type m_EventComposer;
    //! An array of formatted insertions
    insertion_list m_Insertions;

    implementation() : m_SourceHandle(0)
    {
    }
};

//! Destructor
template< typename CharT >
BOOST_LOG_API basic_event_log_backend< CharT >::~basic_event_log_backend()
{
    DeregisterEventSource(m_pImpl->m_SourceHandle);
    delete m_pImpl;
}

//! Constructs backend implementation
template< typename CharT >
BOOST_LOG_API void basic_event_log_backend< CharT >::construct(
    filesystem::path const& message_file_name,
    string_type const& target,
    string_type const& log_name,
    string_type const& source_name,
    event_log::registration_mode reg_mode)
{
    if (reg_mode != event_log::never)
    {
        if (message_file_name.empty())
            BOOST_THROW_EXCEPTION(std::invalid_argument("Message file name not specified."));
        aux::registry_params< char_type > reg_params;
        string_type file_name;
        log::aux::code_convert(message_file_name.string(), file_name);
        reg_params.event_message_file = file_name;
        reg_params.types_supported = DWORD(
            EVENTLOG_SUCCESS |
            EVENTLOG_INFORMATION_TYPE |
            EVENTLOG_WARNING_TYPE |
            EVENTLOG_ERROR_TYPE);
        aux::init_event_log_registry(log_name, source_name, reg_mode == event_log::forced, reg_params);
    }

    log::aux::unique_ptr< implementation > p(new implementation());

    const char_type* target_unc = NULL;
    if (!target.empty())
        target_unc = target.c_str();

    HANDLE hSource = register_event_source(target_unc, source_name.c_str());
    if (!hSource)
    {
        const DWORD err = GetLastError();
        BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Could not register event source", (err));
    }

    p->m_SourceHandle = hSource;

    m_pImpl = p.release();
}

//! The method puts the formatted message to the event log
template< typename CharT >
BOOST_LOG_API void basic_event_log_backend< CharT >::consume(record_view const& rec)
{
    if (!m_pImpl->m_EventComposer.empty())
    {
        log::aux::cleanup_guard< insertion_list > cleaner(m_pImpl->m_Insertions);

        // Get event ID and construct insertions
        DWORD id = m_pImpl->m_EventComposer(rec, m_pImpl->m_Insertions);
        WORD string_count = static_cast< WORD >(m_pImpl->m_Insertions.size());
        scoped_array< const char_type* > strings(new const char_type*[string_count]);
        for (WORD i = 0; i < string_count; ++i)
            strings[i] = m_pImpl->m_Insertions[i].c_str();

        // Get event type
        WORD event_type = EVENTLOG_INFORMATION_TYPE;
        if (!m_pImpl->m_LevelMapper.empty())
            event_type = static_cast< WORD >(m_pImpl->m_LevelMapper(rec));

        WORD event_category = 0;
        if (!m_pImpl->m_CategoryMapper.empty())
            event_category = static_cast< WORD >(m_pImpl->m_CategoryMapper(rec));

        report_event(
            m_pImpl->m_SourceHandle,       // Event log handle.
            event_type,                    // Event type.
            event_category,                // Event category.
            id,                            // Event identifier.
            NULL,                          // No user security identifier.
            string_count,                  // Number of substitution strings.
            0,                             // No data.
            strings.get(),                 // Pointer to strings.
            NULL);                         // No data.
    }
}

//! Returns default log name
template< typename CharT >
BOOST_LOG_API typename basic_event_log_backend< CharT >::string_type
basic_event_log_backend< CharT >::get_default_log_name()
{
    return aux::registry_traits< char_type >::make_default_log_name();
}

//! Returns default source name
template< typename CharT >
BOOST_LOG_API typename basic_event_log_backend< CharT >::string_type
basic_event_log_backend< CharT >::get_default_source_name()
{
    string_type source_name = aux::registry_traits< char_type >::make_default_source_name();
    complete_default_event_log_source_name(source_name);
    return source_name;
}

//! The method installs the function object that maps application severity levels to WinAPI event types
template< typename CharT >
BOOST_LOG_API void basic_event_log_backend< CharT >::set_event_type_mapper(event_type_mapper_type const& mapper)
{
    m_pImpl->m_LevelMapper = mapper;
}

//! The method installs the function object that extracts event category from attribute values
template< typename CharT >
BOOST_LOG_API void basic_event_log_backend< CharT >::set_event_category_mapper(event_category_mapper_type const& mapper)
{
    m_pImpl->m_CategoryMapper = mapper;
}

/*!
 * The method installs the function object that extracts event identifier from the attributes and creates
 * insertion strings that will replace placeholders in the event message.
 */
template< typename CharT >
BOOST_LOG_API void basic_event_log_backend< CharT >::set_event_composer(event_composer_type const& composer)
{
    m_pImpl->m_EventComposer = composer;
}


#ifdef BOOST_LOG_USE_CHAR
template class basic_simple_event_log_backend< char >;
template class basic_event_log_backend< char >;
#endif
#ifdef BOOST_LOG_USE_WCHAR_T
template class basic_simple_event_log_backend< wchar_t >;
template class basic_event_log_backend< wchar_t >;
#endif

} // namespace sinks

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // !defined(BOOST_LOG_WITHOUT_EVENT_LOG)
