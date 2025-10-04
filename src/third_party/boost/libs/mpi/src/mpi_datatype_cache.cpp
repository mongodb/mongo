// (C) Copyright 2005 Matthias Troyer 

// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  Authors: Matthias Troyer

#include <boost/archive/detail/archive_serializer_map.hpp>
#include <boost/mpi/detail/mpi_datatype_cache.hpp>
#include <boost/mpi/datatype.hpp>
#include <map>

namespace boost { namespace mpi { namespace detail {

  typedef std::map<std::type_info const*,MPI_Datatype,type_info_compare>
      stored_map_type;

  struct mpi_datatype_map::implementation
  {
    stored_map_type map;
  };

  mpi_datatype_map::mpi_datatype_map()
  {
      impl = new implementation();
  }

  void mpi_datatype_map::clear()
  {
    // do not free after call to MPI_FInalize
    int finalized=0;
    BOOST_MPI_CHECK_RESULT(MPI_Finalized,(&finalized));
    if (!finalized) {
      // ignore errors in the destructor
      for (stored_map_type::iterator it=impl->map.begin(); it != impl->map.end(); ++it)
        MPI_Type_free(&(it->second));
      // We explicitly created this one, as there is no equivalent in the MPI standard
      MPI_Datatype bool_type = get_mpi_datatype(bool());
      MPI_Type_free(&bool_type);
    }
  }
  
  
  mpi_datatype_map::~mpi_datatype_map()
  {
    clear();
    delete impl;
  }

  MPI_Datatype mpi_datatype_map::get(const std::type_info* t)
  {
      stored_map_type::iterator pos = impl->map.find(t);
      if (pos != impl->map.end())
          return pos->second;
      else
          return MPI_DATATYPE_NULL;
  }

  void mpi_datatype_map::set(const std::type_info* t, MPI_Datatype datatype)
  {
      impl->map[t] = datatype;
  }

  mpi_datatype_map& mpi_datatype_cache()
  {
    static mpi_datatype_map cache;
    return cache;
  }
} } }
