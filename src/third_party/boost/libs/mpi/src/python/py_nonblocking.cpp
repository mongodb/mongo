// (C) Copyright 2007 
// Douglas Gregor <doug.gregor -at- gmail.com>
// Andreas Kloeckner <inform -at- tiker.net>

// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  Authors: Douglas Gregor, Andreas Kloeckner

/** @file py_nonblocking.cpp
 *
 *  This file reflects the Boost.MPI nonblocking operations into Python
 *  functions.
 */

#include <vector>
#include <iterator>
#include <algorithm>
#include <boost/operators.hpp>
#include <boost/python.hpp>
#include <boost/python/stl_iterator.hpp>
#include <boost/python/suite/indexing/vector_indexing_suite.hpp>
#include <boost/mpi.hpp>
#include <boost/shared_ptr.hpp>
#include "request_with_value.hpp"

using namespace std;
using namespace boost::python;
using namespace boost::mpi;

namespace
{
  template <class ValueType, class RequestIterator>
  class py_call_output_iterator :
    public boost::output_iterator_helper< 
      py_call_output_iterator<ValueType, RequestIterator> >
  {
    private:
      object m_callable;
      RequestIterator m_request_iterator;

    public:
      explicit py_call_output_iterator(object callable, 
          const RequestIterator &req_it)
        : m_callable(callable), m_request_iterator(req_it)
      { }

      py_call_output_iterator &operator=(ValueType const &v)
      {
        m_callable((m_request_iterator++)->get_value_or_none(), v);
        return *this;
      }
  };



  typedef std::vector<python::request_with_value> request_list;
  typedef py_call_output_iterator<status, request_list::iterator> 
    status_value_iterator;


  boost::shared_ptr<request_list> make_request_list_from_py_list(object iterable)
  {
    boost::shared_ptr<request_list> result(new request_list);
    std::copy(
        stl_input_iterator<python::request_with_value>(iterable),
        stl_input_iterator<python::request_with_value>(),
        back_inserter(*result));
    return result;
  }




  class request_list_indexing_suite :
    public vector_indexing_suite<request_list, false, request_list_indexing_suite>
  {
    public:
      // FIXME: requests are not comparable, thus __contains__ makes no sense.
      // Unfortunately, indexing_suites insist on having __contains__ available.
      // Just make it error out for now.

      static bool
        contains(request_list& container, request const& key)
        {
          PyErr_SetString(PyExc_NotImplementedError, "mpi requests are not comparable");
          throw error_already_set();
        }
  };




  void check_request_list_not_empty(const request_list &requests)
  {
    if (requests.size() == 0)
    {
      PyErr_SetString(PyExc_ValueError, "cannot wait on an empty request vector");
      throw error_already_set();
    }

  }





  object wrap_wait_any(request_list &requests)
  {
    check_request_list_not_empty(requests);

    pair<status, request_list::iterator> result = 
      wait_any(requests.begin(), requests.end());

    return boost::python::make_tuple(
        result.second->get_value_or_none(),
        result.first, 
        distance(requests.begin(), result.second));
  }




  object wrap_test_any(request_list &requests)
  {
    check_request_list_not_empty(requests);
    ::boost::optional<pair<status, request_list::iterator> > result = 
      test_any(requests.begin(), requests.end());

    if (result)
      return boost::python::make_tuple(
          result->second->get_value_or_none(),
          result->first, 
          distance(requests.begin(), result->second));
    else
      return object();
  }





  void wrap_wait_all(request_list &requests, object py_callable)
  {
    check_request_list_not_empty(requests);
    if (py_callable != object())
      wait_all(requests.begin(), requests.end(), 
          status_value_iterator(py_callable, requests.begin()));
    else
      wait_all(requests.begin(), requests.end());
  }




  bool wrap_test_all(request_list &requests, object py_callable)
  {
    check_request_list_not_empty(requests);
    if (py_callable != object())
      return bool(test_all(requests.begin(), requests.end(), 
			   status_value_iterator(py_callable, requests.begin())));
    else
      return bool(test_all(requests.begin(), requests.end()));
  }




  int wrap_wait_some(request_list &requests, object py_callable)
  {
    check_request_list_not_empty(requests);
    request_list::iterator first_completed;
    if (py_callable != object())
      first_completed = wait_some(requests.begin(), requests.end(), 
          status_value_iterator(py_callable, requests.begin())).second;
    else
      first_completed = wait_some(requests.begin(), requests.end());

    return distance(requests.begin(), first_completed);
  }




  int wrap_test_some(request_list &requests, object py_callable)
  {
    check_request_list_not_empty(requests);
    request_list::iterator first_completed;
    if (py_callable != object())
      first_completed = test_some(requests.begin(), requests.end(), 
          status_value_iterator(py_callable, requests.begin())).second;
    else
      first_completed = test_some(requests.begin(), requests.end());

    return distance(requests.begin(), first_completed);
  }
}




namespace boost { namespace mpi { namespace python {

extern const char* request_list_init_docstring;
extern const char* request_list_append_docstring;

extern const char* nonblocking_wait_any_docstring;
extern const char* nonblocking_test_any_docstring;
extern const char* nonblocking_wait_all_docstring;
extern const char* nonblocking_test_all_docstring;
extern const char* nonblocking_wait_some_docstring;
extern const char* nonblocking_test_some_docstring;

void export_nonblocking()
{
  using boost::python::arg;

  {
    typedef request_list cl;
    class_<cl>("RequestList", "A list of Request objects.")
      .def("__init__", make_constructor(make_request_list_from_py_list),
          /*arg("iterable"),*/ request_list_init_docstring)
      .def(request_list_indexing_suite())
      ;
  }

  def("wait_any", wrap_wait_any,
      (arg("requests")),
      nonblocking_wait_any_docstring);
  def("test_any", wrap_test_any,
      (arg("requests")),
      nonblocking_test_any_docstring);

  def("wait_all", wrap_wait_all,
      (arg("requests"), arg("callable") = object()),
      nonblocking_wait_all_docstring);
  def("test_all", wrap_test_all,
      (arg("requests"), arg("callable") = object()),
      nonblocking_test_all_docstring);

  def("wait_some", wrap_wait_some,
      (arg("requests"), arg("callable") = object()),
      nonblocking_wait_some_docstring);
  def("test_some", wrap_test_some,
      (arg("requests"), arg("callable") = object()),
      nonblocking_test_some_docstring);
}

} } }
