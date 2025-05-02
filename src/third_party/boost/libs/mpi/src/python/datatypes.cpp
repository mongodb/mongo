// (C) Copyright 2006 Douglas Gregor <doug.gregor -at- gmail.com>

// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  Authors: Douglas Gregor

/** @file datatypes.cpp
 *
 *  This file provides datatypes support for Boost.MPI in Python.
 */
#include <boost/mpi/python/serialize.hpp>
#include <boost/mpi.hpp>

namespace boost { namespace mpi { namespace python {

void export_datatypes()
{
#if PY_MAJOR_VERSION < 3 
  register_serialized(long(0), &PyInt_Type);
#endif
  register_serialized(false, &PyBool_Type);
  register_serialized(double(0.0), &PyFloat_Type);
}

} } } // end namespace boost::mpi::python
