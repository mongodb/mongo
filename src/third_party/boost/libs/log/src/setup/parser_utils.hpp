/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   parser_utils.hpp
 * \author Andrey Semashev
 * \date   31.03.2008
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#ifndef BOOST_LOG_PARSER_UTILS_HPP_INCLUDED_
#define BOOST_LOG_PARSER_UTILS_HPP_INCLUDED_

#include <boost/log/detail/config.hpp>
#include <string>
#include <iostream>
#include <cctype>
#include <boost/log/utility/string_literal.hpp>
#include <boost/log/detail/header.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

//! Some constants and algorithms needed for parsing
template< typename > struct char_constants;

#ifdef BOOST_LOG_USE_CHAR
template< >
struct char_constants< char >
{
    typedef char char_type;
    typedef std::basic_string< char_type > string_type;
    typedef boost::log::basic_string_literal< char_type > literal_type;

    static const char_type char_comment = '#';
    static const char_type char_comma = ',';
    static const char_type char_dot = '.';
    static const char_type char_quote = '"';
    static const char_type char_percent = '%';
    static const char_type char_exclamation = '!';
    static const char_type char_and = '&';
    static const char_type char_or = '|';
    static const char_type char_equal = '=';
    static const char_type char_greater = '>';
    static const char_type char_less = '<';
    static const char_type char_underline = '_';
    static const char_type char_backslash = '\\';
    static const char_type char_section_bracket_left = '[';
    static const char_type char_section_bracket_right = ']';
    static const char_type char_paren_bracket_left = '(';
    static const char_type char_paren_bracket_right = ')';

    static const char_type* not_keyword() { return "not"; }
    static const char_type* and_keyword() { return "and"; }
    static const char_type* or_keyword() { return "or"; }
    static const char_type* equal_keyword() { return "="; }
    static const char_type* greater_keyword() { return ">"; }
    static const char_type* less_keyword() { return "<"; }
    static const char_type* not_equal_keyword() { return "!="; }
    static const char_type* greater_or_equal_keyword() { return ">="; }
    static const char_type* less_or_equal_keyword() { return "<="; }
    static const char_type* begins_with_keyword() { return "begins_with"; }
    static const char_type* ends_with_keyword() { return "ends_with"; }
    static const char_type* contains_keyword() { return "contains"; }
    static const char_type* matches_keyword() { return "matches"; }

    static const char_type* message_text_keyword() { return "_"; }

    static literal_type true_keyword() { return literal_type("true"); }
    static literal_type false_keyword() { return literal_type("false"); }

    static const char_type* default_level_attribute_name() { return "Severity"; }

    static const char_type* core_section_name() { return "Core"; }
    static const char_type* sink_section_name_prefix() { return "Sink:"; }

    static const char_type* core_disable_logging_param_name() { return "DisableLogging"; }
    static const char_type* filter_param_name() { return "Filter"; }

    static const char_type* sink_destination_param_name() { return "Destination"; }
    static const char_type* file_name_param_name() { return "FileName"; }
    static const char_type* rotation_size_param_name() { return "RotationSize"; }
    static const char_type* rotation_interval_param_name() { return "RotationInterval"; }
    static const char_type* rotation_time_point_param_name() { return "RotationTimePoint"; }
    static const char_type* append_param_name() { return "Append"; }
    static const char_type* enable_final_rotation_param_name() { return "EnableFinalRotation"; }
    static const char_type* auto_flush_param_name() { return "AutoFlush"; }
    static const char_type* auto_newline_mode_param_name() { return "AutoNewline"; }
    static const char_type* asynchronous_param_name() { return "Asynchronous"; }
    static const char_type* format_param_name() { return "Format"; }
    static const char_type* provider_id_param_name() { return "ProviderID"; }
    static const char_type* log_name_param_name() { return "LogName"; }
    static const char_type* source_name_param_name() { return "LogSource"; }
    static const char_type* registration_param_name() { return "Registration"; }
    static const char_type* local_address_param_name() { return "LocalAddress"; }
    static const char_type* target_address_param_name() { return "TargetAddress"; }
    static const char_type* target_param_name() { return "Target"; }
    static const char_type* max_size_param_name() { return "MaxSize"; }
    static const char_type* max_files_param_name() { return "MaxFiles"; }
    static const char_type* min_free_space_param_name() { return "MinFreeSpace"; }
    static const char_type* scan_for_files_param_name() { return "ScanForFiles"; }

    static const char_type* scan_method_all() { return "All"; }
    static const char_type* scan_method_matching() { return "Matching"; }

    static const char_type* auto_newline_mode_disabled() { return "Disabled"; }
    static const char_type* auto_newline_mode_always_insert() { return "AlwaysInsert"; }
    static const char_type* auto_newline_mode_insert_if_missing() { return "InsertIfMissing"; }

