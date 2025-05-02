//
// Copyright (c) 2018 Stefan Seefeld
// All rights reserved.
//
// This file is part of Boost.uBLAS. It is made available under the
// Boost Software License, Version 1.0.
// (Consult LICENSE or http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <boost/numeric/ublas/opencl.hpp>
#include "../init.hpp"

namespace boost { namespace numeric { namespace ublas { namespace benchmark {

template <typename T>
void init(T &v, unsigned long size, int max_value)
{
  // TBD
}

template <typename T>
void init(vector<T, opencl::storage> &v, unsigned long size, int max_value)
{
  // TBD
}

template <typename T>
void init(matrix<T, opencl::storage> &m, unsigned long size1, unsigned long size2, int max_value)
{
  // TBD
}

template <typename T>
void init(matrix<T, opencl::storage> &m, unsigned long size, int max_value)
{
  init(m, size, size, max_value);
}

}}}}
