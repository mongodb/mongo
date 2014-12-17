//  test_codecvt.hpp  ------------------------------------------------------------------//

//  Copyright Beman Dawes 2009, 2010

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  Library home page: http://www.boost.org/libs/filesystem

#ifndef BOOST_FILESYSTEM3_TEST_CODECVT_HPP
#define BOOST_FILESYSTEM3_TEST_CODECVT_HPP

#include <boost/filesystem/v3/config.hpp>
#include <locale>
#include <cwchar>  // for mbstate_t

  //------------------------------------------------------------------------------------//
  //                                                                                    //
  //                              class test_codecvt                                    //
  //                                                                                    //
  //  Warning: partial implementation; even do_in and do_out only partially meet the    //
  //  standard library specifications as the "to" buffer must hold the entire result.   //
  //                                                                                    //
  //  The value of a wide character is the value of the corresponding narrow character  //
  //  plus 1. This ensures that compares against expected values will fail if the       //
  //  code conversion did not occur as expected.                                        //
  //                                                                                    //
  //------------------------------------------------------------------------------------//

  class test_codecvt
    : public std::codecvt< wchar_t, char, std::mbstate_t >  
  {
  public:
    explicit test_codecvt()
        : std::codecvt<wchar_t, char, std::mbstate_t>() {}
  protected:

    virtual bool do_always_noconv() const throw() { return false; }

    virtual int do_encoding() const throw() { return 0; }

    virtual std::codecvt_base::result do_in(std::mbstate_t&, 
      const char* from, const char* from_end, const char*& from_next,
      wchar_t* to, wchar_t* to_end, wchar_t*& to_next) const
    {
      for (; from != from_end && to != to_end; ++from, ++to)
        *to = *from + 1;
      if (to == to_end)
        return error;
      *to = L'\0';
      from_next = from;
      to_next = to;
      return ok;
   }

    virtual std::codecvt_base::result do_out(std::mbstate_t&,
      const wchar_t* from, const wchar_t* from_end, const wchar_t*& from_next,
      char* to, char* to_end, char*& to_next) const
    {
      for (; from != from_end && to != to_end; ++from, ++to)
        *to = static_cast<char>(*from - 1);
      if (to == to_end)
        return error;
      *to = '\0';
      from_next = from;
      to_next = to;
      return ok;
    }

    virtual std::codecvt_base::result do_unshift(std::mbstate_t&,
        char* /*from*/, char* /*to*/, char* & /*next*/) const  { return ok; } 

    virtual int do_length(std::mbstate_t&,
      const char* /*from*/, const char* /*from_end*/, std::size_t /*max*/) const  { return 0; }

    virtual int do_max_length() const throw () { return 0; }
  };

#endif  // BOOST_FILESYSTEM3_TEST_CODECVT_HPP
