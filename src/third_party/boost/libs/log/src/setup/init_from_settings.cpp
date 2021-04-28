/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   init_from_settings.cpp
 * \author Andrey Semashev
 * \date   11.10.2009
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#ifndef BOOST_LOG_WITHOUT_SETTINGS_PARSERS

#if defined(__GNUC__) && !(defined(__INTEL_COMPILER) || defined(__ICL) || defined(__ICC) || defined(__ECC)) \
    && (__GNUC__ * 100 + __GNUC_MINOR__) >= 407
// This warning is caused by a compiler bug which is exposed when boost::optional is used: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=47679
// It has to be disabled here, before any code is included, since otherwise it doesn't help and the warning is still emitted.
// '*((void*)& foo +2)' may be used uninitialized in this function
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

#include <boost/log/detail/setup_config.hpp>
#include <cstddef>
#include <ios>
#include <map>
#include <vector>
#include <string>
#include <utility>
#include <iostream>
#include <typeinfo>
#include <stdexcept>
#include <algorithm>
#include <boost/type.hpp>
#include <boost/limits.hpp>
#include <boost/cstdint.hpp>
#include <boost/bind/bind.hpp>
#include <boost/smart_ptr/make_shared_object.hpp>
#include <boost/core/null_deleter.hpp>
#include <boost/optional/optional.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/date_time/date_defs.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/type_traits/conditional.hpp>
#include <boost/type_traits/is_unsigned.hpp>
#include <boost/type_traits/integral_constant.hpp>
#include <boost/spirit/home/qi/numeric/numeric_utils.hpp>
#include <boost/log/detail/code_conversion.hpp>
#include <boost/log/detail/singleton.hpp>
#include <boost/log/detail/default_attribute_names.hpp>
#include <boost/log/core.hpp>
#include <boost/log/sinks.hpp>
#include <boost/log/exceptions.hpp>
#include <boost/log/sinks/auto_newline_mode.hpp>
#include <boost/log/sinks/frontend_requirements.hpp>
#include <boost/log/expressions/filter.hpp>
#include <boost/log/expressions/formatter.hpp>
#include <boost/log/utility/string_literal.hpp>
#include <boost/log/utility/functional/nop.hpp>
#include <boost/log/utility/setup/from_settings.hpp>
#include <boost/log/utility/setup/filter_parser.hpp>
#include <boost/log/utility/setup/formatter_parser.hpp>
#if !defined(BOOST_LOG_NO_ASIO)
#include <boost/asio/ip/address.hpp>
#endif
#if !defined(BOOST_LOG_NO_THREADS)
#include <boost/log/detail/locks.hpp>
#include <boost/log/detail/light_rw_mutex.hpp>
#endif
#include "parser_utils.hpp"
#include "spirit_encoding.hpp"
#include <boost/log/detail/header.hpp>

namespace qi = boost::spirit::qi;

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

