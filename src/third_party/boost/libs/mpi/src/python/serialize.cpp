// (C) Copyright 2006 Douglas Gregor <doug.gregor -at- gmail.com>

// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  Authors: Douglas Gregor

/** @file serialize.cpp
 *
 *  This file provides Boost.Serialization support for Python objects.
 */
#include <boost/mpi/python/serialize.hpp>
#include <boost/mpi/python/skeleton_and_content.hpp>
#include <boost/mpi.hpp>

namespace boost { namespace python {

struct pickle::data_t {
  object module;
  object dumps;
  object loads;
};


/// Data used for communicating with the Python `pickle' module.
pickle::data_t* pickle::data;

object pickle::dumps(object obj, int protocol)
{
  if (!data) initialize_data();
  return (data->dumps)(obj, protocol);
}

object pickle::loads(object s)
{
  if (!data) initialize_data();
  return (data->loads)(s);
}

void pickle::initialize_data()
{
  data = new data_t;
  data->module = object(handle<>(PyImport_ImportModule("pickle")));
  data->dumps = data->module.attr("dumps");
  data->loads = data->module.attr("loads");
}

} } // end namespace boost::python

BOOST_PYTHON_DIRECT_SERIALIZATION_ARCHIVE_IMPL(
  ::boost::mpi::packed_iarchive,
  ::boost::mpi::packed_oarchive)

namespace boost { namespace mpi { namespace python { namespace detail {

  boost::python::object skeleton_proxy_base_type; 

  // A map from Python type objects to skeleton/content handlers
  typedef std::map<PyTypeObject*, skeleton_content_handler>
    skeleton_content_handlers_type;

  BOOST_MPI_PYTHON_DECL skeleton_content_handlers_type skeleton_content_handlers;

  bool
  skeleton_and_content_handler_registered(PyTypeObject* type)
  {
    return 
      skeleton_content_handlers.find(type) != skeleton_content_handlers.end();
  }

  void 
  register_skeleton_and_content_handler(PyTypeObject* type, 
                                        const skeleton_content_handler& handler)
  {
    skeleton_content_handlers[type] = handler;
  }

} } } } // end namespace boost::mpi::python::detail
