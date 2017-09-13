// Copyright 2003 The Trustees of Indiana University.

// Use, modification and distribution is subject to the Boost Software 
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  Shared container iterator adaptor 
//  Author: Ronald Garcia
//  See http://boost.org/libs/utility/shared_container_iterator.html 
//  for documentation. 

//
// shared_iterator_test.cpp - Regression tests for shared_container_iterator.
//


#include "boost/shared_container_iterator.hpp"
#include "boost/shared_ptr.hpp"
#include <vector>
#include <cassert>

struct resource {
  static int count;
  resource() { ++count; }
  resource(resource const&) { ++count; }
  ~resource() { --count; }
};
int resource::count = 0;

typedef std::vector<resource> resources_t;

typedef boost::shared_container_iterator< resources_t > iterator;


void set_range(iterator& i, iterator& end)  {

  boost::shared_ptr< resources_t > objs(new resources_t());

  for (int j = 0; j != 6; ++j)
    objs->push_back(resource());
  
  i = iterator(objs->begin(),objs);
  end = iterator(objs->end(),objs);
  assert(resource::count == 6);
}


int main() {

  assert(resource::count == 0);
  
  {
    iterator i;
    {
      iterator end;
      set_range(i,end);
      assert(resource::count == 6);
    }
    assert(resource::count == 6);
  }
  assert(resource::count == 0);
  
  return 0;
}
