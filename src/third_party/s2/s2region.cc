// Copyright 2005 Google Inc. All Rights Reserved.

#include "s2region.h"

S2Region::~S2Region() {
}

bool S2Region::DecodeWithinScope(Decoder* const decoder) {
  return Decode(decoder);
}
