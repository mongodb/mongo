
#ifndef TEST_FRMWK_HPP___
#define TEST_FRMWK_HPP___

/* Copyright (c) 2002,2003 CrystalClear Software, Inc.
 * Use, modification and distribution is subject to the 
 * Boost Software License, Version 1.0. (See accompanying
 * file LICENSE-1.0 or http://www.boost.org/LICENSE-1.0)
 * $Date: 2003/11/23 03:29:56 $
 */


#include <iostream>
#include <string>

//! Really simple test framework for counting and printing
class TestStats
{
public:
  static TestStats& instance() {static TestStats ts; return ts;}
  void addPassingTest() {testcount_++; passcount_++;}
  void addFailingTest() {testcount_++;}
  unsigned int testcount() const {return testcount_;}
  unsigned int passcount() const {return passcount_;}
  void print(std::ostream& out = std::cout) const
  {
    out << testcount_ << " Tests Executed: " ;
    if (passcount() != testcount()) {
      out << (testcount() - passcount()) << " FAILURES";
    }
    else {
      out << "All Succeeded" << std::endl;
    }
    out << std::endl;
  }
private:  
  TestStats() : testcount_(0), passcount_(0) {}
  unsigned int testcount_;
  unsigned int passcount_;
};


bool check(const std::string& testname, bool testcond) 
{
  TestStats& stat = TestStats::instance();
  if (testcond) {
    std::cout << "Pass :: " << testname << " " <<  std::endl;
    stat.addPassingTest();
    return true;
  }
  else {
    stat.addFailingTest();
    std::cout << "FAIL :: " << testname << " " <<  std::endl;
    return false;
  }
}


int printTestStats() 
{
  TestStats& stat = TestStats::instance();
  stat.print();
  return stat.testcount() - stat.passcount();
}

#endif
