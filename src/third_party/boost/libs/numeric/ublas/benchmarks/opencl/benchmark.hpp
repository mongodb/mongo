//
// Copyright (c) 2018 Stefan Seefeld
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or
// copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef opencl_benchmark_hpp_
#define opencl_benchmark_hpp_
#define BOOST_UBLAS_ENABLE_OPENCL

#include <boost/numeric/ublas/opencl.hpp>
#include "../benchmark.hpp"
#include "init.hpp"
#include <memory>

namespace boost { namespace numeric { namespace ublas { namespace benchmark {
namespace opencl {

struct base
{
  base(compute::device d = compute::system::default_device())
    : context(d),
      queue(context, d)
  {}
  compute::context context;
  compute::command_queue queue;
};

template <typename T, bool C> struct data_factory;
template <typename T>
struct data_factory<T, true>
{
  typedef T type;
  typedef std::unique_ptr<type> ptr_type;
  static ptr_type create(long) { return ptr_type(new T());}
};
template <typename T>
struct data_factory<T, false>
{
  typedef T type;
  typedef std::unique_ptr<type> ptr_type;
  static ptr_type create(long, compute::context) { return ptr_type(new T());}
};
template <>
struct data_factory<void, true>
{
  typedef void type;
  typedef void *ptr_type;
  static ptr_type create(long) { return 0;}
};
template <>
struct data_factory<void, false>
{
  typedef void type;
  typedef void *ptr_type;
  static ptr_type create(long, compute::context) { return 0;}
};
template <typename T>
struct data_factory<ublas::vector<T>, true>
{
  typedef ublas::vector<T> type;
  typedef std::unique_ptr<type> ptr_type;
  static ptr_type create(long l) { return ptr_type(new type(l));}
};
template <typename T>
struct data_factory<ublas::vector<T>, false>
{
  typedef ublas::vector<T, ublas::opencl::storage> type;
  typedef std::unique_ptr<type> ptr_type;
  static ptr_type create(long l, compute::context c)
  { return ptr_type(new type(l, c));}
};
template <typename T, typename L>
struct data_factory<ublas::matrix<T, L>, true>
{
  typedef ublas::matrix<T, L> type;
  typedef std::unique_ptr<type> ptr_type;
  static ptr_type create(long l)
  { return ptr_type(new type(l, l));}
};
template <typename T, typename L>
struct data_factory<ublas::matrix<T, L>, false>
{
  typedef ublas::matrix<T, L, ublas::opencl::storage> type;
  typedef std::unique_ptr<type> ptr_type;
  static ptr_type create(long l, compute::context c)
  { return ptr_type(new type(l, l, c));}
};

template <typename S, bool> class benchmark;

template <typename R, typename O>
class benchmark<R(O), true> : public base, public ublas::benchmark::benchmark
{
public:
  benchmark(std::string const &name)
    : base(),
      ublas::benchmark::benchmark(name + " with copy")
  {}
  virtual void setup(long l)
  {
    r = data_factory<R, true>::create(l);
    a = data_factory<O, true>::create(l);
    init(*a, l, 200);
  }
  typename data_factory<R, true>::ptr_type r;
  typename data_factory<O, true>::ptr_type a;
};

template <typename R, typename O1, typename O2>
class benchmark<R(O1, O2), true> : public base, public ublas::benchmark::benchmark
{
public:
  benchmark(std::string const &name)
    : base(),
      ublas::benchmark::benchmark(name + " with copy")
  {}
  virtual void setup(long l)
  {
    r = data_factory<R, true>::create(l);
    a = data_factory<O1, true>::create(l);
    init(*a, l, 200);
    b = data_factory<O2, true>::create(l);
    init(*b, l, 200);
  }
  typename data_factory<R, true>::ptr_type r;
  typename data_factory<O1, true>::ptr_type a;
  typename data_factory<O2, true>::ptr_type b;
};

template <typename R, typename O1, typename O2, typename O3>
class benchmark<R(O1, O2, O3), true> : public base, public ublas::benchmark::benchmark
{
public:
  benchmark(std::string const &name)
    : base(),
      ublas::benchmark::benchmark(name + " with copy")
  {}
  virtual void setup(long l)
  {
    r = data_factory<R, true>::create(l);
    a = data_factory<O1, true>::create(l);
    init(*a, l, 200);
    b = data_factory<O2, true>::create(l);
    init(*b, l, 200);
    c = data_factory<O3, true>::create(l);
    init(*c, l, 200);
  }
  typename data_factory<R, true>::ptr_type r;
  typename data_factory<O1, true>::ptr_type a;
  typename data_factory<O2, true>::ptr_type b;
  typename data_factory<O3, true>::ptr_type c;
};

template <typename R, typename O>
class benchmark<R(O), false> : public base, public ublas::benchmark::benchmark
{
public:
  benchmark(std::string const &name)
    : base(),
      ublas::benchmark::benchmark(name + " w/o copy")
  {}
  virtual void setup(long l)
  {
    r = data_factory<R, false>::create(l, context);
    a = data_factory<O, false>::create(l, context);
    init(*a, l, 200);
  }
  typename data_factory<R, false>::ptr_type r;
  typename data_factory<O, false>::ptr_type a;
};

template <typename R, typename O1, typename O2>
class benchmark<R(O1, O2), false> : public base, public ublas::benchmark::benchmark
{
public:
  benchmark(std::string const &name) : base(), ublas::benchmark::benchmark(name + " w/o copy") {}
  virtual void setup(long l)
  {
    r = data_factory<R, false>::create(l, context);
    a = data_factory<O1, false>::create(l, context);
    init(*a, l, 200);
    b = data_factory<O2, false>::create(l, context);
    init(*b, l, 200);
  }
  typename data_factory<R, false>::ptr_type r;
  typename data_factory<O1, false>::ptr_type a;
  typename data_factory<O2, false>::ptr_type b;
};

template <typename R, typename O1, typename O2, typename O3>
class benchmark<R(O1, O2, O3), false> : public base, public ublas::benchmark::benchmark
{
public:
  benchmark(std::string const &name) : base(), ublas::benchmark::benchmark(name + " w/o copy") {}
  virtual void setup(long l)
  {
    r = data_factory<R, false>::create(l, context);
    a = data_factory<O1, false>::create(l, context);
    init(*a, l, 200);
    b = data_factory<O2, false>::create(l, context);
    init(*b, l, 200);
    c = data_factory<O3, false>::create(l, context);
    init(*c, l, 200);
  }
  typename data_factory<R, false>::ptr_type r;
  typename data_factory<O1, false>::ptr_type a;
  typename data_factory<O2, false>::ptr_type b;
  typename data_factory<O3, false>::ptr_type c;
};
}}}}}

#endif
