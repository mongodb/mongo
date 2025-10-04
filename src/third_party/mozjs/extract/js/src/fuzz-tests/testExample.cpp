/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/ScopeExit.h"

#include "jsapi.h"

#include "fuzz-tests/tests.h"
#include "vm/Interpreter.h"

#include "vm/JSContext-inl.h"

using namespace JS;
using namespace js;

extern JS::PersistentRootedObject gGlobal;
extern JSContext* gCx;

static int testExampleInit(int* argc, char*** argv) {
  /* This function is called once at startup. You can use it to e.g. read
     environment variables to initialize additional options you might need.
     Note that `gCx` and `gGlobal` are pre-initialized by the harness.
  */
  return 0;
}

static int testExampleFuzz(const uint8_t* buf, size_t size) {
  /* If your code directly or indirectly allocates GC memory, then it makes
     sense to attempt and collect that after every iteration. This should detect
     GC issues as soon as possible (right after your iteration), rather than
     later when your code happens to trigger GC coincidentially. You can of
     course disable this code
     if it is not required in your use case, which will speed up fuzzing. */
  auto gcGuard = mozilla::MakeScopeExit([&] {
    JS::PrepareForFullGC(gCx);
    JS::NonIncrementalGC(gCx, JS::GCOptions::Normal, JS::GCReason::API);
  });

  /* Add code here that processes the given buffer.
     While doing so, you need to follow these rules:

     1. Do not modify or free the buffer. Make a copy if necessary.
     2. This function must always return 0.
     3. Do not crash or abort unless the condition constitutes a bug.
     4. You may use the `gGlobal` and `gCx` variables, they are pre-initialized.
     5. Try to keep the effects of this function contained, such that future
        calls to this function are not affected. Otherwise you end up with
        non-reproducible testcases and coverage measurements will be incorrect.
  */

  return 0;
}

MOZ_FUZZING_INTERFACE_RAW(testExampleInit, /* init function */
                          testExampleFuzz, /* fuzzing function */
                          Example          /* module name */
);