BOOST_LOG_ANONYMOUS_NAMESPACE {

//! Throws an exception when a parameter value is not valid
BOOST_LOG_NORETURN void throw_invalid_value(const char* param_name)
{
    std::string descr = std::string("Invalid parameter \"")
                        + param_name
                        + "\" value";
    BOOST_LOG_THROW_DESCR(invalid_value, descr);
}

//! Extracts an integral value from parameter value
template< typename IntT, typename CharT >
inline IntT param_cast_to_int(const char* param_name, std::basic_string< CharT > const& value)
{
    IntT res = 0;
    typedef typename conditional<
        is_unsigned< IntT >::value,
        qi::extract_uint< IntT, 10, 1, -1 >,
        qi::extract_int< IntT, 10, 1, -1 >
    >::type extract;
    const CharT* begin = value.c_str(), *end = begin + value.size();
    if (extract::call(begin, end, res) && begin == end)
        return res;
    else
        throw_invalid_value(param_name);
}

//! Case-insensitive character comparison predicate
struct is_case_insensitive_equal
{
    typedef bool result_type;

    template< typename CharT >
    result_type operator() (CharT left, CharT right) const BOOST_NOEXCEPT
    {
        typedef typename boost::log::aux::encoding< CharT >::type encoding;
        return encoding::tolower(left) == encoding::tolower(right);
    }
};

//! Extracts a boolean value from parameter value
template< typename CharT >
inline bool param_cast_to_bool(const char* param_name, std::basic_string< CharT > const& value)
{
    typedef CharT char_type;
    typedef boost::log::aux::char_constants< char_type > constants;
    typedef boost::log::basic_string_literal< char_type > literal_type;

    const char_type* begin = value.c_str(), *end = begin + value.size();
    std::size_t len = end - begin;

    literal_type keyword = constants::true_keyword();
    if (keyword.size() == len && std::equal(begin, end, keyword.c_str(), is_case_insensitive_equal()))
    {
        return true;
    }
    else
    {
        keyword = constants::false_keyword();
        if (keyword.size() == len && std::equal(begin, end, keyword.c_str(), is_case_insensitive_equal()))
        {
            return false;
        }
        else
        {
            return param_cast_to_int< unsigned int >(param_name, value) != 0;
        }
    }
}

//! Extracts an \c auto_newline_mode value from parameter value
template< typename CharT >
inline sinks::auto_newline_mode param_cast_to_auto_newline_mode(const char* param_name, std::basic_string< CharT > const& value)
{
    typedef CharT char_type;
    typedef boost::log::aux::char_constants< char_type > constants;

    if (value == constants::auto_newline_mode_disabled())
        return sinks::disabled_auto_newline;
    else if (value == constants::auto_newline_mode_always_insert())
        return sinks::always_insert;
    else if (value == constants::auto_newline_mode_insert_if_missing())
        return sinks::insert_if_missing;
    else
    {
        BOOST_LOG_THROW_DESCR(invalid_value,
            "Auto newline mode \"" + boost::log::aux::to_narrow(value) + "\" is not supported");
    }
}

#if !defined(BOOST_LOG_NO_ASIO)
//! Extracts a network address from parameter value
template< typename CharT >
inline std::string param_cast_to_address(const char* param_name, std::basic_string< CharT > const& value)
{
    return log::aux::to_narrow(value);
}
#endif // !defined(BOOST_LOG_NO_ASIO)

template< typename CharT >
inline bool is_weekday(const CharT* str, std::size_t len, boost::log::basic_string_literal< CharT > const& weekday, boost::log::basic_string_literal< CharT > const& short_weekday)
{
    return (len == weekday.size() && std::equal(weekday.begin(), weekday.end(), str)) ||
        (len == short_weekday.size() && std::equal(short_weekday.begin(), short_weekday.end(), str));
}

//! The function extracts the file rotation time point predicate from the parameter
template< typename CharT >
sinks::file::rotation_at_time_point param_cast_to_rotation_time_point(const char* param_name, std::basic_string< CharT > const& value)
{
    typedef CharT char_type;
    typedef boost::log::aux::char_constants< char_type > constants;
    typedef typename boost::log::aux::encoding< char_type >::type encoding;
    typedef qi::extract_uint< unsigned short, 10, 1, 2 > day_extract;
    typedef qi::extract_uint< unsigned char, 10, 2, 2 > time_component_extract;

    const char_type colon = static_cast< char_type >(':');
    optional< date_time::weekdays > weekday;
    optional< unsigned short > day;
    unsigned char hour = 0, minute = 0, second = 0;
    const char_type* begin = value.c_str(), *end = begin + value.size();

    if (!encoding::isalnum(*begin)) // begin is null-terminated, so we also check that the string is not empty here
        throw_invalid_value(param_name);

    const char_type* p = begin + 1;
    if (encoding::isalpha(*begin))
    {
        // This must be a weekday
        while (encoding::isalpha(*p))
            ++p;

        std::size_t len = p - begin;
        if (is_weekday(begin, len, constants::monday_keyword(), constants::short_monday_keyword()))
            weekday = date_time::Monday;
        else if (is_weekday(begin, len, constants::tuesday_keyword(), constants::short_tuesday_keyword()))
            weekday = date_time::Tuesday;
        else if (is_weekday(begin, len, constants::wednesday_keyword(), constants::short_wednesday_keyword()))
            weekday = date_time::Wednesday;
        else if (is_weekday(begin, len, constants::thursday_keyword(), constants::short_thursday_keyword()))
            weekday = date_time::Thursday;
        else if (is_weekday(begin, len, constants::friday_keyword(), constants::short_friday_keyword()))
            weekday = date_time::Friday;
        else if (is_weekday(begin, len, constants::saturday_keyword(), constants::short_saturday_keyword()))
            weekday = date_time::Saturday;
        else if (is_weekday(begin, len, constants::sunday_keyword(), constants::short_sunday_keyword()))
            weekday = date_time::Sunday;
        else
            throw_invalid_value(param_name);
    }
    else
    {
        // This may be either a month day or an hour
        while (encoding::isdigit(*p))
            ++p;

        if (encoding::isspace(*p))
        {
            // This is a month day
            unsigned short mday = 0;
            const char_type* b = begin;
            if (!day_extract::call(b, p, mday) || b != p)
                throw_invalid_value(param_name);

            day = mday;
        }
        else if (*p == colon)
        {
            // This is an hour, reset the pointer
            p = begin;
        }
        else
            throw_invalid_value(param_name);
    }

    // Skip spaces
    while (encoding::isspace(*p))
        ++p;

    // Parse hour
    if (!time_component_extract::call(p, end, hour) || *p != colon)
        throw_invalid_value(param_name);
    ++p;

    // Parse minute
    if (!time_component_extract::call(p, end, minute) || *p != colon)
        throw_invalid_value(param_name);
    ++p;

    // Parse second
    if (!time_component_extract::call(p, end, second) || p != end)
        throw_invalid_value(param_name);

    // Construct the predicate
    if (weekday)
        return sinks::file::rotation_at_time_point(weekday.get(), hour, minute, second);
    else if (day)
        return sinks::file::rotation_at_time_point(gregorian::greg_day(day.get()), hour, minute, second);
    else
        return sinks::file::rotation_at_time_point(hour, minute, second);
}

//! Base class for default sink factories
template< typename CharT >
class basic_default_sink_factory :
    public sink_factory< CharT >
{
public:
    typedef sink_factory< CharT > base_type;
    typedef typename base_type::char_type char_type;
    typedef typename base_type::string_type string_type;
    typedef typename base_type::settings_section settings_section;
    typedef boost::log::aux::char_constants< char_type > constants;

protected:
    //! Sink backend character selection function
    template< typename InitializerT >
    static shared_ptr< sinks::sink > select_backend_character_type(settings_section const& params, InitializerT initializer)
    {
#if defined(BOOST_LOG_USE_CHAR) && defined(BOOST_LOG_USE_WCHAR_T)
        if (optional< string_type > wide_param = params["Wide"])
        {
            if (param_cast_to_bool("Wide", wide_param.get()))
                return initializer(params, type< wchar_t >());
        }

        return initializer(params, type< char >());
#elif defined(BOOST_LOG_USE_CHAR)
        return initializer(params, type< char >());
#elif defined(BOOST_LOG_USE_WCHAR_T)
        return initializer(params, type< wchar_t >());
#endif
    }

    //! The function initializes common parameters of a formatting sink and returns the constructed sink
    template< typename BackendT >
    static shared_ptr< sinks::sink > init_sink(shared_ptr< BackendT > const& backend, settings_section const& params)
    {
        typedef BackendT backend_t;
        typedef typename sinks::has_requirement<
            typename backend_t::frontend_requirements,
            sinks::formatted_records
        >::type is_formatting_t;

        // Filter
        filter filt;
        if (optional< string_type > filter_param = params["Filter"])
        {
            filt = parse_filter(filter_param.get());
        }

        shared_ptr< sinks::basic_sink_frontend > p;

#if !defined(BOOST_LOG_NO_THREADS)
        // Asynchronous. TODO: make it more flexible.
        bool async = false;
        if (optional< string_type > async_param = params["Asynchronous"])
        {
            async = param_cast_to_bool("Asynchronous", async_param.get());
        }

        // Construct the frontend, considering Asynchronous parameter
        if (!async)
        {
            p = init_formatter(boost::make_shared< sinks::synchronous_sink< backend_t > >(backend), params, is_formatting_t());
        }
        else
        {
            p = init_formatter(boost::make_shared< sinks::asynchronous_sink< backend_t > >(backend), params, is_formatting_t());

            // https://svn.boost.org/trac/boost/ticket/10638
            // The user doesn't have a way to process excaptions from the dedicated thread anyway, so just suppress them instead of
            // terminating the application.
            p->set_exception_handler(nop());
        }
#else
        // When multithreading is disabled we always use the unlocked sink frontend
        p = init_formatter(boost::make_shared< sinks::unlocked_sink< backend_t > >(backend), params, is_formatting_t());
#endif

        p->set_filter(filt);

        return p;
    }

private:
    //! The function initializes formatter for the sinks that support formatting
    template< typename SinkT >
    static shared_ptr< SinkT > init_formatter(shared_ptr< SinkT > const& sink, settings_section const& params, true_type)
    {
        // Formatter
        if (optional< string_type > format_param = params["Format"])
        {
            typedef typename SinkT::char_type sink_char_type;
            std::basic_string< sink_char_type > format_str;
            log::aux::code_convert(format_param.get(), format_str);
            sink->set_formatter(parse_formatter(format_str));
        }
        return sink;
    }
    template< typename SinkT >
    static shared_ptr< SinkT > init_formatter(shared_ptr< SinkT > const& sink, settings_section const& params, false_type)
    {
        return sink;
    }
};

//! Default console sink factory
template< typename CharT >
class default_console_sink_factory :
    public basic_default_sink_factory< CharT >
{
public:
    typedef basic_default_sink_factory< CharT > base_type;
    typedef typename base_type::char_type char_type;
    typedef typename base_type::string_type string_type;
    typedef typename base_type::settings_section settings_section;
    typedef typename base_type::constants constants;

private:
    struct impl;
    friend struct impl;
    struct impl
    {
        typedef shared_ptr< sinks::sink > result_type;

        template< typename BackendCharT >
        result_type operator() (settings_section const& params, type< BackendCharT >) const
        {
            // Construct the backend
            typedef boost::log::aux::char_constants< BackendCharT > constants;
            typedef sinks::basic_text_ostream_backend< BackendCharT > backend_t;
            shared_ptr< backend_t > backend = boost::make_shared< backend_t >();
            backend->add_stream(shared_ptr< typename backend_t::stream_type >(&constants::get_console_log_stream(), boost::null_deleter()));

            // Auto newline mode
            if (optional< string_type > auto_newline_param = params["AutoNewline"])
            {
                backend->set_auto_newline_mode(param_cast_to_auto_newline_mode("AutoNewline", auto_newline_param.get()));
            }

            // Auto flush
            if (optional< string_type > auto_flush_param = params["AutoFlush"])
            {
                backend->auto_flush(param_cast_to_bool("AutoFlush", auto_flush_param.get()));
            }

            return base_type::init_sink(backend, params);
        }
    };

public:
    //! The function constructs a sink that writes log records to the console
    shared_ptr< sinks::sink > create_sink(settings_section const& params) BOOST_OVERRIDE
    {
        return base_type::select_backend_character_type(params, impl());
    }
};

//! Default text file sink factory
template< typename CharT >
class default_text_file_sink_factory :
    public basic_default_sink_factory< CharT >
{
public:
    typedef basic_default_sink_factory< CharT > base_type;
    typedef typename base_type::char_type char_type;
    typedef typename base_type::string_type string_type;
    typedef typename base_type::settings_section settings_section;
    typedef typename base_type::constants constants;

public:
    //! The function constructs a sink that writes log records to a text file
    shared_ptr< sinks::sink > create_sink(settings_section const& params) BOOST_OVERRIDE
    {
        typedef sinks::text_file_backend backend_t;
        shared_ptr< backend_t > backend = boost::make_shared< backend_t >();

        // FileName
        if (optional< string_type > file_name_param = params["FileName"])
        {
            backend->set_file_name_pattern(filesystem::path(file_name_param.get()));
        }
        else
            BOOST_LOG_THROW_DESCR(missing_value, "File name is not specified");

        // Target file name
        if (optional< string_type > target_file_name_param = params["TargetFileName"])
        {
            backend->set_target_file_name_pattern(filesystem::path(target_file_name_param.get()));
        }

        // File rotation size
        if (optional< string_type > rotation_size_param = params["RotationSize"])
        {
            backend->set_rotation_size(param_cast_to_int< uintmax_t >("RotationSize", rotation_size_param.get()));
        }

        // File rotation interval
        if (optional< string_type > rotation_interval_param = params["RotationInterval"])
        {
            backend->set_time_based_rotation(sinks::file::rotation_at_time_interval(
                posix_time::seconds(param_cast_to_int< unsigned int >("RotationInterval", rotation_interval_param.get()))));
        }
        else if (optional< string_type > rotation_time_point_param = params["RotationTimePoint"])
        {
            // File rotation time point
            backend->set_time_based_rotation(param_cast_to_rotation_time_point("RotationTimePoint", rotation_time_point_param.get()));
        }

        // Final rotation
        if (optional< string_type > enable_final_rotation_param = params["EnableFinalRotation"])
        {
            backend->enable_final_rotation(param_cast_to_bool("EnableFinalRotation", enable_final_rotation_param.get()));
        }

        // Auto newline mode
        if (optional< string_type > auto_newline_param = params["AutoNewline"])
        {
            backend->set_auto_newline_mode(param_cast_to_auto_newline_mode("AutoNewline", auto_newline_param.get()));
        }

        // Auto flush
        if (optional< string_type > auto_flush_param = params["AutoFlush"])
        {
            backend->auto_flush(param_cast_to_bool("AutoFlush", auto_flush_param.get()));
        }

        // Append
        if (optional< string_type > append_param = params["Append"])
        {
            if (param_cast_to_bool("Append", append_param.get()))
                backend->set_open_mode(std::ios_base::out | std::ios_base::app);
        }

        // File collector parameters
        // Target directory
        if (optional< string_type > target_param = params["Target"])
        {
            filesystem::path target_dir(target_param.get());

            // Max total size
            uintmax_t max_size = (std::numeric_limits< uintmax_t >::max)();
            if (optional< string_type > max_size_param = params["MaxSize"])
                max_size = param_cast_to_int< uintmax_t >("MaxSize", max_size_param.get());

            // Min free space
            uintmax_t space = 0;
            if (optional< string_type > min_space_param = params["MinFreeSpace"])
                space = param_cast_to_int< uintmax_t >("MinFreeSpace", min_space_param.get());

            // Max number of files
            uintmax_t max_files = (std::numeric_limits< uintmax_t >::max)();
            if (optional< string_type > max_files_param = params["MaxFiles"])
                max_files = param_cast_to_int< uintmax_t >("MaxFiles", max_files_param.get());

            backend->set_file_collector(sinks::file::make_collector(
                keywords::target = target_dir,
                keywords::max_size = max_size,
                keywords::min_free_space = space,
                keywords::max_files = max_files));

            // Scan for log files
            if (optional< string_type > scan_param = params["ScanForFiles"])
            {
                string_type const& value = scan_param.get();
                if (value == constants::scan_method_all())
                    backend->scan_for_files(sinks::file::scan_all);
                else if (value == constants::scan_method_matching())
                    backend->scan_for_files(sinks::file::scan_matching);
                else
                {
                    BOOST_LOG_THROW_DESCR(invalid_value,
                        "File scan method \"" + boost::log::aux::to_narrow(value) + "\" is not supported");
                }
            }
        }

        return base_type::init_sink(backend, params);
    }
};

#ifndef BOOST_LOG_WITHOUT_SYSLOG

//! Default syslog sink factory
template< typename CharT >
class default_syslog_sink_factory :
    public basic_default_sink_factory< CharT >
{
public:
    typedef basic_default_sink_factory< CharT > base_type;
    typedef typename base_type::char_type char_type;
    typedef typename base_type::string_type string_type;
    typedef typename base_type::settings_section settings_section;
    typedef typename base_type::constants constants;

public:
    //! The function constructs a sink that writes log records to syslog
    shared_ptr< sinks::sink > create_sink(settings_section const& params) BOOST_OVERRIDE
    {
        // Construct the backend
        typedef sinks::syslog_backend backend_t;
        shared_ptr< backend_t > backend = boost::make_shared< backend_t >();

        // For now we use only the default level mapping. Will add support for configuration later.
        backend->set_severity_mapper(sinks::syslog::direct_severity_mapping< >(log::aux::default_attribute_names::severity()));

#if !defined(BOOST_LOG_NO_ASIO)
        // Setup local and remote addresses
        if (optional< string_type > local_address_param = params["LocalAddress"])
            backend->set_local_address(param_cast_to_address("LocalAddress", local_address_param.get()));

        if (optional< string_type > target_address_param = params["TargetAddress"])
            backend->set_target_address(param_cast_to_address("TargetAddress", target_address_param.get()));
#endif // !defined(BOOST_LOG_NO_ASIO)

        return base_type::init_sink(backend, params);
    }
};

#endif // !defined(BOOST_LOG_WITHOUT_SYSLOG)

#ifndef BOOST_LOG_WITHOUT_DEBUG_OUTPUT

//! Default debugger sink factory
template< typename CharT >
class default_debugger_sink_factory :
    public basic_default_sink_factory< CharT >
{
public:
    typedef basic_default_sink_factory< CharT > base_type;
    typedef typename base_type::char_type char_type;
    typedef typename base_type::string_type string_type;
    typedef typename base_type::settings_section settings_section;
    typedef typename base_type::constants constants;

private:
    struct impl;
    friend struct impl;
    struct impl
    {
        typedef shared_ptr< sinks::sink > result_type;

        template< typename BackendCharT >
        result_type operator() (settings_section const& params, type< BackendCharT >) const
        {
            // Construct the backend
            typedef sinks::basic_debug_output_backend< BackendCharT > backend_t;
            shared_ptr< backend_t > backend = boost::make_shared< backend_t >();

            return base_type::init_sink(backend, params);
        }
    };

public:
    //! The function constructs a sink that writes log records to the debugger
    shared_ptr< sinks::sink > create_sink(settings_section const& params)
    {
        return base_type::select_backend_character_type(params, impl());
    }
};

#endif // !defined(BOOST_LOG_WITHOUT_DEBUG_OUTPUT)

#ifndef BOOST_LOG_WITHOUT_EVENT_LOG

//! Default simple event log sink factory
template< typename CharT >
class default_simple_event_log_sink_factory :
    public basic_default_sink_factory< CharT >
{
public:
    typedef basic_default_sink_factory< CharT > base_type;
    typedef typename base_type::char_type char_type;
    typedef typename base_type::string_type string_type;
    typedef typename base_type::settings_section settings_section;
    typedef typename base_type::constants constants;

private:
    struct impl;
    friend struct impl;
    struct impl
    {
        typedef shared_ptr< sinks::sink > result_type;

        template< typename BackendCharT >
        result_type operator() (settings_section const& params, type< BackendCharT >) const
        {
            typedef sinks::basic_simple_event_log_backend< BackendCharT > backend_t;
            typedef typename backend_t::string_type backend_string_type;

            // Determine the log name
            backend_string_type log_name;
            if (optional< string_type > log_name_param = params["LogName"])
                log::aux::code_convert(log_name_param.get(), log_name);
            else
                log_name = backend_t::get_default_log_name();

            // Determine the log source name
            backend_string_type source_name;
            if (optional< string_type > log_source_param = params["LogSource"])
                log::aux::code_convert(log_source_param.get(), source_name);
            else
                source_name = backend_t::get_default_source_name();

            // Determine the registration mode
            sinks::event_log::registration_mode reg_mode = sinks::event_log::on_demand;
            if (optional< string_type > registration_param = params["Registration"])
            {
                string_type const& value = registration_param.get();
                if (value == constants::registration_never())
                    reg_mode = sinks::event_log::never;
                else if (value == constants::registration_on_demand())
                    reg_mode = sinks::event_log::on_demand;
                else if (value == constants::registration_forced())
                    reg_mode = sinks::event_log::forced;
                else
                {
                    BOOST_LOG_THROW_DESCR(invalid_value,
                        "The registration mode \"" + log::aux::to_narrow(value) + "\" is not supported");
                }
            }

            // Construct the backend
            shared_ptr< backend_t > backend(boost::make_shared< backend_t >((
                keywords::log_name = log_name,
                keywords::log_source = source_name,
                keywords::registration = reg_mode)));

            // For now we use only the default event type mapping. Will add support for configuration later.
            backend->set_event_type_mapper(sinks::event_log::direct_event_type_mapping< >(log::aux::default_attribute_names::severity()));

            return base_type::init_sink(backend, params);
        }
    };

public:
    //! The function constructs a sink that writes log records to the Windows NT Event Log
    shared_ptr< sinks::sink > create_sink(settings_section const& params)
    {
        return base_type::select_backend_character_type(params, impl());
    }
};

#endif // !defined(BOOST_LOG_WITHOUT_EVENT_LOG)


//! The supported sinks repository
template< typename CharT >
struct sinks_repository :
    public log::aux::lazy_singleton< sinks_repository< CharT > >
{
    typedef log::aux::lazy_singleton< sinks_repository< CharT > > base_type;

#if !defined(BOOST_LOG_BROKEN_FRIEND_TEMPLATE_SPECIALIZATIONS)
    friend class log::aux::lazy_singleton< sinks_repository< CharT > >;
#else
    friend class base_type;
#endif

    typedef CharT char_type;
    typedef std::basic_string< char_type > string_type;
    typedef basic_settings_section< char_type > settings_section;
    typedef boost::log::aux::char_constants< char_type > constants;
    typedef boost::shared_ptr< sink_factory< char_type > > sink_factory_ptr;
    typedef std::map< std::string, sink_factory_ptr > sink_factories;

#if !defined(BOOST_LOG_NO_THREADS)
    //! Synchronization mutex
    log::aux::light_rw_mutex m_Mutex;
#endif
    //! Map of the sink factories
    sink_factories m_Factories;

    //! The function constructs a sink from the settings
    shared_ptr< sinks::sink > construct_sink_from_settings(settings_section const& params)
    {
        typedef typename settings_section::const_reference param_const_reference;
        if (param_const_reference dest_node = params["Destination"])
        {
            std::string dest = log::aux::to_narrow(dest_node.get().get());

            BOOST_LOG_EXPR_IF_MT(log::aux::shared_lock_guard< log::aux::light_rw_mutex > lock(m_Mutex);)
            typename sink_factories::const_iterator it = m_Factories.find(dest);
            if (it != m_Factories.end())
            {
                return it->second->create_sink(params);
            }
            else
            {
                BOOST_LOG_THROW_DESCR(invalid_value, "The sink destination is not supported: " + dest);
            }
        }
        else
        {
            BOOST_LOG_THROW_DESCR(missing_value, "The sink destination is not set");
        }
    }

    static void init_instance()
    {
        sinks_repository& instance = base_type::get_instance();
        instance.m_Factories["TextFile"] = boost::make_shared< default_text_file_sink_factory< char_type > >();
        instance.m_Factories["Console"] = boost::make_shared< default_console_sink_factory< char_type > >();
#ifndef BOOST_LOG_WITHOUT_SYSLOG
        instance.m_Factories["Syslog"] = boost::make_shared< default_syslog_sink_factory< char_type > >();
#endif
#ifndef BOOST_LOG_WITHOUT_DEBUG_OUTPUT
        instance.m_Factories["Debugger"] = boost::make_shared< default_debugger_sink_factory< char_type > >();
#endif
#ifndef BOOST_LOG_WITHOUT_EVENT_LOG
        instance.m_Factories["SimpleEventLog"] = boost::make_shared< default_simple_event_log_sink_factory< char_type > >();
#endif
    }

private:
    sinks_repository() {}
};

//! The function applies the settings to the logging core
template< typename CharT >
void apply_core_settings(basic_settings_section< CharT > const& params)
{
    typedef CharT char_type;
    typedef std::basic_string< char_type > string_type;

    core_ptr core = boost::log::core::get();

    // Filter
    if (optional< string_type > filter_param = params["Filter"])
        core->set_filter(parse_filter(filter_param.get()));
    else
        core->reset_filter();

    // DisableLogging
    if (optional< string_type > disable_logging_param = params["DisableLogging"])
        core->set_logging_enabled(!param_cast_to_bool("DisableLogging", disable_logging_param.get()));
    else
        core->set_logging_enabled(true);
}

} // namespace


