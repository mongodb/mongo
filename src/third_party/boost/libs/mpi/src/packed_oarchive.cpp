// (C) Copyright 2005 Matthias Troyer 

// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  Authors: Matthias Troyer

#define BOOST_ARCHIVE_SOURCE
#include <boost/mpi/packed_oarchive.hpp>
#include <boost/archive/detail/archive_serializer_map.hpp>
#include <boost/archive/impl/archive_serializer_map.ipp>

namespace boost { namespace archive {
// explicitly instantiate all required templates

template class detail::archive_serializer_map<mpi::packed_oarchive> ;

} } // end namespace boost::archive
