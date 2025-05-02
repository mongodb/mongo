//
// Copyright (c) 2018 Stefan Seefeld
// All rights reserved.
//
// This file is part of Boost.uBLAS. It is made available under the
// Boost Software License, Version 1.0.
// (Consult LICENSE or http://www.boost.org/LICENSE_1_0.txt)

#define BOOST_UBLAS_ENABLE_OPENCL
#include <boost/numeric/ublas/opencl.hpp>
#include <boost/program_options.hpp>
#include "benchmark.hpp"
#include <complex>
#include <string>

namespace po = boost::program_options;
namespace ublas = boost::numeric::ublas;
namespace bm = boost::numeric::ublas::benchmark;
namespace opencl = boost::numeric::ublas::opencl;

namespace boost { namespace numeric { namespace ublas { namespace benchmark { namespace opencl {

template <typename S, bool C> class inner_prod;

template <typename R, typename V, bool C>
class inner_prod<R(V,V), C> : public benchmark<R(V,V), C>
{
public:
  inner_prod(std::string const &name) : benchmark<R(V,V), C>(name) {}
  virtual void operation(long l)
  {
    ublas::opencl::inner_prod(*this->a, *this->b, this->queue);
  }
};

}}}}}

template <typename T>
void benchmark(std::string const &type, bool copy)
{
  using vector = ublas::vector<T>;
  std::string name = "opencl::inner_prod(vector<" + type + ">)";
  std::vector<long> sizes({1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536});
  if (copy)
  {
    bm::opencl::inner_prod<T(vector,vector), true> p(name);
    p.run(sizes);
  }
  else
  {
    bm::opencl::inner_prod<T(vector,vector), false> p(name);
    p.run(sizes);
  }
}

int main(int argc, char **argv)
{
  opencl::library lib;
  po::variables_map vm;
  try
  {
    po::options_description desc("Inner product\n"
                                 "Allowed options");
    desc.add_options()("help,h", "produce help message");
    desc.add_options()("type,t", po::value<std::string>(), "select value-type (float, double, fcomplex, dcomplex)");
    desc.add_options()("copy,c", po::value<bool>(), "include host<->device copy in timing");

    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help"))
    {
      std::cout << desc << std::endl;
      return 0;
    }
  }
  catch(std::exception &e)
  {
    std::cerr << "error: " << e.what() << std::endl;
    return 1;
  }
  std::string type = vm.count("type") ? vm["type"].as<std::string>() : "float";
  bool copy = vm.count("copy") ? vm["copy"].as<bool>() : false;
  if (type == "float")
    benchmark<float>("float", copy);
  else if (type == "double")
    benchmark<double>("double", copy);
  // else if (type == "fcomplex")
  //   benchmark<std::complex<float>>("std::complex<float>", copy);
  // else if (type == "dcomplex")
  //   benchmark<std::complex<double>>("std::complex<double>", copy);
  else
    std::cerr << "unsupported value-type \"" << vm["type"].as<std::string>() << '\"' << std::endl;
}