//! The function initializes the logging library from a settings container
template< typename CharT >
BOOST_LOG_SETUP_API void init_from_settings(basic_settings_section< CharT > const& setts)
{
    typedef basic_settings_section< CharT > section;
    typedef typename section::char_type char_type;
    typedef sinks_repository< char_type > sinks_repo_t;

    // Apply core settings
    if (section core_params = setts["Core"])
        apply_core_settings(core_params);

    // Construct and initialize sinks
    if (section sink_params = setts["Sinks"])
    {
        sinks_repo_t& sinks_repo = sinks_repo_t::get();
        std::vector< shared_ptr< sinks::sink > > new_sinks;

        for (typename section::const_iterator it = sink_params.begin(), end = sink_params.end(); it != end; ++it)
        {
            section sink_params = *it;

            // Ignore empty sections as they are most likely individual parameters (which should not be here anyway)
            if (!sink_params.empty())
            {
                new_sinks.push_back(sinks_repo.construct_sink_from_settings(sink_params));
            }
        }

        std::for_each(new_sinks.begin(), new_sinks.end(), boost::bind(&core::add_sink, core::get(), boost::placeholders::_1));
    }
}


//! The function registers a factory for a sink
template< typename CharT >
BOOST_LOG_SETUP_API void register_sink_factory(const char* sink_name, shared_ptr< sink_factory< CharT > > const& factory)
{
    sinks_repository< CharT >& repo = sinks_repository< CharT >::get();
    BOOST_LOG_EXPR_IF_MT(lock_guard< log::aux::light_rw_mutex > lock(repo.m_Mutex);)
    repo.m_Factories[sink_name] = factory;
}

#ifdef BOOST_LOG_USE_CHAR
template BOOST_LOG_SETUP_API void register_sink_factory< char >(const char* sink_name, shared_ptr< sink_factory< char > > const& factory);
template BOOST_LOG_SETUP_API void init_from_settings< char >(basic_settings_section< char > const& setts);
#endif

#ifdef BOOST_LOG_USE_WCHAR_T
template BOOST_LOG_SETUP_API void register_sink_factory< wchar_t >(const char* sink_name, shared_ptr< sink_factory< wchar_t > > const& factory);
template BOOST_LOG_SETUP_API void init_from_settings< wchar_t >(basic_settings_section< wchar_t > const& setts);
#endif

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // BOOST_LOG_WITHOUT_SETTINGS_PARSERS
