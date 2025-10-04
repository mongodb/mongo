// Copyright (C) 2005 Douglas Gregor.

// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

// Compute parents, children, levels, etc. to effect a parallel
// computation tree.

#include <boost/mpi/detail/computation_tree.hpp>

namespace boost { namespace mpi { namespace detail {

int computation_tree::default_branching_factor = 3;

computation_tree
::computation_tree(int rank, int size, int root, int branching_factor)
  : rank(rank), size(size), root(root),
    branching_factor_(branching_factor > 1? branching_factor
                      /* default */: default_branching_factor),
    level_(0)
{
  // The position in the tree, once we've adjusted for non-zero
  // roots.
  int n = (rank + size - root) % size;
  int sum = 0;
  int term = 1;

  /* The level is the smallest value of k such that

  f^0 + f^1 + ... + f^k > n

  for branching factor f and index n in the tree. */
  while (sum <= n) {
    ++level_;
    term *= branching_factor_;
    sum += term;
  }
}

int computation_tree::level_index(int n) const
{
  int sum = 0;
  int term = 1;
  while (n--) {
    sum += term;
    term *= branching_factor_;
  }
  return sum;
}

int computation_tree::parent() const
{
  if (rank == root) return rank;
  int n = rank + size - 1 - root;
  return ((n % size / branching_factor_) + root) % size ;
}

int computation_tree::child_begin() const
{
  // Zero-based index of this node
  int n = (rank + size - root) % size;

  // Compute the index of the child (in a zero-based tree)
  int child_index = level_index(level_ + 1)
    + branching_factor_ * (n - level_index(level_));

  if (child_index >= size) return root;
  else return (child_index + root) % size;
}

} } } // end namespace boost::mpi::detail
