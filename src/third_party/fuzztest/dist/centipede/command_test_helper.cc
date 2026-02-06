// Copyright 2022 The Centipede Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "absl/base/nullability.h"

// A binary linked with the fork server that exits/crashes in different ways.
int main(int argc, char** absl_nonnull argv) {
  assert(argc == 2);
  printf("Got input: %s", argv[1]);
  fflush(stdout);
  if (!strcmp(argv[1], "success")) return EXIT_SUCCESS;
  if (!strcmp(argv[1], "fail")) return EXIT_FAILURE;
  if (!strcmp(argv[1], "ret42")) return 42;
  if (!strcmp(argv[1], "abort")) abort();
  // Sleep longer than kTimeout in CommandDeathTest_ForkServerHangingBinary.
  if (!strcmp(argv[1], "hang")) sleep(5);
  return 17;
}
