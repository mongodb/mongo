//
// Copyright (c) 2018 Stefan Seefeld
// All rights reserved.
//
// This file is part of Boost.uBLAS. It is made available under the
// Boost Software License, Version 1.0.
// (Consult LICENSE or http://www.boost.org/LICENSE_1_0.txt)

#include "init.hpp"
#include "benchmark.hpp"

namespace boost { namespace numeric { namespace ublas { namespace benchmark {

template <typename S> class add;

template <typename R, typename O1, typename O2>
class add<R(O1, O2)> : public benchmark
{
public:
  add(std::string const &name) : benchmark(name) {}
  virtual void setup(long l)
  {
    init(a, l, 200);
    init(b, l, 200);
  }
  virtual void operation(long l)
  {
    c = a + b;
  }
private:
  O1 a;
  O2 b;
  R c;
};

}}}}
