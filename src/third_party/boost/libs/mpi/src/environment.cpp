// Copyright (C) 2005-2006 Douglas Gregor <doug.gregor@gmail.com>

// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

// Message Passing Interface 1.1 -- 7.1.1. Environmental Inquiries
#include <boost/mpi/environment.hpp>
#include <boost/mpi/exception.hpp>
#include <boost/mpi/detail/mpi_datatype_cache.hpp>
#include <boost/core/uncaught_exceptions.hpp>
#include <cassert>
#include <string>
#include <exception>
#include <stdexcept>
#include <ostream>

namespace boost { namespace mpi {
namespace detail {

int level2int(threading::level l)
{
  using namespace threading;
  
  switch(l) {
  case single:
    return MPI_THREAD_SINGLE;
  case funneled:
    return MPI_THREAD_FUNNELED;
  case serialized:
    return MPI_THREAD_SERIALIZED;
  case multiple:
    return MPI_THREAD_MULTIPLE;
  default:
    environment::abort(70); // EX_SOFTWARE
  }
}
  
threading::level int2level(int mpi_level)
{
  using namespace threading;
  
  switch(mpi_level) {
  case MPI_THREAD_SINGLE:
    return single;
  case MPI_THREAD_FUNNELED:
    return funneled;
  case MPI_THREAD_SERIALIZED:
    return serialized;
  case MPI_THREAD_MULTIPLE:
    return multiple;
  default:
    environment::abort(70); // EX_SOFTWARE
  }
}
} // namespace detail

namespace threading {

std::istream& operator>>(std::istream& in, level& l)
{
  std::string tk;
  in >> tk;
  if (!in.bad()) {
    if (tk == "single") {
      l = single;
    } else if (tk == "funneled") {
      l = funneled;
    } else if (tk == "serialized") {
      l = serialized;
    } else if (tk == "multiple") {
      l = multiple;
    } else {
      in.setstate(std::ios::badbit);
    }
  }
  return in;
}

std::ostream& operator<<(std::ostream& out, level l)
{
  switch(l) {
  case single:
    out << "single";
    break;
  case funneled:
    out << "funneled";
    break;
  case serialized:
    out << "serialized";
    break;
  case multiple:
    out << "multiple";
    break;
  default:
    out << "<level error>[" << int(l) << ']';
    out.setstate(std::ios::badbit);
    break;
  }
  return out;
}

} // namespace threading

#ifdef BOOST_MPI_HAS_NOARG_INITIALIZATION
environment::environment(bool abrt)
  : i_initialized(false),
    abort_on_exception(abrt)
{
  if (!initialized()) {
    BOOST_MPI_CHECK_RESULT(MPI_Init, (0, 0));
    i_initialized = true;
  }

#if (2 <= MPI_VERSION)
  MPI_Comm_set_errhandler(MPI_COMM_WORLD, MPI_ERRORS_RETURN);
#else
  MPI_Errhandler_set(MPI_COMM_WORLD, MPI_ERRORS_RETURN);
#endif
}

environment::environment(threading::level mt_level, bool abrt)
  : i_initialized(false),
    abort_on_exception(abrt)
{
  // It is not clear that we can pass null in MPI_Init_thread.
  int dummy_thread_level = 0;
  if (!initialized()) {
    BOOST_MPI_CHECK_RESULT(MPI_Init_thread, 
                           (0, 0, detail::level2int(mt_level), &dummy_thread_level ));
    i_initialized = true;
  }

#if (2 <= MPI_VERSION)
  MPI_Comm_set_errhandler(MPI_COMM_WORLD, MPI_ERRORS_RETURN);
#else
  MPI_Errhandler_set(MPI_COMM_WORLD, MPI_ERRORS_RETURN);
#endif
}
#endif

environment::environment(int& argc, char** &argv, bool abrt)
  : i_initialized(false),
    abort_on_exception(abrt)
{
  if (!initialized()) {
    BOOST_MPI_CHECK_RESULT(MPI_Init, (&argc, &argv));
    i_initialized = true;
  }

#if (2 <= MPI_VERSION)
  MPI_Comm_set_errhandler(MPI_COMM_WORLD, MPI_ERRORS_RETURN);
#else
  MPI_Errhandler_set(MPI_COMM_WORLD, MPI_ERRORS_RETURN);
#endif
}

environment::environment(int& argc, char** &argv, threading::level mt_level,
                         bool abrt)
  : i_initialized(false),
    abort_on_exception(abrt)
{
  // It is not clear that we can pass null in MPI_Init_thread.
  int dummy_thread_level = 0;
  if (!initialized()) {
    BOOST_MPI_CHECK_RESULT(MPI_Init_thread, 
                           (&argc, &argv, detail::level2int(mt_level), &dummy_thread_level));
    i_initialized = true;
  }

#if (2 <= MPI_VERSION)
  MPI_Comm_set_errhandler(MPI_COMM_WORLD, MPI_ERRORS_RETURN);
#else
  MPI_Errhandler_set(MPI_COMM_WORLD, MPI_ERRORS_RETURN);
#endif
}

environment::~environment()
{
  if (i_initialized) {
    if (boost::core::uncaught_exceptions() > 0 && abort_on_exception) {
      abort(-1);
    } else if (!finalized()) {
      detail::mpi_datatype_cache().clear();
      BOOST_MPI_CHECK_RESULT(MPI_Finalize, ());
    }
  }
}

void environment::abort(int errcode)
{
  BOOST_MPI_CHECK_RESULT(MPI_Abort, (MPI_COMM_WORLD, errcode));
}

bool environment::initialized()
{
  int flag;
  BOOST_MPI_CHECK_RESULT(MPI_Initialized, (&flag));
  return flag != 0;
}

bool environment::finalized()
{
  int flag;
  BOOST_MPI_CHECK_RESULT(MPI_Finalized, (&flag));
  return flag != 0;
}

int environment::max_tag()
{
  int* max_tag_value;
  int found = 0;

#if (2 <= MPI_VERSION)
  BOOST_MPI_CHECK_RESULT(MPI_Comm_get_attr,
                         (MPI_COMM_WORLD, MPI_TAG_UB, &max_tag_value, &found));
#else
  BOOST_MPI_CHECK_RESULT(MPI_Attr_get,
                         (MPI_COMM_WORLD, MPI_TAG_UB, &max_tag_value, &found));
#endif
  assert(found != 0);
  return *max_tag_value - num_reserved_tags;
}

int environment::collectives_tag()
{
  return max_tag() + 1;
}

optional<int> environment::host_rank()
{
  int* host;
  int found = 0;

#if (2 <= MPI_VERSION)
  BOOST_MPI_CHECK_RESULT(MPI_Comm_get_attr,
                         (MPI_COMM_WORLD, MPI_HOST, &host, &found));
#else
  BOOST_MPI_CHECK_RESULT(MPI_Attr_get,
                         (MPI_COMM_WORLD, MPI_HOST, &host, &found));
#endif
  if (!found || *host == MPI_PROC_NULL)
    return optional<int>();
  else
    return *host;
}

optional<int> environment::io_rank()
{
  int* io;
  int found = 0;

#if (2 <= MPI_VERSION)
  BOOST_MPI_CHECK_RESULT(MPI_Comm_get_attr,
                         (MPI_COMM_WORLD, MPI_IO, &io, &found));
#else
  BOOST_MPI_CHECK_RESULT(MPI_Attr_get,
                         (MPI_COMM_WORLD, MPI_IO, &io, &found));
#endif
  if (!found || *io == MPI_PROC_NULL)
    return optional<int>();
  else
    return *io;
}

std::string environment::processor_name()
{
  char name[MPI_MAX_PROCESSOR_NAME];
  int len;

  BOOST_MPI_CHECK_RESULT(MPI_Get_processor_name, (name, &len));
  return std::string(name, len);
}

threading::level environment::thread_level()
{
  int level;

  BOOST_MPI_CHECK_RESULT(MPI_Query_thread, (&level));
  return detail::int2level(level);
}

bool environment::is_main_thread()
{
  int isit;

  BOOST_MPI_CHECK_RESULT(MPI_Is_thread_main, (&isit));
  return static_cast<bool>(isit);
}

std::pair<int, int> environment::version()
{
  int major, minor;
  BOOST_MPI_CHECK_RESULT(MPI_Get_version, (&major, &minor));
  return std::make_pair(major, minor);
}

std::string environment::library_version()
{
#if (3 <= MPI_VERSION)
  char lib_version[MPI_MAX_LIBRARY_VERSION_STRING];
  int len = 0;
  BOOST_MPI_CHECK_RESULT(MPI_Get_library_version, (lib_version, &len));
  return std::string(lib_version, len);
#else
  return "";
#endif
}

} } // end namespace boost::mpi
