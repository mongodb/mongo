// -*- C++ -*-
//  Boost general library 'format'  ---------------------------
//  See http://www.boost.org for updates, documentation, and revision history.

//  Copyright (c) 2001 Samuel Krempp
//                  krempp@crans.ens-cachan.fr
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

// several suggestions from Jens Maurer

// ------------------------------------------------------------------------------
// bench_variants.cc :  do the same task, with sprintf, stream, and format
//                      and compare their times.

// This benchmark is provided purely for information.
// It might not even compile as-is, 
//   or not give any sensible results. 
//      (e.g., it expects sprintf to be POSIX compliant)

// ------------------------------------------------------------------------------


#include <iostream>
#include <iomanip>
#include <cstdio>  // sprintf
#include <cstring>
#include <fstream>
#include <cmath>   // floor
#include <boost/timer/timer.hpp>
#include <vector>

#include <boost/format.hpp>

// portable /dev/null stream equivalent, by James Kanze, http://www.gabi-soft.de
class NulStreambuf : public std::streambuf
{
public:
  NulStreambuf() { 
      setp( dummyBuffer , dummyBuffer + 64 ) ;
  }
  virtual int  overflow( int c );
  virtual int  underflow(); 
private:
    char                dummyBuffer[ 64 ] ;
} ;

class NulStream : public std::basic_ostream<char, std::char_traits<char> > 
{
public:
  NulStream();
  virtual ~NulStream();
  NulStreambuf*    rdbuf() {
    return static_cast< NulStreambuf* >(
                   ((std::basic_ostream<char, std::char_traits<char> > *) this) -> rdbuf() ) ;
  }
} ;
 

//-------------------------------------------------------------------------------------
//   NulStream implementation

NulStream::NulStream()  : std::basic_ostream<char, std::char_traits<char> > (NULL) {
  init( new NulStreambuf ) ;
}

NulStream::~NulStream() {
    delete rdbuf() ;
}

int  NulStreambuf::underflow(){ return std::ios::traits_type::eof();    
}

int NulStreambuf::overflow( int c ){
    setp( dummyBuffer , dummyBuffer + 64 ) ;
    return (c == std::ios::traits_type::eof()) ? '\0' : c ;
}



// -------------------------------------------------------------------------------------

