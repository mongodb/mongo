// Copyright (C) 2007 Trustees of Indiana University

// Authors: Douglas Gregor
//          Andrew Lumsdaine

// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#include <boost/mpi/group.hpp>
#include <boost/mpi/communicator.hpp>

namespace boost { namespace mpi {

group::group(const MPI_Group& in_group, bool adopt)
{
  if (in_group != MPI_GROUP_EMPTY) {
    if (adopt) group_ptr.reset(new MPI_Group(in_group), group_free());
    else       group_ptr.reset(new MPI_Group(in_group));
  }
}

optional<int> group::rank() const
{
  if (!group_ptr)
    return optional<int>();

  int rank;
  BOOST_MPI_CHECK_RESULT(MPI_Group_rank, (*group_ptr, &rank));
  if (rank == MPI_UNDEFINED)
    return optional<int>();
  else
    return rank;
}

int group::size() const
{
  if (!group_ptr)
    return 0;

  int size;
  BOOST_MPI_CHECK_RESULT(MPI_Group_size, (*group_ptr, &size));
  return size;
}

bool operator==(const group& g1, const group& g2)
{
  int result;
  BOOST_MPI_CHECK_RESULT(MPI_Group_compare,
                         ((MPI_Group)g1, (MPI_Group)g2, &result));
  return result == MPI_IDENT;
}

group operator|(const group& g1, const group& g2)
{
  MPI_Group result;
  BOOST_MPI_CHECK_RESULT(MPI_Group_union, 
                         ((MPI_Group)g1, (MPI_Group)g2, &result));
  return group(result, /*adopt=*/true);
}

group operator&(const group& g1, const group& g2)
{
  MPI_Group result;
  BOOST_MPI_CHECK_RESULT(MPI_Group_intersection, 
                         ((MPI_Group)g1, (MPI_Group)g2, &result));
  return group(result, /*adopt=*/true);
}

group operator-(const group& g1, const group& g2)
{
  MPI_Group result;
  BOOST_MPI_CHECK_RESULT(MPI_Group_difference, 
                         ((MPI_Group)g1, (MPI_Group)g2, &result));
  return group(result, /*adopt=*/true);
}

template<> 
int*
group::translate_ranks(int* first, int* last, const group& to_group, int* out)
{
  BOOST_MPI_CHECK_RESULT(MPI_Group_translate_ranks,
                         ((MPI_Group)*this,
                          last-first,
                          first,
                          (MPI_Group)to_group,
                          out));
  return out + (last - first);
}

template<> group group::include(int* first, int* last)
{
  MPI_Group result;
  BOOST_MPI_CHECK_RESULT(MPI_Group_incl,
                         ((MPI_Group)*this, last - first, first, &result));
  return group(result, /*adopt=*/true);
}

template<> group group::exclude(int* first, int* last)
{
  MPI_Group result;
  BOOST_MPI_CHECK_RESULT(MPI_Group_excl,
                         ((MPI_Group)*this, last - first, first, &result));
  return group(result, /*adopt=*/true);
}

} } // end namespace boost::mpi
