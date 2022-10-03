// Copyright 2008 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <stdio.h>
#include <re2/filtered_re2.h>
#include <re2/re2.h>

int main() {
  re2::FilteredRE2 f;
  int id;
  f.Add("a.*b.*c", RE2::DefaultOptions, &id);
  std::vector<std::string> v;
  f.Compile(&v);
  std::vector<int> ids;
  f.FirstMatch("abbccc", ids);

  int n;
  if (RE2::FullMatch("axbyc", "a.*b.*c") &&
      RE2::PartialMatch("foo123bar", "(\\d+)", &n) && n == 123) {
    printf("PASS\n");
    return 0;
  }

  printf("FAIL\n");
  return 2;
}
