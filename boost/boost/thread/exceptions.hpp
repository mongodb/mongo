// Copyright (C) 2001-2003
// William E. Kempf
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying 
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_THREAD_EXCEPTIONS_PDM070801_H
#define BOOST_THREAD_EXCEPTIONS_PDM070801_H

#include <boost/thread/detail/config.hpp>

//  pdm: Sorry, but this class is used all over the place & I end up
//       with recursive headers if I don't separate it
//  wek: Not sure why recursive headers would cause compilation problems
//       given the include guards, but regardless it makes sense to
//       seperate this out any way.

#include <string>
#include <stdexcept>

namespace boost {

class BOOST_THREAD_DECL thread_exception : public std::exception
{
protected:
    thread_exception();
    thread_exception(int sys_err_code);

public:
    ~thread_exception() throw();

    int native_error() const;

private:
    int m_sys_err;
};

class BOOST_THREAD_DECL lock_error : public thread_exception
{
public:
    lock_error();
    lock_error(int sys_err_code);
    ~lock_error() throw();

    virtual const char* what() const throw();
};

class BOOST_THREAD_DECL thread_resource_error : public thread_exception
{
public:
    thread_resource_error();
    thread_resource_error(int sys_err_code);
    ~thread_resource_error() throw();

    virtual const char* what() const throw();
};

class BOOST_THREAD_DECL unsupported_thread_option : public thread_exception
{
public:
    unsupported_thread_option();
    unsupported_thread_option(int sys_err_code);
    ~unsupported_thread_option() throw();

    virtual const char* what() const throw();
};

class BOOST_THREAD_DECL invalid_thread_argument : public thread_exception
{
public:
    invalid_thread_argument();
    invalid_thread_argument(int sys_err_code);
    ~invalid_thread_argument() throw();

    virtual const char* what() const throw();
};

class BOOST_THREAD_DECL thread_permission_error : public thread_exception
{
public:
    thread_permission_error();
    thread_permission_error(int sys_err_code);
    ~thread_permission_error() throw();

    virtual const char* what() const throw();
};

} // namespace boost

#endif // BOOST_THREAD_CONFIG_PDM070801_H

// Change log:
//    3 Jan 03  WEKEMPF Modified for DLL implementation.

