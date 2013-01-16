// Copyright 2004 Google Inc.
// All Rights Reserved.
//
//

#include <iostream>
using std::ostream;
using std::cout;
using std::endl;

#include "int128.h"
#include "integral_types.h"

const uint128 kuint128max(static_cast<uint64>(GG_LONGLONG(0xFFFFFFFFFFFFFFFF)),
                          static_cast<uint64>(GG_LONGLONG(0xFFFFFFFFFFFFFFFF)));

ostream& operator<<(ostream& o, const uint128& b) {
  return (o << b.hi_ << "::" << b.lo_);
}
