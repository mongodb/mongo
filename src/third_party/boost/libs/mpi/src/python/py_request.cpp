// (C) Copyright 2006 Douglas Gregor <doug.gregor -at- gmail.com>

// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  Authors: Douglas Gregor

/** @file request.cpp
 *
 *  This file reflects the Boost.MPI @c request class into
 *  Python.
 */
#include <boost/python.hpp>
#include <boost/mpi.hpp>
#include "request_with_value.hpp"

using namespace boost::python;
using namespace boost::mpi;

const object python::request_with_value::get_value() const 
{
  if (m_internal_value.get())
    return *m_internal_value;
  else if (m_external_value)
    return *m_external_value;
  else
  {
    PyErr_SetString(PyExc_ValueError, "request value not available");
    throw boost::python::error_already_set();
  }
}

const object python::request_with_value::get_value_or_none() const 
{
  if (m_internal_value.get())
    return *m_internal_value;
  else if (m_external_value)
    return *m_external_value;
  else
    return object();
}

const object python::request_with_value::wrap_wait()
{
  status stat = request::wait();
  if (m_internal_value.get() || m_external_value)
    return boost::python::make_tuple(get_value(), stat);
  else
    return object(stat);
}

const object python::request_with_value::wrap_test()
{
  ::boost::optional<status> stat = request::test();
  if (stat)
  {
    if (m_internal_value.get() || m_external_value)
      return boost::python::make_tuple(get_value(), *stat);
    else
      return object(*stat);
  }
  else
    return object();
}


namespace boost { namespace mpi { namespace python {
  
const object request_test(request &req)                                         
{                                                                               
  ::boost::optional<status> stat = req.test();                                  
  if (stat)                                                                     
    return object(*stat);                                                       
  else                                                                          
    return object();                                                            
}      

extern const char* request_docstring;
extern const char* request_with_value_docstring;
extern const char* request_wait_docstring;
extern const char* request_test_docstring;
extern const char* request_cancel_docstring;
extern const char* request_value_docstring;

void export_request()
{
  using boost::python::arg;
  using boost::python::object;
  
  {
    typedef request cl;
    class_<cl>("Request", request_docstring, no_init)
      .def("wait", &cl::wait, request_wait_docstring)
      .def("test", &request_test, request_test_docstring)
      .def("cancel", &cl::cancel, request_cancel_docstring)
      ;
  }
  {
    typedef request_with_value cl;
    class_<cl, bases<request> >(
        "RequestWithValue", request_with_value_docstring, no_init)
      .def("wait", &cl::wrap_wait, request_wait_docstring)
      .def("test", &cl::wrap_test, request_test_docstring)
      ;
  }

  implicitly_convertible<request, request_with_value>();
}

} } } // end namespace boost::mpi::python
