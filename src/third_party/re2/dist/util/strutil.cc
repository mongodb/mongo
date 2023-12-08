// Copyright 1999-2005 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "util/strutil.h"

namespace re2 {

void PrefixSuccessor(std::string* prefix) {
  // We can increment the last character in the string and be done
  // unless that character is 255, in which case we have to erase the
  // last character and increment the previous character, unless that
  // is 255, etc. If the string is empty or consists entirely of
  // 255's, we just return the empty string.
  while (!prefix->empty()) {
    char& c = prefix->back();
    if (c == '\xff') {  // char literal avoids signed/unsigned.
      prefix->pop_back();
    } else {
      ++c;
      break;
    }
  }
}

}  // namespace re2
