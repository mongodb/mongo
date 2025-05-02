// (C) Copyright 2006 Douglas Gregor <doug.gregor -at- gmail.com>

// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  Authors: Douglas Gregor

/** @file communicator.cpp
 *
 *  This file reflects the Boost.MPI @c communicator class into
 *  Python.
 */
#include <boost/python.hpp>
#include <boost/mpi.hpp>
#include <boost/mpi/python/serialize.hpp>
#include "request_with_value.hpp"

using namespace boost::python;
using namespace boost::mpi;

namespace boost { namespace mpi { namespace python {

extern const char* communicator_docstring;
extern const char* communicator_default_constructor_docstring;
extern const char* communicator_rank_docstring;
extern const char* communicator_size_docstring;
extern const char* communicator_send_docstring;
extern const char* communicator_recv_docstring;
extern const char* communicator_isend_docstring;
extern const char* communicator_irecv_docstring;
extern const char* communicator_probe_docstring;
extern const char* communicator_iprobe_docstring;
extern const char* communicator_barrier_docstring;
extern const char* communicator_split_docstring;
extern const char* communicator_split_key_docstring;
extern const char* communicator_abort_docstring;

object 
communicator_recv(const communicator& comm, int source, int tag,
                  bool return_status)
{
  using boost::python::make_tuple;

  object result;
  status stat = comm.recv(source, tag, result);
  if (return_status)
    return make_tuple(result, stat);
  else
    return result;
}

request_with_value 
communicator_irecv(const communicator& comm, int source, int tag)
{
  boost::shared_ptr<object> result(new object());
  request_with_value req(comm.irecv(source, tag, *result));
  req.m_internal_value = result;
  return req;
}

object
communicator_iprobe(const communicator& comm, int source, int tag)
{
  if (boost::optional<status> result = comm.iprobe(source, tag))
    return object(*result);
  else
    return object();
}

extern void export_skeleton_and_content(class_<communicator>&);

void export_communicator()
{
  using boost::python::arg;
  using boost::python::object;
  
  class_<communicator> comm("Communicator", communicator_docstring);
  comm
    .def(init<>())
    .add_property("rank", &communicator::rank, communicator_rank_docstring)
    .add_property("size", &communicator::size, communicator_size_docstring)
    .def("send", 
         (void (communicator::*)(int, int, const object&) const)
           &communicator::send<object>, 
         (arg("dest"), arg("tag") = 0, arg("value") = object()),
         communicator_send_docstring)
    .def("recv", &communicator_recv, 
         (arg("source") = any_source, arg("tag") = any_tag,
          arg("return_status") = false),
         communicator_recv_docstring)
    .def("isend", 
         (request (communicator::*)(int, int, const object&) const)
           &communicator::isend<object>, 
         (arg("dest"), arg("tag") = 0, arg("value") = object()),
         communicator_isend_docstring)
    .def("irecv", &communicator_irecv, 
         (arg("source") = any_source, arg("tag") = any_tag),
         communicator_irecv_docstring)
    .def("probe", &communicator::probe, 
         (arg("source") = any_source, arg("tag") = any_tag),
         communicator_probe_docstring)
    .def("iprobe", &communicator_iprobe, 
         (arg("source") = any_source, arg("tag") = any_tag),
         communicator_iprobe_docstring)
    .def("barrier", &communicator::barrier, communicator_barrier_docstring)
    .def("__nonzero__", &communicator::operator bool)
    .def("split", 
         (communicator (communicator::*)(int) const)&communicator::split,
         (arg("color")), communicator_split_docstring)
    .def("split", 
         (communicator (communicator::*)(int, int) const)&communicator::split,
         (arg("color"), arg("key")))
    .def("abort", &communicator::abort, arg("errcode"), 
         communicator_abort_docstring)
       ;

  // Module-level attributes
  scope().attr("any_source") = any_source;
  scope().attr("any_tag") = any_tag;

  {
    communicator world;
    scope().attr("world") = world;
    scope().attr("rank") = world.rank();
    scope().attr("size") = world.size();
  }

  // Export skeleton and content
  export_skeleton_and_content(comm);
}

} } } // end namespace boost::mpi::python
