/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_WeakMapPtr_h
#define js_WeakMapPtr_h

#include "jspubtd.h"

#include "js/TypeDecls.h"

namespace JS {

// A wrapper around the internal C++ representation of SpiderMonkey WeakMaps,
// usable outside the engine.
//
// The supported template specializations are enumerated in gc/WeakMapPtr.cpp.
// If you want to use this class for a different key/value combination, add it
// to the list and the compiler will generate the relevant machinery.
template <typename K, typename V>
class JS_PUBLIC_API(WeakMapPtr)
{
  public:
    WeakMapPtr() : ptr(nullptr) {}
    bool init(JSContext* cx);
    bool initialized() { return ptr != nullptr; }
    void destroy();
    virtual ~WeakMapPtr() { MOZ_ASSERT(!initialized()); }
    void trace(JSTracer* tracer);

    V lookup(const K& key);
    bool put(JSContext* cx, const K& key, const V& value);
    V removeValue(const K& key);

  private:
    void* ptr;

    // WeakMapPtr is neither copyable nor assignable.
    WeakMapPtr(const WeakMapPtr& wmp) = delete;
    WeakMapPtr& operator=(const WeakMapPtr& wmp) = delete;
};

} /* namespace JS */

#endif  /* js_WeakMapPtr_h */