namespace benchmark {

static int NTests = 300000;

//static std::stringstream nullStream;
static NulStream nullStream;
static boost::timer::nanosecond_type tstream, tpf;
//static const std::string fstring="%3$#x %1$20.10E %2$g %3$d \n";
static const std::string fstring="%3$0#6x %1$20.10E %2$g %3$0+5d \n";
static const double     arg1=45.23;
static const double     arg2=12.34;
static const int        arg3=23;
static const std::string res = 
"0x0017     4.5230000000E+01 12.34 +0023 \n";
//static const std::string res = "23.0000     4.5230000000E+01 12.34 23 \n";

void test_sprintf();
void test_nullstream();
void test_opti_nullstream();
void test_parsed_once_format();
void test_reused_format();
void test_format();
void test_try1();
void test_try2();

void test_sprintf()
{
    using namespace std;

    vector<char> bufr;
    bufr.reserve(4000);
    char *buf = &bufr[0];

    // Check that sprintf is Unix98 compatible on the platform :
    sprintf(buf, fstring.c_str(), arg1, arg2, arg3);
    if( strncmp( buf, res.c_str(), res.size()) != 0 ) {
      cerr << endl << buf;
    }
    // time the loop :
    boost::timer::cpu_timer chrono;
    for(int i=0; i<NTests; ++i) {
      sprintf(buf, fstring.c_str(), arg1, arg2, arg3);
    }
    tpf=chrono.elapsed().wall;
    cout  << left << setw(20) <<"printf time"<< right <<":" << tpf  << endl;
}

void test_try1()
{
  using namespace std;
  boost::io::basic_oaltstringstream<char> oss;
  oss << boost::format(fstring) % arg1 % arg2 % arg3;
  boost::timer::cpu_timer chrono;
  size_t dummy=0;
  for(int i=0; i<NTests; ++i) {
      dummy += oss.cur_size();
  }
  boost::timer::nanosecond_type t = chrono.elapsed().wall;
  cout  << left << setw(20) <<"try1 time"<< right <<":" << setw(5) << t
        << ",  = " << t / tpf << " * printf "
        << ",  = " << t / tstream << " * nullStream"
        << ", accum = " << dummy << endl;
}

void test_try2()
{
  using namespace std;
  boost::io::basic_oaltstringstream<char> oss;
  oss << boost::format(fstring) % arg1 % arg2 % arg3;
  oss << "blas 34567890GGGGGGGGGGGGGGGGGGGGGGGGGGGGggggggggggggggggggggggggggg " << endl;
  string s = oss.cur_str();
  oss << s << s << s;
  oss.clear_buffer();
  oss << s << s;
  s = oss.cur_str();
  boost::timer::cpu_timer chrono;
  size_t dummy=0;
  for(int i=0; i<NTests; ++i) {
      dummy += oss.cur_size();
  }
  boost::timer::nanosecond_type t = chrono.elapsed().wall;
  cout  << left << setw(20) <<"try2 time"<< right <<":" << setw(5) << t
        << ",  = " << t / tpf << " * printf "
        << ",  = " << t / tstream << " * nullStream"
        << ", accum = " << dummy << endl;
}

void do_stream(std::ostream& os) {
    using namespace std;
    std::ios_base::fmtflags f = os.flags();
    os << hex << showbase << internal << setfill('0') << setw(6) << arg3
       << dec << noshowbase << right << setfill(' ') 
       << " " 
       << scientific << setw(20) << setprecision(10) << uppercase << arg1 
       << setprecision(6) << nouppercase ;
    os.flags(f);
    os << " " << arg2 << " " 
       << showpos << setw(5) << internal << setfill('0') << arg3 << " \n" ;
    os.flags(f);
}

void test_nullstream()
{
    using namespace std;
    boost::timer::cpu_timer chrono;
    boost::io::basic_oaltstringstream<char> oss;

    {   
        do_stream(oss);
        if(oss.str() != res ) {
            cerr << endl << oss.str() ;
        }
    }

    for(int i=0; i<NTests; ++i) { 
        do_stream(nullStream);
    }

//     for(int i=0; i<NTests; ++i) { 
//       std::ios_base::fmtflags f0 = nullStream.flags();
//       nullStream << hex << showbase << arg3
//                  << dec << noshowbase << " " 
//                  << scientific << setw(20) << setprecision(10) << uppercase <<  arg1 
//                  << setprecision(0);
//       nullStream.flags(f0);
//       nullStream << " " << arg2 << " " << arg3 << " \n" ;

//     }
    boost::timer::nanosecond_type t = chrono.elapsed().wall;
    cout  << left << setw(20) <<"ostream time"<< right <<":" << setw(5) << t  
          << ",  = " << t / tpf << " * printf \n";
    tstream = t;
}

void test_opti_nullstream()
{
    using namespace std;
    boost::timer::cpu_timer chrono;
    boost::io::basic_oaltstringstream<char> oss;
    //static const std::string fstring="%3$#x %1$20.10E %2$g %3$d \n";

    std::ios_base::fmtflags f0 = oss.flags(), f1, f2;
    streamsize p0 = oss.precision();
    {
      oss << hex << showbase; 
      f1 = oss.flags();
      oss << arg3;

      oss.flags(f0);
      oss << " " << scientific << setw(20) << setprecision(10) << uppercase;
      f2 = oss.flags();
      oss << arg1;

      oss.flags(f0); oss.precision(p0);
      oss << " " << arg2 << " " << arg3 << " \n" ;
    
      if(oss.str() != res ) {
        cerr << endl << oss.str() ;
      }
    }

    for(int i=0; i<NTests; ++i) { 
      nullStream.flags(f1);
      nullStream << arg3;

      nullStream << setw(20) << setprecision(10);
      nullStream.flags(f2);
      nullStream << arg1;

      nullStream.flags(f0); nullStream.precision(p0);
      nullStream << " " << arg2 << " " << arg3 << " \n" ;
    }
    boost::timer::nanosecond_type t = chrono.elapsed().wall;
    cout  << left << setw(20) <<"opti-stream time"<< right <<":" << setw(5) << t  
          << ",  = " << t / tpf << " * printf \n";
    //    tstream = t;
}

void test_parsed_once_format()
{
    using namespace std;
    static const boost::format fmter(fstring);

    boost::io::basic_oaltstringstream<char> oss;
    oss << boost::format(fmter) % arg1 % arg2 % arg3 ;
    if( oss.str() != res ) {
      cerr << endl << oss.str();
    }

    // not only is the format-string parsed once,
    // but also the buffer of the internal stringstream is already allocated.

    boost::timer::cpu_timer chrono;        
    for(int i=0; i<NTests; ++i) {
        nullStream << boost::format(fmter) % arg1 % arg2 % arg3;
    }
    boost::timer::nanosecond_type t=chrono.elapsed().wall;
    cout  << left << setw(20) <<"parsed-once time"<< right <<":" << setw(5) << t 
          << ",  = " << t / tpf << " * printf "
          << ",  = " << t / tstream << " * nullStream \n";
}

void test_reused_format()
{
  using namespace std;
  boost::io::basic_oaltstringstream<char> oss;
  oss << boost::format(fstring) % arg1 % arg2 % arg3;
  if(oss.str() != res ) {
    cerr << endl << oss.str();
  }

  boost::timer::cpu_timer chrono;
  boost::format fmter;
  for(int i=0; i<NTests; ++i) {
    nullStream << fmter.parse(fstring) % arg1 % arg2 % arg3;
  }
  boost::timer::nanosecond_type t = chrono.elapsed().wall;
  cout  << left << setw(20) <<"reused format time"<< right <<":" << setw(5) << t
        << ",  = " << t / tpf << " * printf "
        << ",  = " << t / tstream << " * nullStream \n";
}

void test_format()
{
  using namespace std;
  boost::io::basic_oaltstringstream<char> oss;
  oss << boost::format(fstring) % arg1 % arg2 % arg3;
  if(oss.str() != res ) {
    cerr << endl << oss.str();
  }

  boost::timer::cpu_timer chrono;
  for(int i=0; i<NTests; ++i) {
    nullStream << boost::format(fstring) % arg1 % arg2 % arg3;
  }
  boost::timer::nanosecond_type t = chrono.elapsed().wall;
  cout  << left << setw(20) <<"format time"<< right <<":" << setw(5) << t
        << ",  = " << t / tpf << " * printf "
        << ",  = " << t / tstream << " * nullStream \n";
}

} // benchmark

int main(int argc, char * argv[]) {
    using namespace benchmark;
    using namespace boost;
    using namespace std;
    const string::size_type  npos = string::npos;

    string choices = "";
    if (1<argc) {
        choices = (argv[1]); // profiling is easier launching only one.
        NTests = 1000 * 1000;  // andmoreprecise with many iterations
        cout << "choices (" << choices << ") \n";
    }

    if (choices == "" || choices.find('p') != npos)
        test_sprintf();
    if (choices == "" || choices.find('n') != npos)
        test_nullstream();
    if (choices == "" || choices.find('1') != npos)
        test_parsed_once_format();
    if (choices == "" || choices.find('r') != npos)
        test_reused_format();
    if (choices == "" || choices.find('f') != npos)
        test_format();
    if (choices.find('t') != npos)
        test_try1();
    if (choices.find('y') != npos)
        test_try2();
    if (choices.find('o') != npos)
        test_opti_nullstream();
    return 0;
}

