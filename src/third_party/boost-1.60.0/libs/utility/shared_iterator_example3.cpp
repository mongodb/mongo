// Copyright 2003 The Trustees of Indiana University.

// Use, modification and distribution is subject to the Boost Software 
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#include "boost/shared_container_iterator.hpp"
#include "boost/shared_ptr.hpp"
#include "boost/tuple/tuple.hpp" // for boost::tie
#include <algorithm>              // for std::copy
#include <iostream>              
#include <vector>


typedef boost::shared_container_iterator< std::vector<int> > iterator;

std::pair<iterator,iterator>
return_range() {
  boost::shared_ptr< std::vector<int> > range(new std::vector<int>());
  range->push_back(0);
  range->push_back(1);
  range->push_back(2);
  range->push_back(3);
  range->push_back(4);
  range->push_back(5);
  return boost::make_shared_container_range(range);
}


int main() {


  iterator i,end;
  
  boost::tie(i,end) = return_range();

  std::copy(i,end,std::ostream_iterator<int>(std::cout,","));
  std::cout.put('\n');

  return 0;
}