    static const char_type* registration_never() { return "Never"; }
    static const char_type* registration_on_demand() { return "OnDemand"; }
    static const char_type* registration_forced() { return "Forced"; }

    static const char_type* text_file_destination() { return "TextFile"; }
    static const char_type* console_destination() { return "Console"; }
    static const char_type* syslog_destination() { return "Syslog"; }
    static const char_type* simple_event_log_destination() { return "SimpleEventLog"; }
    static const char_type* debugger_destination() { return "Debugger"; }

    static literal_type monday_keyword() { return literal_type("Monday"); }
    static literal_type short_monday_keyword() { return literal_type("Mon"); }
    static literal_type tuesday_keyword() { return literal_type("Tuesday"); }
    static literal_type short_tuesday_keyword() { return literal_type("Tue"); }
    static literal_type wednesday_keyword() { return literal_type("Wednesday"); }
    static literal_type short_wednesday_keyword() { return literal_type("Wed"); }
    static literal_type thursday_keyword() { return literal_type("Thursday"); }
    static literal_type short_thursday_keyword() { return literal_type("Thu"); }
    static literal_type friday_keyword() { return literal_type("Friday"); }
    static literal_type short_friday_keyword() { return literal_type("Fri"); }
    static literal_type saturday_keyword() { return literal_type("Saturday"); }
    static literal_type short_saturday_keyword() { return literal_type("Sat"); }
    static literal_type sunday_keyword() { return literal_type("Sunday"); }
    static literal_type short_sunday_keyword() { return literal_type("Sun"); }

    static std::ostream& get_console_log_stream() { return std::clog; }

    static int to_number(char_type c)
    {
        using namespace std; // to make sure we can use C functions unqualified
        int n = 0;
        if (isdigit(c))
            n = c - '0';
        else if (c >= 'a' && c <= 'f')
            n = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            n = c - 'A' + 10;
        return n;
    }

    //! Skips spaces in the beginning of the input
    static const char_type* trim_spaces_left(const char_type* begin, const char_type* end);
    //! Skips spaces in the end of the input
    static const char_type* trim_spaces_right(const char_type* begin, const char_type* end);
    //! Scans for the attribute name placeholder in the input
    static const char_type* scan_attr_placeholder(const char_type* begin, const char_type* end);
    //! Parses an operand string (possibly quoted) from the input
    static const char_type* parse_operand(const char_type* begin, const char_type* end, string_type& operand);
    //! Converts escape sequences to the corresponding characters
    static void translate_escape_sequences(string_type& str);
};
#endif

#ifdef BOOST_LOG_USE_WCHAR_T
template< >
struct char_constants< wchar_t >
{
    typedef wchar_t char_type;
    typedef std::basic_string< char_type > string_type;
    typedef boost::log::basic_string_literal< char_type > literal_type;

    static const char_type char_comment = L'#';
    static const char_type char_comma = L',';
    static const char_type char_dot = L'.';
    static const char_type char_quote = L'"';
    static const char_type char_percent = L'%';
    static const char_type char_exclamation = L'!';
    static const char_type char_and = L'&';
    static const char_type char_or = L'|';
    static const char_type char_equal = L'=';
    static const char_type char_greater = L'>';
    static const char_type char_less = L'<';
    static const char_type char_underline = L'_';
    static const char_type char_backslash = L'\\';
    static const char_type char_section_bracket_left = L'[';
    static const char_type char_section_bracket_right = L']';
    static const char_type char_paren_bracket_left = L'(';
    static const char_type char_paren_bracket_right = L')';

    static const char_type* not_keyword() { return L"not"; }
    static const char_type* and_keyword() { return L"and"; }
    static const char_type* or_keyword() { return L"or"; }
    static const char_type* equal_keyword() { return L"="; }
    static const char_type* greater_keyword() { return L">"; }
    static const char_type* less_keyword() { return L"<"; }
    static const char_type* not_equal_keyword() { return L"!="; }
    static const char_type* greater_or_equal_keyword() { return L">="; }
    static const char_type* less_or_equal_keyword() { return L"<="; }
    static const char_type* begins_with_keyword() { return L"begins_with"; }
    static const char_type* ends_with_keyword() { return L"ends_with"; }
    static const char_type* contains_keyword() { return L"contains"; }
    static const char_type* matches_keyword() { return L"matches"; }

    static const char_type* message_text_keyword() { return L"_"; }

    static literal_type true_keyword() { return literal_type(L"true"); }
    static literal_type false_keyword() { return literal_type(L"false"); }

    static const char_type* default_level_attribute_name() { return L"Severity"; }

    static const char_type* core_section_name() { return L"Core"; }
    static const char_type* sink_section_name_prefix() { return L"Sink:"; }

    static const char_type* core_disable_logging_param_name() { return L"DisableLogging"; }
    static const char_type* filter_param_name() { return L"Filter"; }

