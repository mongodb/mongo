//
// Copyright (c) 2018 Stefan Seefeld
// All rights reserved.
//
// This file is part of Boost.uBLAS. It is made available under the
// Boost Software License, Version 1.0.
// (Consult LICENSE or http://www.boost.org/LICENSE_1_0.txt)

#include <boost/numeric/ublas/matrix.hpp>
#include <boost/program_options.hpp>
#include "prod.hpp"
#include <complex>
#include <string>

namespace po = boost::program_options;
namespace ublas = boost::numeric::ublas;
namespace bm = boost::numeric::ublas::benchmark;

template <typename T>
void benchmark(std::string const &type)
{
  using matrix = ublas::matrix<T, ublas::basic_row_major<>>;
  bm::prod<matrix(matrix, matrix)> p("prod(matrix<" + type + ">)");
  p.run(std::vector<long>({1, 2, 4, 8, 16, 32, 64, 128, 256, 512}));//, 1024}));
}

int main(int argc, char **argv)
{
  po::variables_map vm;
  try
  {
    po::options_description desc("Matrix product\n"
                                 "Allowed options");
    desc.add_options()("help,h", "produce help message");
    desc.add_options()("type,t", po::value<std::string>(), "select value-type (float, double, fcomplex, dcomplex)");

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
  if (type == "float")
    benchmark<float>("float");
  else if (type == "double")
    benchmark<double>("double");
  else if (type == "fcomplex")
    benchmark<std::complex<float>>("std::complex<float>");
  else if (type == "dcomplex")
    benchmark<std::complex<double>>("std::complex<double>");
  else
    std::cerr << "unsupported value-type \"" << vm["type"].as<std::string>() << '\"' << std::endl;
}
