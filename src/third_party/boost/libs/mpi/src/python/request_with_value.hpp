// (C) Copyright 2006 
// Douglas Gregor <doug.gregor -at- gmail.com>
// Andreas Kloeckner <inform -at- tiker.net>

// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  Authors: Douglas Gregor, Andreas Kloeckner

#ifndef BOOST_MPI_PYTHON_REQUEST_WITH_VALUE_HPP
#define BOOST_MPI_PYTHON_REQUEST_WITH_VALUE_HPP

#include <boost/python.hpp>
#include <boost/mpi.hpp>

namespace boost { namespace mpi { namespace python {

  /** This wrapper adds a @c boost::python::object value to the @c
   * boost::mpi::request structure, for the benefit of @c irecv() requests.
   *
   * In order to be able to return the value of his requests to the user, we
   * need a handle that we can update to contain the transmitted value once the
   * request completes. Since we're passing the address on to irecv to fill at
   * any time in the future, this address may not change over time. 
   *
   * There are two possible cases: 
   * - plain irecv()
   * - skeleton-content irecv()
   *
   * In the first case, we need to own the storage from this object, the
   * m_internal_value is used for this. In the second case, the updated
   * python::object is part of a boost::mpi::python::content object: the
   * m_external_value field handles this case. Furthermore, in the latter case,
   * we now have a lifetime dependency on that content object; this can be
   * handled with the BPL's with_custodian_and_ward facility.
   *
   * Since requests and request_with_value are supposed to be copyconstructible,
   * we can't put the handle immediately inside this instance. Moreover, since
   * we need to be able to put request_with_value inside request_vectors, any
   * values we own must be held in a shared_ptr instance.
   */

  class request_with_value : public request 
  { 
    private:
      boost::shared_ptr<boost::python::object> m_internal_value;
      boost::python::object *m_external_value;

    public:
      request_with_value()
        : m_external_value(0)
      { }
      request_with_value(const request &req)
        : request(req), m_external_value(0)
      { }

      const boost::python::object get_value() const;
      const boost::python::object get_value_or_none() const;

      const boost::python::object wrap_wait();
      const boost::python::object wrap_test();

      friend request_with_value communicator_irecv(const communicator &, int, int);
      friend request_with_value communicator_irecv_content(
          const communicator&, int, int, content&);
  };

} } }

#endif
