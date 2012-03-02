//  Generate an HTML table showing path decomposition  ---------------------------------//

//  Copyright Beman Dawes 2005.

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  See http://www.boost.org/libs/filesystem for documentation.

//  For purposes of generating the table, support both POSIX and Windows paths

#include "boost/filesystem.hpp"
#include <iostream>
#include <fstream>

using boost::filesystem::path;
using std::string;
using std::cout;

namespace
{
  std::ifstream infile;
  std::ofstream posix_outfile;
  std::ifstream posix_infile;
  std::ofstream outfile;

  bool posix;

  const string empty_string;

  struct column_base
  {
    virtual string heading() const = 0;
    virtual string cell_value( const path & p ) const = 0;
  };

  struct c0 : public column_base
  {
    string heading() const { return string("<code>string()</code>"); }
    string cell_value( const path & p ) const { return p.string(); }
  } o0;

    struct c1 : public column_base
  {
    string heading() const { return string("<code>generic_<br>string()</code>"); }
    string cell_value( const path & p ) const { return p.generic_string(); }
  } o1;

  struct c2 : public column_base
  {
    string heading() const { return string("Iteration<br>over<br>Elements"); }
    string cell_value( const path & p ) const
    {
      string s;
      for( path::iterator i(p.begin()); i != p.end(); ++i )
      {
        if ( i != p.begin() ) s += ',';
        s += (*i).string();
      }
      return s;
    }
  } o2;

  struct c3 : public column_base
  {
    string heading() const { return string("<code>root_<br>path()</code>"); }
    string cell_value( const path & p ) const { return p.root_path().string(); }
  } o3;

  struct c4 : public column_base
  {
    string heading() const { return string("<code>root_<br>name()</code>"); }
    string cell_value( const path & p ) const { return p.root_name().string(); }
  } o4;

  struct c5 : public column_base
  {
    string heading() const { return string("<code>root_<br>directory()</code>"); }
    string cell_value( const path & p ) const { return p.root_directory().string(); }
  } o5;

  struct c6 : public column_base
  {
    string heading() const { return string("<code>relative_<br>path()</code>"); }
    string cell_value( const path & p ) const { return p.relative_path().string(); }
  } o6;

  struct c7 : public column_base
  {
    string heading() const { return string("<code>parent_<br>path()</code>"); }
    string cell_value( const path & p ) const { return p.parent_path().string(); }
  } o7;

  struct c8 : public column_base
  {
    string heading() const { return string("<code>filename()</code>"); }
    string cell_value( const path & p ) const { return p.filename().string(); }
  } o8;

  const column_base * column[] = { &o2, &o0, &o1, &o3, &o4, &o5, &o6, &o7, &o8 };

  //  do_cell  ---------------------------------------------------------------//

  void do_cell( const string & test_case, int i )
  {
    string temp = column[i]->cell_value(path(test_case));
    string value;
    outfile << "<td>";
    if (temp.empty())
      value = "<font size=\"-1\"><i>empty</i></font>";
    else
     value = string("<code>") + temp + "</code>";

    if (posix)
      posix_outfile << value << '\n';
    else
    {
      std::getline(posix_infile, temp);
      if (value != temp) // POSIX and Windows differ
      {
        value.insert(0, "<br>");
        value.insert(0, temp);
        value.insert(0, "<span style=\"background-color: #CCFFCC\">");
        value += "</span>";
      }
      outfile << value;
    }
    outfile << "</td>\n";
  }

//  do_row  ------------------------------------------------------------------//

  void do_row( const string & test_case )
  {
    outfile << "<tr>\n";

    if (test_case.empty())
      outfile << "<td><font size=\"-1\"><i>empty</i></font></td>\n";
    else
      outfile << "<td><code>" << test_case << "</code></td>\n";
    
    for ( int i = 0; i < sizeof(column)/sizeof(column_base&); ++i )
    {
      do_cell( test_case, i );
    }

    outfile << "</tr>\n";
  }

//  do_table  ----------------------------------------------------------------//

  void do_table()
  {
    outfile <<
      "<h1>Path Decomposition Table</h1>\n"
      "<p>Shaded entries indicate cases where <i>POSIX</i> and <i>Windows</i>\n"
      "implementations yield different results. The top value is the\n"
      "<i>POSIX</i> result and the bottom value is the <i>Windows</i> result.\n" 
      "<table border=\"1\" cellspacing=\"0\" cellpadding=\"5\">\n"
      "<p>\n"
      ;

    // generate the column headings

    outfile << "<tr><td><b>Constructor<br>argument</b></td>\n";

    for ( int i = 0; i < sizeof(column)/sizeof(column_base&); ++i )
    {
      outfile << "<td><b>" << column[i]->heading() << "</b></td>\n";
    }

    outfile << "</tr>\n";

    // generate a row for each input line

    string test_case;
    while ( std::getline( infile, test_case ) )
    {
      if (!test_case.empty() && *--test_case.end() == '\r')
        test_case.erase(test_case.size()-1);
      if (test_case.empty() || test_case[0] != '#')
        do_row( test_case );
    }

    outfile << "</table>\n";
  }

} // unnamed namespace

//  main  ------------------------------------------------------------------------------//

#define BOOST_NO_CPP_MAIN_SUCCESS_MESSAGE
#include <boost/test/included/prg_exec_monitor.hpp>

int cpp_main( int argc, char * argv[] ) // note name!
{
  if ( argc != 5 )
  {
    std::cerr <<
      "Usage: path_table \"POSIX\"|\"Windows\" input-file posix-file output-file\n"
      "Run on POSIX first, then on Windows\n"
      "  \"POSIX\" causes POSIX results to be saved in posix-file;\n"
      "  \"Windows\" causes POSIX results read from posix-file\n"
      "  input-file contains the paths to appear in the table.\n"
      "  posix-file will be used for POSIX results\n"
      "  output-file will contain the generated HTML.\n"
      ;
    return 1;
  }

  infile.open( argv[2] );
  if ( !infile )
  {
    std::cerr << "Could not open input file: " << argv[2] << std::endl;
    return 1;
  }

  if (string(argv[1]) == "POSIX")
  {
    posix = true;
    posix_outfile.open( argv[3] );
    if ( !posix_outfile )
    {
      std::cerr << "Could not open POSIX output file: " << argv[3] << std::endl;
      return 1;
    }
  }
  else
  {
    posix = false;
    posix_infile.open( argv[3] );
    if ( !posix_infile )
    {
      std::cerr << "Could not open POSIX input file: " << argv[3] << std::endl;
      return 1;
    }
  }

  outfile.open( argv[4] );
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
