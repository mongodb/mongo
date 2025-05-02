// (C) Copyright 2006 Douglas Gregor <doug.gregor -at- gmail.com>

// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  Authors: Douglas Gregor

/** @file environment.cpp
 *
 *  This file reflects the Boost.MPI "environment" class into Python
 *  methods at module level.
 */

#include <locale>
#include <string>
#include <boost/python.hpp>
#include <boost/mpi.hpp>

using namespace boost::python;
using namespace boost::mpi;

namespace boost { namespace mpi { namespace python {

extern const char* environment_init_docstring;
extern const char* environment_finalize_docstring;
extern const char* environment_abort_docstring;
extern const char* environment_initialized_docstring;
extern const char* environment_finalized_docstring;

/**
 * The environment used by the Boost.MPI Python module. This will be
 * zero-initialized before it is used. 
 */
static environment* env; 
  
bool mpi_init(list python_argv, bool abort_on_exception)
{
  // If MPI is already initialized, do nothing.
  if (environment::initialized())
    return false;

#if PY_MAJOR_VERSION >= 3
  #ifdef BOOST_MPI_HAS_NOARG_INITIALIZATION
    env = new environment(abort_on_exception);
  #else
    #error No argument initialization, supported from MPI 1.2 and up, is needed when using Boost.MPI with Python 3.x
  #endif
#else
  
  // Convert Python argv into C-style argc/argv.
  int my_argc = extract<int>(python_argv.attr("__len__")());
  char** my_argv = new char*[my_argc];
  for (int arg = 0; arg < my_argc; ++arg)
    my_argv[arg] = strdup(extract<const char*>(python_argv[arg]));

  // Initialize MPI
  int mpi_argc = my_argc;
  char** mpi_argv = my_argv;
  env = new environment(mpi_argc, mpi_argv, abort_on_exception);

  // If anything changed, convert C-style argc/argv into Python argv
  if (mpi_argv != my_argv)
    PySys_SetArgv(mpi_argc, mpi_argv);

  for (int arg = 0; arg < mpi_argc; ++arg)
    free(mpi_argv[arg]);
  delete [] mpi_argv;
#endif

  return true;
}

void mpi_finalize()
{
  if (env) {
    delete env;
    env = 0;
  }
}

void export_environment()
{
  using boost::python::arg;

  def("init", mpi_init, (arg("argv"), arg("abort_on_exception") = true),
      environment_init_docstring);
  def("finalize", mpi_finalize, environment_finalize_docstring);

  // Setup initialization and finalization code
  if (!environment::initialized()) {
    // MPI_Init from sys.argv
    object sys = object(handle<>(PyImport_ImportModule("sys")));
    mpi_init(extract<list>(sys.attr("argv")), true);

    // Setup MPI_Finalize call when the program exits
    object atexit = object(handle<>(PyImport_ImportModule("atexit")));
    object finalize = scope().attr("finalize");
    atexit.attr("register")(finalize);
  }

  def("abort", &environment::abort, arg("errcode"),
      environment_abort_docstring);
  def("initialized", &environment::initialized, 
      environment_initialized_docstring);
  def("finalized", &environment::finalized,
      environment_finalized_docstring);
  scope().attr("max_tag") = environment::max_tag();
  scope().attr("collectives_tag") = environment::collectives_tag();
  scope().attr("processor_name") = environment::processor_name();

  if (optional<int> host_rank = environment::host_rank())
    scope().attr("host_rank") = *host_rank;
  else
    scope().attr("host_rank") = object();
  
  if (optional<int> io_rank = environment::io_rank())
    scope().attr("io_rank") = *io_rank;
  else
    scope().attr("io_rank") = object();
}

} } } // end namespace boost::mpi::python