    static const char_type* sink_destination_param_name() { return L"Destination"; }
    static const char_type* file_name_param_name() { return L"FileName"; }
    static const char_type* rotation_size_param_name() { return L"RotationSize"; }
    static const char_type* rotation_interval_param_name() { return L"RotationInterval"; }
    static const char_type* rotation_time_point_param_name() { return L"RotationTimePoint"; }
    static const char_type* append_param_name() { return L"Append"; }
    static const char_type* enable_final_rotation_param_name() { return L"EnableFinalRotation"; }
    static const char_type* auto_flush_param_name() { return L"AutoFlush"; }
    static const char_type* auto_newline_mode_param_name() { return L"AutoNewline"; }
    static const char_type* asynchronous_param_name() { return L"Asynchronous"; }
    static const char_type* format_param_name() { return L"Format"; }
    static const char_type* provider_id_param_name() { return L"ProviderID"; }
    static const char_type* log_name_param_name() { return L"LogName"; }
    static const char_type* source_name_param_name() { return L"LogSource"; }
    static const char_type* registration_param_name() { return L"Registration"; }
    static const char_type* local_address_param_name() { return L"LocalAddress"; }
    static const char_type* target_address_param_name() { return L"TargetAddress"; }
    static const char_type* target_param_name() { return L"Target"; }
    static const char_type* max_size_param_name() { return L"MaxSize"; }
    static const char_type* max_files_param_name() { return L"MaxFiles"; }
    static const char_type* min_free_space_param_name() { return L"MinFreeSpace"; }
    static const char_type* scan_for_files_param_name() { return L"ScanForFiles"; }

    static const char_type* scan_method_all() { return L"All"; }
    static const char_type* scan_method_matching() { return L"Matching"; }

    static const char_type* auto_newline_mode_disabled() { return L"Disabled"; }
    static const char_type* auto_newline_mode_always_insert() { return L"AlwaysInsert"; }
    static const char_type* auto_newline_mode_insert_if_missing() { return L"InsertIfMissing"; }

    static const char_type* registration_never() { return L"Never"; }
    static const char_type* registration_on_demand() { return L"OnDemand"; }
    static const char_type* registration_forced() { return L"Forced"; }

    static const char_type* text_file_destination() { return L"TextFile"; }
    static const char_type* console_destination() { return L"Console"; }
    static const char_type* syslog_destination() { return L"Syslog"; }
    static const char_type* simple_event_log_destination() { return L"SimpleEventLog"; }
    static const char_type* debugger_destination() { return L"Debugger"; }

    static literal_type monday_keyword() { return literal_type(L"Monday"); }
    static literal_type short_monday_keyword() { return literal_type(L"Mon"); }
    static literal_type tuesday_keyword() { return literal_type(L"Tuesday"); }
    static literal_type short_tuesday_keyword() { return literal_type(L"Tue"); }
    static literal_type wednesday_keyword() { return literal_type(L"Wednesday"); }
    static literal_type short_wednesday_keyword() { return literal_type(L"Wed"); }
    static literal_type thursday_keyword() { return literal_type(L"Thursday"); }
    static literal_type short_thursday_keyword() { return literal_type(L"Thu"); }
    static literal_type friday_keyword() { return literal_type(L"Friday"); }
    static literal_type short_friday_keyword() { return literal_type(L"Fri"); }
    static literal_type saturday_keyword() { return literal_type(L"Saturday"); }
    static literal_type short_saturday_keyword() { return literal_type(L"Sat"); }
    static literal_type sunday_keyword() { return literal_type(L"Sunday"); }
    static literal_type short_sunday_keyword() { return literal_type(L"Sun"); }

    static std::wostream& get_console_log_stream() { return std::wclog; }

    static int to_number(char_type c)
    {
        int n = 0;
        if (c >= L'0' && c <= L'9')
            n = c - L'0';
        else if (c >= L'a' && c <= L'f')
            n = c - L'a' + 10;
        else if (c >= L'A' && c <= L'F')
            n = c - L'A' + 10;
        return n;
    }

    static bool iswxdigit(char_type c)
    {
        return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
    }

    //! Skips spaces in the beginning of the input
    static const char_type* trim_spaces_left(const char_type* begin, const char_type* end);
    //! Skips spaces in the end of the input
    static const char_type* trim_spaces_right(const char_type* begin, const char_type* end);
    //! Scans for the attribute name placeholder in the input
    static const char_type* scan_attr_placeholder(const char_type* begin, const char_type* end);
    //! Parses an operand string (possibly quoted) from the input
    static const char_type* parse_operand(const char_type* begin, const char_type* end, string_type& operand);
    //! Converts escape sequences to the corresponding characters
    static void translate_escape_sequences(string_type& str);
};
#endif

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // BOOST_LOG_PARSER_UTILS_HPP_INCLUDED_
