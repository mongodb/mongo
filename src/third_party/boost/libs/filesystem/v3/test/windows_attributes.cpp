//  windows_attributes  ----------------------------------------------------------------//

//  Copyright Beman Dawes 2010
   
//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  Library home page: http://www.boost.org/libs/filesystem

//--------------------------------------------------------------------------------------//

//                   Useful for debugging status related issues                         //

//--------------------------------------------------------------------------------------//

#include <boost/filesystem.hpp>
#include <boost/detail/lightweight_main.hpp>
#include <windows.h>
#include <map>
#include <utility>
#include <iostream>
#include <string>

using std::make_pair;
namespace fs = boost::filesystem;

int cpp_main( int argc, char* argv[])
{
  typedef std::map<DWORD, std::string> decode_type;
  decode_type table;

  table.insert(make_pair<DWORD, std::string>(FILE_ATTRIBUTE_ARCHIVE, "FILE_ATTRIBUTE_ARCHIVE"));
  table.insert(make_pair<DWORD, std::string>(FILE_ATTRIBUTE_COMPRESSED, "FILE_ATTRIBUTE_COMPRESSED"));
  table.insert(make_pair<DWORD, std::string>(FILE_ATTRIBUTE_DEVICE, "FILE_ATTRIBUTE_DEVICE"));
  table.insert(make_pair<DWORD, std::string>(FILE_ATTRIBUTE_DIRECTORY, "FILE_ATTRIBUTE_DIRECTORY"));
  table.insert(make_pair<DWORD, std::string>(FILE_ATTRIBUTE_ENCRYPTED, "FILE_ATTRIBUTE_ENCRYPTED"));
  table.insert(make_pair<DWORD, std::string>(FILE_ATTRIBUTE_HIDDEN, "FILE_ATTRIBUTE_HIDDEN"));
  table.insert(make_pair<DWORD, std::string>(FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, "FILE_ATTRIBUTE_NOT_CONTENT_INDEXED"));
  table.insert(make_pair<DWORD, std::string>(FILE_ATTRIBUTE_OFFLINE, "FILE_ATTRIBUTE_OFFLINE"));
  table.insert(make_pair<DWORD, std::string>(FILE_ATTRIBUTE_READONLY, "FILE_ATTRIBUTE_READONLY"));
  table.insert(make_pair<DWORD, std::string>(FILE_ATTRIBUTE_REPARSE_POINT, "FILE_ATTRIBUTE_REPARSE_POINT"));
  table.insert(make_pair<DWORD, std::string>(FILE_ATTRIBUTE_SPARSE_FILE, "FILE_ATTRIBUTE_SPARSE_FILE"));
  table.insert(make_pair<DWORD, std::string>(FILE_ATTRIBUTE_SYSTEM, "FILE_ATTRIBUTE_SYSTEM"));
  table.insert(make_pair<DWORD, std::string>(FILE_ATTRIBUTE_TEMPORARY, "FILE_ATTRIBUTE_TEMPORARY"));
  table.insert(make_pair<DWORD, std::string>(FILE_ATTRIBUTE_VIRTUAL, "FILE_ATTRIBUTE_VIRTUAL"));

  if (argc < 2)
  {
    std::cout << "Usage: windows_attributes path\n";
    return 1;
  }

  //  report Win32  ::GetFileAttributesA()

  DWORD at(::GetFileAttributesA(argv[1]));
  if (at == INVALID_FILE_ATTRIBUTES)
  {
    std::cout << "GetFileAttributes(\"" << argv[1]
              << "\") returned INVALID_FILE_ATTRIBUTES\n";
    return 0;
  }

  std::cout << "GetFileAttributes(\"" << argv[1]
            << "\") returned ";

  bool bar = false;
  for (decode_type::iterator it = table.begin(); it != table.end(); ++it)
  {
    if (!(it->first & at))
      continue;
    if (bar)
      std::cout << " | ";
    bar = true;
    std::cout << it->second;
    at &= ~it->first;
  }
  std::cout << std::endl;

  if (at)
    std::cout << "plus unknown attributes " << at << std::endl;

  //  report Boost Filesystem file_type

  fs::file_status stat = fs::status(argv[1]);

  const char* types[] = 
    { 
    "status_error",
    "file_not_found",
    "regular_file",
    "directory_file",
    // the following may not apply to some operating systems or file systems
    "symlink_file",
    "block_file",
    "character_file",
    "fifo_file",
    "socket_file",
    "reparse_file",  // Windows: FILE_ATTRIBUTE_REPARSE_POINT that is not a symlink
    "type_unknown",  // file does exist", but isn't one of the above types or
                   // we don't have strong enough permission to find its type

    "_detail_directory_symlink"  // internal use only; never exposed to users
  };

  std::cout << "boost::filesystem::status().type() is " << types[stat.type()] << std::endl;

  return 0;
}
