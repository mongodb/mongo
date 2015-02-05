// Copyright 2003 The Trustees of Indiana University.

// Use, modification and distribution is subject to the Boost Software 
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#include "boost/shared_container_iterator.hpp"
#include "boost/shared_ptr.hpp"
#include <algorithm>
#include <iostream>
#include <vector>

typedef boost::shared_container_iterator< std::vector<int> > iterator;


void set_range(iterator& i, iterator& end)  {

  boost::shared_ptr< std::vector<int> > ints(new std::vector<int>());
  
  ints->push_back(0);
  ints->push_back(1);
  ints->push_back(2);
  ints->push_back(3);
  ints->push_back(4);
  ints->push_back(5);
  
  i = iterator(ints->begin(),ints);
  end = iterator(ints->end(),ints);
}


int main() {

  iterator i,end;

  set_range(i,end);

  std::copy(i,end,std::ostream_iterator<int>(std::cout,","));
  std::cout.put('\n');

  return 0;
}
