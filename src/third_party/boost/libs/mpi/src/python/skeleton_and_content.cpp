// (C) Copyright 2006 Douglas Gregor <doug.gregor -at- gmail.com>

// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  Authors: Douglas Gregor

/** @file skeleton_and_content.cpp
 *
 *  This file reflects the skeleton/content facilities into Python.
 */
#include <boost/mpi/python/skeleton_and_content.hpp>
#include <boost/mpi/python/serialize.hpp>
#include <boost/python/list.hpp>
#include <typeinfo>
#include <list>
#include "utility.hpp"
#include "request_with_value.hpp"

using namespace boost::python;
using namespace boost::mpi;

namespace boost { namespace mpi { namespace python {

namespace detail {
  typedef std::map<PyTypeObject*, skeleton_content_handler>
    skeleton_content_handlers_type;

// We're actually importing skeleton_content_handlers from skeleton_and_content.cpp.
#if defined(BOOST_HAS_DECLSPEC) && (defined(BOOST_MPI_PYTHON_DYN_LINK) || defined(BOOST_ALL_DYN_LINK))
#  define BOOST_SC_DECL __declspec(dllimport)
#else
#  define BOOST_SC_DECL
#endif

  extern BOOST_SC_DECL skeleton_content_handlers_type skeleton_content_handlers;
}

/**
 * An exception that will be thrown when the object passed to the
 * Python version of skeleton() does not have a skeleton.
 */
struct object_without_skeleton : public std::exception {
  explicit object_without_skeleton(object value) : value(value) { }
  virtual ~object_without_skeleton() throw() { }

  object value;
};

str object_without_skeleton_str(const object_without_skeleton& e)
{
  return str("\nThe skeleton() or get_content() function was invoked for a Python\n"
             "object that is not supported by the Boost.MPI skeleton/content\n"
             "mechanism. To transfer objects via skeleton/content, you must\n"
             "register the C++ type of this object with the C++ function:\n"
             "  boost::mpi::python::register_skeleton_and_content()\n"
             "Object: " + str(e.value) + "\n");
}

/**
 * Extract the "skeleton" from a Python object. In truth, all we're
 * doing at this point is verifying that the object is a C++ type that
 * has been registered for the skeleton/content mechanism.
 */
object skeleton(object value)
{
  PyTypeObject* type = value.ptr()->ob_type;
  detail::skeleton_content_handlers_type::iterator pos = 
    detail::skeleton_content_handlers.find(type);
  if (pos == detail::skeleton_content_handlers.end())
    throw object_without_skeleton(value);
  else
    return pos->second.get_skeleton_proxy(value);
}

/**
 * Extract the "content" from a Python object, which must be a C++
 * type that has been registered for the skeleton/content mechanism.
 */
content get_content(object value)
{
  PyTypeObject* type = value.ptr()->ob_type;
  detail::skeleton_content_handlers_type::iterator pos = 
    detail::skeleton_content_handlers.find(type);
  if (pos == detail::skeleton_content_handlers.end())
    throw object_without_skeleton(value);
  else
    return pos->second.get_content(value);
}

/// Send the content part of a Python object.
void 
communicator_send_content(const communicator& comm, int dest, int tag, 
                          const content& c)
{
  comm.send(dest, tag, c.base());
}

/// Receive the content of a Python object. We return the object
/// received, not the content wrapper.
object 
communicator_recv_content(const communicator& comm, int source, int tag,
                          const content& c, bool return_status)
{
  using boost::python::make_tuple;

  status stat = comm.recv(source, tag, c.base());
  if (return_status)
    return make_tuple(c.object, stat);
  else
    return c.object;
}

/// Receive the content of a Python object. The request object's value
/// attribute will reference the object whose content is being
/// received, not the content wrapper.
request_with_value
communicator_irecv_content(const communicator& comm, int source, int tag,
                           content& c)
{
  request_with_value req(comm.irecv(source, tag, c.base()));
  req.m_external_value = &c.object;
  return req;
}

extern const char* object_without_skeleton_docstring;
extern const char* object_without_skeleton_object_docstring;
extern const char* skeleton_proxy_docstring;
extern const char* skeleton_proxy_object_docstring;
extern const char* content_docstring;
extern const char* skeleton_docstring;
extern const char* get_content_docstring;

void export_skeleton_and_content(class_<communicator>& comm)
{
  using boost::python::arg;

  // Expose the object_without_skeleton exception
  object type = 
    class_<object_without_skeleton>
      ("ObjectWithoutSkeleton", object_without_skeleton_docstring, no_init)
      .def_readonly("object", &object_without_skeleton::value,
                    object_without_skeleton_object_docstring)
      .def("__str__", &object_without_skeleton_str)
    ;
  translate_exception<object_without_skeleton>::declare(type);

  // Expose the Python variants of "skeleton_proxy" and "content", and
  // their generator functions.
  detail::skeleton_proxy_base_type = 
    class_<skeleton_proxy_base>("SkeletonProxy", skeleton_proxy_docstring, 
                                no_init)
      .def_readonly("object", &skeleton_proxy_base::object,
                    skeleton_proxy_object_docstring);
  class_<content>("Content", content_docstring, no_init);
  def("skeleton", &skeleton, arg("object"), skeleton_docstring);
  def("get_content", &get_content, arg("object"), get_content_docstring);

  // Expose communicator send/recv operations for content.
  comm
    .def("send", communicator_send_content,
         (arg("dest"), arg("tag") = 0, arg("value")))
    .def("recv", communicator_recv_content,
         (arg("source") = any_source, arg("tag") = any_tag, arg("buffer"), 
          arg("return_status") = false))
    .def("irecv", communicator_irecv_content,
         (arg("source") = any_source, arg("tag") = any_tag, arg("buffer")),
         with_custodian_and_ward_postcall<0, 4>()
         );
}

} } } // end namespace boost::mpi::python
