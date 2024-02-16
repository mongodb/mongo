// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Test for crashes in tcmalloc during shared library initialization
// http://b/3485510.

#include <assert.h>
#include <pthread.h>
#include <stdint.h>

#include <vector>

namespace {

struct Foo {
  int x;
  Foo() : x(42) {}
};

static void* fn(void*) {
  while (true) {
    std::vector<Foo*> v;
    v.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
      v.push_back(new Foo);
    }
    for (int i = 0; i < 1000; ++i) {
      assert(v[i]->x == 42);
      delete v[i];
    }
  }
  return nullptr;
}

#ifndef NTHR
#define NTHR 10
#endif

static pthread_t Init() {
  pthread_t tid[NTHR];
  for (uintptr_t i = 0; i < NTHR; ++i) {
    pthread_create(&tid[i], nullptr, fn, (void*)i);
  }
  return tid[0];
}

pthread_t ignored_init_result = Init();

}  // namespace

// This is used to pull in this object from archive
// (when built with --dynamic_mode=off).
pthread_t* Func() { return &ignored_init_result; }
