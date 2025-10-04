// Copyright (C) 2018 Alain Miniussi <alain.miniussi -at- oca.eu>.

// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#include <sstream>
#include <boost/mpi/error_string.hpp>

namespace boost { namespace mpi {

std::string error_string(int err)
{
  char buffer[MPI_MAX_ERROR_STRING];
  int len;
  int status = MPI_Error_string(err, buffer, &len);
  if (status == MPI_SUCCESS) {
    return std::string(buffer);
  } else {
    std::ostringstream out;
    if (status == MPI_ERR_ARG) {
      out << "<invalid MPI error code " << err << ">";
    } else {
      out << "<got error " << status 
          << " while probing MPI error " << err << ">";
    }
    return out.str();
  }
}
    
} }
