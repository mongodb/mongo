/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "fuzz-tests/tests.h"

#include <stdio.h>

#include "js/AllocPolicy.h"
#include "js/GlobalObject.h"
#include "js/Initialization.h"
#include "js/RootingAPI.h"
#include "js/Stack.h"
#include "vm/JSContext.h"

#ifdef LIBFUZZER
#  include "FuzzerDefs.h"
#endif

using namespace mozilla;

JS::PersistentRootedObject gGlobal;
JSContext* gCx = nullptr;

static const JSClass* getGlobalClass() {
  static const JSClass c = {"global", JSCLASS_GLOBAL_FLAGS,
                            &JS::DefaultGlobalClassOps};
  return &c;
}

static JSObject* jsfuzz_createGlobal(JSContext* cx, JSPrincipals* principals) {
  /* Create the global object. */
  JS::RealmOptions options;
  options.creationOptions().setWeakRefsEnabled(
      JS::WeakRefSpecifier::EnabledWithCleanupSome);
  return JS_NewGlobalObject(cx, getGlobalClass(), principals,
                            JS::FireOnNewGlobalHook, options);
}

static bool jsfuzz_init(JSContext** cx, JS::PersistentRootedObject* global) {
  *cx = JS_NewContext(8L * 1024 * 1024);
  if (!*cx) {
    return false;
  }

  const size_t MAX_STACK_SIZE = 500000;

  JS_SetNativeStackQuota(*cx, MAX_STACK_SIZE);

  js::UseInternalJobQueues(*cx);
  if (!JS::InitSelfHostedCode(*cx)) {
    return false;
  }
  global->init(*cx);
  *global = jsfuzz_createGlobal(*cx, nullptr);
  if (!*global) {
    return false;
  }
  JS::EnterRealm(*cx, *global);
  return true;
}

static void jsfuzz_uninit(JSContext* cx) {
  if (cx) {
    JS::LeaveRealm(cx, nullptr);
    JS_DestroyContext(cx);
    cx = nullptr;
  }
}

int main(int argc, char* argv[]) {
  if (!JS_Init()) {
    fprintf(stderr, "Error: Call to jsfuzz_init() failed\n");
    return 1;
  }

  if (!jsfuzz_init(&gCx, &gGlobal)) {
    fprintf(stderr, "Error: Call to jsfuzz_init() failed\n");
    return 1;
  }

  const char* fuzzerEnv = getenv("FUZZER");
  if (!fuzzerEnv) {
    fprintf(stderr,
            "Must specify fuzzing target in FUZZER environment variable\n");
    return 1;
  }

  std::string moduleNameStr(getenv("FUZZER"));

  FuzzerFunctions funcs =
      FuzzerRegistry::getInstance().getModuleFunctions(moduleNameStr);
  FuzzerInitFunc initFunc = funcs.first;
  FuzzerTestingFunc testingFunc = funcs.second;
  if (initFunc) {
    int ret = initFunc(&argc, &argv);
    if (ret) {
      fprintf(stderr, "Fuzzing Interface: Error: Initialize callback failed\n");
      return ret;
    }
  }

  if (!testingFunc) {
    fprintf(stderr, "Fuzzing Interface: Error: No testing callback found\n");
    return 1;
  }

#ifdef LIBFUZZER
  fuzzer::FuzzerDriver(&argc, &argv, testingFunc);
#elif AFLFUZZ
  testingFunc(nullptr, 0);
#endif

  jsfuzz_uninit(gCx);

  JS_ShutDown();

  return 0;
}
