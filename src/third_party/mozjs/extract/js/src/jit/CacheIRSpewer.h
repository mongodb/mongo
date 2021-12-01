/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_CacheIRSpewer_h
#define jit_CacheIRSpewer_h

#ifdef JS_CACHEIR_SPEW

#include "mozilla/Maybe.h"

#include "jit/CacheIR.h"
#include "js/TypeDecls.h"
#include "threading/LockGuard.h"
#include "vm/JSONPrinter.h"
#include "vm/MutexIDs.h"

namespace js {
namespace jit {

class CacheIRSpewer
{
    Mutex outputLock;
    Fprinter output;
    mozilla::Maybe<JSONPrinter> json;
    static CacheIRSpewer cacheIRspewer;

    CacheIRSpewer();
    ~CacheIRSpewer();

    bool enabled() { return json.isSome(); }

    // These methods can only be called when enabled() is true.
    Mutex& lock() { MOZ_ASSERT(enabled()); return outputLock; }

    void beginCache(const IRGenerator& generator);
    void valueProperty(const char* name, const Value& v);
    void attached(const char* name);
    void endCache();

  public:
    static CacheIRSpewer& singleton() { return cacheIRspewer; }
    bool init();

    class MOZ_RAII Guard {
        CacheIRSpewer& sp_;
        const IRGenerator& gen_;
        const char* name_;

      public:
        Guard(const IRGenerator& gen, const char* name)
          : sp_(CacheIRSpewer::singleton()),
            gen_(gen),
            name_(name)
        {
          if (sp_.enabled()) {
            sp_.lock().lock();
            sp_.beginCache(gen_);
          }
        }

        ~Guard() {
          if (sp_.enabled()) {
            if (name_ != nullptr)
              sp_.attached(name_);
            sp_.endCache();
            sp_.lock().unlock();
          }
        }

        void valueProperty(const char* name, const Value& v) const {
          sp_.valueProperty(name, v);
        }

        explicit operator bool() const {
          return sp_.enabled();
        }
    };
};

} // namespace jit
} // namespace js

#endif /* JS_CACHEIR_SPEW */

#endif /* jit_CacheIRSpewer_h */
