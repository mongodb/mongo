/*=============================================================================
    Boost.Wave: A Standard compliant C++ preprocessor library

    http://www.boost.org/

    Copyright (c) 2001-2007 Hartmut Kaiser. Distributed under the Boost
    Software License, Version 1.0. (See accompanying file
    LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/

#if !defined(BOOST_WAVE_CPP_THROW_HPP_INCLUDED)
#define BOOST_WAVE_CPP_THROW_HPP_INCLUDED

#include <string>
#include <boost/throw_exception.hpp>

///////////////////////////////////////////////////////////////////////////////
// helper macro for throwing exceptions
#if !defined(BOOST_WAVE_THROW)
#ifdef BOOST_NO_STRINGSTREAM
#include <strstream>
#define BOOST_WAVE_THROW(cls, code, msg, act_pos)                             \
    {                                                                         \
        using namespace boost::wave;                                          \
        std::strstream stream;                                                \
            stream << cls::severity_text(cls::code) << ": "                   \
            << cls::error_text(cls::code);                                    \
        if ((msg)[0] != 0) stream << ": " << (msg);                           \
        stream << std::ends;                                                  \
        std::string throwmsg = stream.str(); stream.freeze(false);            \
        boost::throw_exception(cls(throwmsg.c_str(), cls::code,               \
            (act_pos).get_line(), (act_pos).get_column(),                     \
            (act_pos).get_file().c_str()));                                   \
    }                                                                         \
    /**/
#else
#include <sstream>
#define BOOST_WAVE_THROW(cls, code, msg, act_pos)                             \
    {                                                                         \
        using namespace boost::wave;                                          \
        std::stringstream stream;                                             \
            stream << cls::severity_text(cls::code) << ": "                   \
            << cls::error_text(cls::code);                                    \
        if ((msg)[0] != 0) stream << ": " << (msg);                           \
        stream << std::ends;                                                  \
        boost::throw_exception(cls(stream.str().c_str(), cls::code,           \
            (act_pos).get_line(), (act_pos).get_column(),                     \
            (act_pos).get_file().c_str()));                                   \
    }                                                                         \
    /**/
#endif // BOOST_NO_STRINGSTREAM
#endif // BOOST_WAVE_THROW

///////////////////////////////////////////////////////////////////////////////
// helper macro for throwing exceptions with additional parameter
#if !defined(BOOST_WAVE_THROW_NAME)
#ifdef BOOST_NO_STRINGSTREAM
#include <strstream>
#define BOOST_WAVE_THROW_NAME(cls, code, msg, act_pos, name)                  \
    {                                                                         \
        using namespace boost::wave;                                          \
        std::strstream stream;                                                \
            stream << cls::severity_text(cls::code) << ": "                   \
            << cls::error_text(cls::code);                                    \
        if ((msg)[0] != 0) stream << ": " << (msg);                           \
        stream << std::ends;                                                  \
        std::string throwmsg = stream.str(); stream.freeze(false);            \
        boost::throw_exception(cls(throwmsg.c_str(), cls::code,               \
            (act_pos).get_line(), (act_pos).get_column(),                     \
            (act_pos).get_file().c_str(), (name)));                           \
    }                                                                         \
    /**/
#else
#include <sstream>
#define BOOST_WAVE_THROW_NAME(cls, code, msg, act_pos, name)                  \
    {                                                                         \
        using namespace boost::wave;                                          \
        std::stringstream stream;                                             \
            stream << cls::severity_text(cls::code) << ": "                   \
            << cls::error_text(cls::code);                                    \
        if ((msg)[0] != 0) stream << ": " << (msg);                           \
        stream << std::ends;                                                  \
        boost::throw_exception(cls(stream.str().c_str(), cls::code,           \
            (act_pos).get_line(), (act_pos).get_column(),                     \
            (act_pos).get_file().c_str(), (name)));                           \
    }                                                                         \
    /**/
#endif // BOOST_NO_STRINGSTREAM
#endif // BOOST_WAVE_THROW_NAME

///////////////////////////////////////////////////////////////////////////////
// helper macro for throwing exceptions with additional parameter
#if !defined(BOOST_WAVE_THROW_VAR)
#ifdef BOOST_NO_STRINGSTREAM
#include <strstream>
#define BOOST_WAVE_THROW_VAR(cls, code, msg, act_pos)                         \
    {                                                                         \
        using namespace boost::wave;                                          \
        std::strstream stream;                                                \
            stream << cls::severity_text(code) << ": "                        \
            << cls::error_text(code);                                         \
        if ((msg)[0] != 0) stream << ": " << (msg);                           \
        stream << std::ends;                                                  \
        std::string throwmsg = stream.str(); stream.freeze(false);            \
        boost::throw_exception(cls(throwmsg.c_str(), code,                    \
            (act_pos).get_line(), (act_pos).get_column(),                     \
            (act_pos).get_file().c_str()));                                   \
    }                                                                         \
    /**/
#else
#include <sstream>
#define BOOST_WAVE_THROW_VAR(cls, code, msg, act_pos)                         \
    {                                                                         \
        using namespace boost::wave;                                          \
        std::stringstream stream;                                             \
            stream << cls::severity_text(code) << ": "                        \
            << cls::error_text(code);                                         \
        if ((msg)[0] != 0) stream << ": " << (msg);                           \
        stream << std::ends;                                                  \
        boost::throw_exception(cls(stream.str().c_str(), code,                \
            (act_pos).get_line(), (act_pos).get_column(),                     \
            (act_pos).get_file().c_str()));                                   \
    }                                                                         \
    /**/
#endif // BOOST_NO_STRINGSTREAM
#endif // BOOST_WAVE_THROW_VAR

#endif // !defined(BOOST_WAVE_CPP_THROW_HPP_INCLUDED)
