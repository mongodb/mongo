//
// Copyright (c) 2018 Stefan Seefeld
// All rights reserved.
//
// This file is part of Boost.uBLAS. It is made available under the
// Boost Software License, Version 1.0.
// (Consult LICENSE or http://www.boost.org/LICENSE_1_0.txt)

#include <boost/numeric/ublas/vector.hpp>
#include <boost/numeric/ublas/matrix.hpp>

namespace boost { namespace numeric { namespace ublas { namespace benchmark {

template <typename T>
void init(vector<T> &v, unsigned long size, int max_value)
{
  v = vector<T>(size);
  for (unsigned long i = 0; i < v.size(); ++i)
      v(i) = std::rand() % max_value;
}

template <typename T, typename L>
void init(matrix<T, L> &m, unsigned long size1, unsigned long size2, int max_value)
{
  m = matrix<T, L>(size1, size2);
  for (unsigned long i = 0; i < m.size1(); ++i)
    for (unsigned long j = 0; j < m.size2(); ++j)
      m(i, j) = std::rand() % max_value;
}

template <typename T, typename L>
void init(matrix<T, L> &m, unsigned long size, int max_value)
{
  return init(m, size, size, max_value);
}

}}}}
