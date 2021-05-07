/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   exceptions.cpp
 * \author Andrey Semashev
 * \date   31.10.2009
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#include <boost/log/detail/config.hpp>
#include <boost/throw_exception.hpp>
#include <boost/exception/exception.hpp>
#include <boost/exception/errinfo_at_line.hpp>
#include <boost/exception/info.hpp>
#include <boost/log/exceptions.hpp>
#include <boost/log/support/exception.hpp>
#include <boost/log/detail/header.hpp>

#ifdef _MSC_VER
#pragma warning(push)
// conversion from 'size_t' to 'boost::error_info<boost::throw_line_,int>::value_type', possible loss of data
// No idea why line number is stored as a signed integer in the error info...
#pragma warning(disable: 4267)
#endif

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

//! Attaches attribute name exception information
BOOST_LOG_API void attach_attribute_name_info(exception& e, attribute_name const& name)
{
    e << attribute_name_info(name);
}

} // namespace aux

bad_alloc::bad_alloc(const char* descr) :
    m_message(descr)
{
}

bad_alloc::bad_alloc(std::string const& descr) :
    m_message(descr)
{
}

bad_alloc::~bad_alloc() throw()
{
}

const char* bad_alloc::what() const throw()
{
    return m_message.c_str();
}

void bad_alloc::throw_(const char* file, std::size_t line, const char* descr)
{
    boost::throw_exception(boost::enable_error_info(bad_alloc(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

void bad_alloc::throw_(const char* file, std::size_t line, std::string const& descr)
{
    boost::throw_exception(boost::enable_error_info(bad_alloc(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

capacity_limit_reached::capacity_limit_reached(const char* descr) :
    bad_alloc(descr)
{
}

capacity_limit_reached::capacity_limit_reached(std::string const& descr) :
    bad_alloc(descr)
{
}

capacity_limit_reached::~capacity_limit_reached() throw()
{
}

void capacity_limit_reached::throw_(const char* file, std::size_t line, const char* descr)
{
    boost::throw_exception(boost::enable_error_info(capacity_limit_reached(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

void capacity_limit_reached::throw_(const char* file, std::size_t line, std::string const& descr)
{
    boost::throw_exception(boost::enable_error_info(capacity_limit_reached(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

runtime_error::runtime_error(std::string const& descr) :
    std::runtime_error(descr)
{
}

runtime_error::~runtime_error() throw()
{
}

missing_value::missing_value() :
    runtime_error("Requested value not found")
{
}

missing_value::missing_value(std::string const& descr) :
    runtime_error(descr)
{
}

missing_value::~missing_value() throw()
{
}

void missing_value::throw_(const char* file, std::size_t line)
{
    boost::throw_exception(boost::enable_error_info(missing_value())
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

void missing_value::throw_(const char* file, std::size_t line, const char* descr)
{
    boost::throw_exception(boost::enable_error_info(missing_value(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

void missing_value::throw_(const char* file, std::size_t line, std::string const& descr)
{
    boost::throw_exception(boost::enable_error_info(missing_value(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

void missing_value::throw_(const char* file, std::size_t line, const char* descr, attribute_name const& name)
{
    boost::throw_exception(boost::enable_error_info(missing_value(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
        << attribute_name_info(name)
    );
}

void missing_value::throw_(const char* file, std::size_t line, std::string const& descr, attribute_name const& name)
{
    boost::throw_exception(boost::enable_error_info(missing_value(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
        << attribute_name_info(name)
    );
}

invalid_type::invalid_type() :
    runtime_error("Requested value has invalid type")
{
}

invalid_type::invalid_type(std::string const& descr) :
    runtime_error(descr)
{
}

invalid_type::~invalid_type() throw()
{
}

void invalid_type::throw_(const char* file, std::size_t line)
{
    boost::throw_exception(boost::enable_error_info(invalid_type())
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

void invalid_type::throw_(const char* file, std::size_t line, const char* descr)
{
    boost::throw_exception(boost::enable_error_info(invalid_type(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

void invalid_type::throw_(const char* file, std::size_t line, std::string const& descr)
{
    boost::throw_exception(boost::enable_error_info(invalid_type(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

void invalid_type::throw_(const char* file, std::size_t line, const char* descr, attribute_name const& name)
{
    boost::throw_exception(boost::enable_error_info(invalid_type(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
        << attribute_name_info(name)
    );
}

void invalid_type::throw_(const char* file, std::size_t line, std::string const& descr, attribute_name const& name)
{
    boost::throw_exception(boost::enable_error_info(invalid_type(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
        << attribute_name_info(name)
    );
}

void invalid_type::throw_(const char* file, std::size_t line, const char* descr, typeindex::type_index const& type)
{
    boost::throw_exception(boost::enable_error_info(invalid_type(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
        << type_info_info(type)
    );
}

void invalid_type::throw_(const char* file, std::size_t line, std::string const& descr, typeindex::type_index const& type)
{
    boost::throw_exception(boost::enable_error_info(invalid_type(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
        << type_info_info(type)
    );
}

void invalid_type::throw_(const char* file, std::size_t line, const char* descr, attribute_name const& name, typeindex::type_index const& type)
{
    boost::throw_exception(boost::enable_error_info(invalid_type(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
        << attribute_name_info(name)
        << type_info_info(type)
    );
}

void invalid_type::throw_(const char* file, std::size_t line, std::string const& descr, attribute_name const& name, typeindex::type_index const& type)
{
    boost::throw_exception(boost::enable_error_info(invalid_type(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
        << attribute_name_info(name)
        << type_info_info(type)
    );
}

invalid_value::invalid_value() :
    runtime_error("The value is invalid")
{
}

invalid_value::invalid_value(std::string const& descr) :
    runtime_error(descr)
{
}

invalid_value::~invalid_value() throw()
{
}

void invalid_value::throw_(const char* file, std::size_t line)
{
    boost::throw_exception(boost::enable_error_info(invalid_value())
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

void invalid_value::throw_(const char* file, std::size_t line, const char* descr)
{
    boost::throw_exception(boost::enable_error_info(invalid_value(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

void invalid_value::throw_(const char* file, std::size_t line, std::string const& descr)
{
    boost::throw_exception(boost::enable_error_info(invalid_value(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

parse_error::parse_error() :
    runtime_error("Failed to parse content")
{
}

parse_error::parse_error(std::string const& descr) :
    runtime_error(descr)
{
}

parse_error::~parse_error() throw()
{
}

void parse_error::throw_(const char* file, std::size_t line)
{
    boost::throw_exception(boost::enable_error_info(parse_error())
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

void parse_error::throw_(const char* file, std::size_t line, const char* descr)
{
    boost::throw_exception(boost::enable_error_info(parse_error(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

void parse_error::throw_(const char* file, std::size_t line, std::string const& descr)
{
    boost::throw_exception(boost::enable_error_info(parse_error(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

void parse_error::throw_(const char* file, std::size_t line, const char* descr, std::size_t content_line)
{
    boost::throw_exception(boost::enable_error_info(parse_error(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
        << boost::errinfo_at_line(content_line)
    );
}

void parse_error::throw_(const char* file, std::size_t line, std::string const& descr, std::size_t content_line)
{
    boost::throw_exception(boost::enable_error_info(parse_error(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
        << boost::errinfo_at_line(content_line)
    );
}

void parse_error::throw_(const char* file, std::size_t line, const char* descr, attribute_name const& name)
{
    boost::throw_exception(boost::enable_error_info(parse_error(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
        << attribute_name_info(name)
    );
}

void parse_error::throw_(const char* file, std::size_t line, std::string const& descr, attribute_name const& name)
{
    boost::throw_exception(boost::enable_error_info(parse_error(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
        << attribute_name_info(name)
    );
}

conversion_error::conversion_error() :
    runtime_error("Failed to perform conversion")
{
}

conversion_error::conversion_error(std::string const& descr) :
    runtime_error(descr)
{
}

conversion_error::~conversion_error() throw()
{
}

void conversion_error::throw_(const char* file, std::size_t line)
{
    boost::throw_exception(boost::enable_error_info(conversion_error())
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

void conversion_error::throw_(const char* file, std::size_t line, const char* descr)
{
    boost::throw_exception(boost::enable_error_info(conversion_error(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

void conversion_error::throw_(const char* file, std::size_t line, std::string const& descr)
{
    boost::throw_exception(boost::enable_error_info(conversion_error(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

system_error::system_error(boost::system::error_code code, std::string const& descr) :
    boost::system::system_error(code, descr)
{
}

system_error::~system_error() throw()
{
}

void system_error::throw_(const char* file, std::size_t line, const char* descr, int system_error_code)
{
    boost::throw_exception(boost::enable_error_info(system_error(boost::system::error_code(system_error_code, boost::system::system_category()), descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

void system_error::throw_(const char* file, std::size_t line, std::string const& descr, int system_error_code)
{
    boost::throw_exception(boost::enable_error_info(system_error(boost::system::error_code(system_error_code, boost::system::system_category()), descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

void system_error::throw_(const char* file, std::size_t line, const char* descr, boost::system::error_code code)
{
    boost::throw_exception(boost::enable_error_info(system_error(code, descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

void system_error::throw_(const char* file, std::size_t line, std::string const& descr, boost::system::error_code code)
{
    boost::throw_exception(boost::enable_error_info(system_error(code, descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

logic_error::logic_error(std::string const& descr) :
    std::logic_error(descr)
{
}

logic_error::~logic_error() throw()
{
}

void logic_error::throw_(const char* file, std::size_t line, const char* descr)
{
    boost::throw_exception(boost::enable_error_info(logic_error(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

void logic_error::throw_(const char* file, std::size_t line, std::string const& descr)
{
    boost::throw_exception(boost::enable_error_info(logic_error(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

odr_violation::odr_violation() :
    logic_error("ODR violation detected")
{
}

odr_violation::odr_violation(std::string const& descr) :
    logic_error(descr)
{
}

odr_violation::~odr_violation() throw()
{
}

void odr_violation::throw_(const char* file, std::size_t line)
{
    boost::throw_exception(boost::enable_error_info(odr_violation())
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

void odr_violation::throw_(const char* file, std::size_t line, const char* descr)
{
    boost::throw_exception(boost::enable_error_info(odr_violation(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

void odr_violation::throw_(const char* file, std::size_t line, std::string const& descr)
{
    boost::throw_exception(boost::enable_error_info(odr_violation(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

unexpected_call::unexpected_call() :
    logic_error("Invalid call sequence")
{
}

unexpected_call::unexpected_call(std::string const& descr) :
    logic_error(descr)
{
}

unexpected_call::~unexpected_call() throw()
{
}

void unexpected_call::throw_(const char* file, std::size_t line)
{
    boost::throw_exception(boost::enable_error_info(unexpected_call())
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

void unexpected_call::throw_(const char* file, std::size_t line, const char* descr)
{
    boost::throw_exception(boost::enable_error_info(unexpected_call(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

void unexpected_call::throw_(const char* file, std::size_t line, std::string const& descr)
{
    boost::throw_exception(boost::enable_error_info(unexpected_call(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

setup_error::setup_error() :
    logic_error("The library is not initialized properly")
{
}

setup_error::setup_error(std::string const& descr) :
    logic_error(descr)
{
}

setup_error::~setup_error() throw()
{
}

void setup_error::throw_(const char* file, std::size_t line)
{
    boost::throw_exception(boost::enable_error_info(setup_error())
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

void setup_error::throw_(const char* file, std::size_t line, const char* descr)
{
    boost::throw_exception(boost::enable_error_info(setup_error(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

void setup_error::throw_(const char* file, std::size_t line, std::string const& descr)
{
    boost::throw_exception(boost::enable_error_info(setup_error(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

limitation_error::limitation_error() :
    logic_error("Boost.Log library limit reached")
{
}

limitation_error::limitation_error(std::string const& descr) :
    logic_error(descr)
{
}

limitation_error::~limitation_error() throw()
{
}

void limitation_error::throw_(const char* file, std::size_t line)
{
    boost::throw_exception(boost::enable_error_info(limitation_error())
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

void limitation_error::throw_(const char* file, std::size_t line, const char* descr)
{
    boost::throw_exception(boost::enable_error_info(limitation_error(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

void limitation_error::throw_(const char* file, std::size_t line, std::string const& descr)
{
    boost::throw_exception(boost::enable_error_info(limitation_error(descr))
        << boost::throw_file(file)
        << boost::throw_line(line)
    );
}

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <boost/log/detail/footer.hpp>
