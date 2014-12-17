//  Generate an HTML table showing path decomposition  -----------------------//

//  Copyright Beman Dawes 2005.  Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/filesystem for documentation.

//  For purposes of generating the table, support both POSIX and Windows paths
#define BOOST_FILESYSTEM_NAMESPACE posix
#define BOOST_POSIX_PATH
#include "boost/filesystem/path.hpp"

#undef BOOST_FILESYSTEM_PATH_HPP
#undef BOOST_FILESYSTEM_NAMESPACE
#define BOOST_FILESYSTEM_NAMESPACE windows
#define BOOST_WINDOWS_PATH
#include "boost/filesystem/path.hpp"

namespace pos = boost::posix;
namespace win = boost::windows;

#include <iostream>
#include <fstream>

using std::string;
using std::cout;

namespace
{
  std::ifstream infile;
  std::ofstream outfile;

  const string empty_string;

  struct column_abc
  {
    virtual string heading() const = 0;
    virtual string cell_value( const pos::path & p ) const = 0;
    virtual string cell_value( const win::path & p ) const = 0;
  };

  struct c0 : public column_abc
  {
    string heading() const { return string("<code>string()</code>"); }
    string cell_value( const pos::path & p ) const
    { 
      return p.string();
    }
    string cell_value( const win::path & p ) const
    {
      return p.string();
    }
  } o0;

    struct c1 : public column_abc
  {
    string heading() const { return string("<code>file_<br>string()"); }
    string cell_value( const pos::path & p ) const { return p.file_string(); }
    string cell_value( const win::path & p ) const { return p.file_string(); }
  } o1;

  struct c2 : public column_abc
  {
    string heading() const { return string("Elements found<br>by iteration"); }
    string cell_value( const pos::path & p ) const
    {
      string s;
      for( pos::path::iterator i(p.begin()); i != p.end(); ++i )
      {
        if ( i != p.begin() ) s += ',';
        s += '\"';
        s += *i;
        s += '\"';
      }
      if ( s.empty() ) s += "\"\"";
      return s;
    }
    string cell_value( const win::path & p ) const
    {
      string s;
      for( win::path::iterator i(p.begin()); i != p.end(); ++i )
      {
        if ( i != p.begin() ) s += ',';
        s += '\"';
        s += *i;
        s += '\"';
      }
      if ( s.empty() ) s += "\"\"";
      return s;
    }
  } o2;

  struct c3 : public column_abc
  {
    string heading() const { return string("<code>root_<br>path()<br>.string()</code>"); }
    string cell_value( const pos::path & p ) const { return p.root_path().string(); }
    string cell_value( const win::path & p ) const { return p.root_path().string(); }
  } o3;

  struct c4 : public column_abc
  {
    string heading() const { return string("<code>root_<br>name()</code>"); }
    string cell_value( const pos::path & p ) const { return p.root_name(); }
    string cell_value( const win::path & p ) const { return p.root_name(); }
  } o4;

  struct c5 : public column_abc
  {
    string heading() const { return string("<code>root_<br>directory()</code>"); }
    string cell_value( const pos::path & p ) const { return p.root_directory(); }
    string cell_value( const win::path & p ) const { return p.root_directory(); }
  } o5;

  struct c6 : public column_abc
  {
    string heading() const { return string("<code>relative_<br>path()<br>.string()</code>"); }
    string cell_value( const pos::path & p ) const { return p.relative_path().string(); }
    string cell_value( const win::path & p ) const { return p.relative_path().string(); }
  } o6;

  struct c7 : public column_abc
  {
    string heading() const { return string("<code>branch_<br>path()<br>.string()</code>"); }
    string cell_value( const pos::path & p ) const { return p.branch_path().string(); }
    string cell_value( const win::path & p ) const { return p.branch_path().string(); }
  } o7;

  struct c8 : public column_abc
  {
    string heading() const { return string("<code>leaf()</code>"); }
    string cell_value( const pos::path & p ) const { return p.leaf(); }
    string cell_value( const win::path & p ) const { return p.leaf(); }
  } o8;

  const column_abc * column[] = { &o2, &o0, &o1, &o3, &o4, &o5, &o6, &o7, &o8 };

  //  do_cell  ---------------------------------------------------------------//

  void do_cell( const string & test_case, int i )
  {

    string posix_result( column[i]->cell_value( pos::path(test_case) ) );
    string windows_result( column[i]->cell_value( win::path(test_case) ) );

    outfile <<
      (posix_result != windows_result
        ? "<td bgcolor=\"#CCFF99\"><code>"
        : "<td><code>");

    if ( posix_result.empty() || posix_result[0] != '\"' )
      outfile << '\"' << posix_result << '\"';
    else
      outfile << posix_result;

    if ( posix_result != windows_result )
    {
      outfile << "<br>";
      if ( windows_result.empty() || windows_result[0] != '\"' )
        outfile << '\"' << windows_result << '\"';
      else
        outfile << windows_result;
    }

    outfile << "</code></td>\n";
  }

//  do_row  ------------------------------------------------------------------//

  void do_row( const string & test_case )
  {
    outfile << "<tr>\n<td><code>\"" << test_case << "\"</code></td>\n";

    for ( int i = 0; i < sizeof(column)/sizeof(column_abc&); ++i )
    {
      do_cell( test_case, i );
    }

    outfile << "</tr>\n";
  }

//  do_table  ----------------------------------------------------------------//

  void do_table()
  {
    outfile <<
      "<h1>Path Decomposition Table for <i>POSIX</i> and <i>Windows</i></h1>\n"
      "<p>Shaded entries indicate cases where <i>POSIX</i> and <i>Windows</i>\n"
      "implementations yield different results. The top value is the\n"
      "<i>POSIX</i> result and the bottom value is the <i>Windows</i> result.\n" 
      "<table border=\"1\" cellspacing=\"0\" cellpadding=\"5\">\n"
      "<p>\n"
      ;

    // generate the column headings

    outfile << "<tr><td><b>Constructor<br>argument</b></td>\n";

    for ( int i = 0; i < sizeof(column)/sizeof(column_abc&); ++i )
    {
      outfile << "<td><b>" << column[i]->heading() << "</b></td>\n";
    }

    outfile << "</tr>\n";

    // generate a row for each input line

    string test_case;
    while ( std::getline( infile, test_case ) )
    {
      do_row( test_case );
    }

    outfile << "</table>\n";
  }

} // unnamed namespace

//  main  --------------------------------------------------------------------//

#define BOOST_NO_CPP_MAIN_SUCCESS_MESSAGE
#include <boost/test/included/prg_exec_monitor.hpp>

int cpp_main( int argc, char * argv[] ) // note name!
{
  if ( argc != 3 )
  {
    std::cerr <<
      "Usage: path_table input-file output-file\n"
      "  input-file contains the paths to appear in the table.\n"
      "  output-file will contain the generated HTML.\n"
      ;
    return 1;
  }

  infile.open( argv[1] );
  if ( !infile )
  {
    std::cerr << "Could not open input file: " << argv[1] << std::endl;
    return 1;
  }

  outfile.open( argv[2] );
  if ( !outfile )
  {
    std::cerr << "Could not open output file: " << argv[2] << std::endl;
    return 1;
  }

  outfile << "<html>\n"
          "<head>\n"
          "<title>Path Decomposition Table</title>\n"
          "</head>\n"
          "<body bgcolor=\"#ffffff\" text=\"#000000\">\n"
          ;

  do_table();

  outfile << "</body>\n"
          "</html>\n"
          ;

  return 0;
}
