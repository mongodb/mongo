/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_CacheIRSpewer_h
#define jit_CacheIRSpewer_h

#ifdef JS_CACHEIR_SPEW

#  include "mozilla/Maybe.h"

#  include "jit/CacheIR.h"
#  include "jit/CacheIRGenerator.h"
#  include "jit/CacheIRReader.h"
#  include "jit/CacheIRWriter.h"
#  include "js/TypeDecls.h"
#  include "threading/LockGuard.h"
#  include "vm/JSONPrinter.h"
#  include "vm/MutexIDs.h"

namespace js {
namespace jit {

class CacheIRSpewer {
  Mutex outputLock_ MOZ_UNANNOTATED;
  Fprinter output_;
  mozilla::Maybe<JSONPrinter> json_;
  static CacheIRSpewer cacheIRspewer;

  // Counter to record how many times Guard class is called. This is used to
  // determine when to flush outputs based on the given interval value.
  // For example, if |spewInterval_ = 2|, outputs will be flushed on
  // guardCount_ values 0,2,4,6,...
  uint32_t guardCount_;

  // Interval at which to flush output files. This value can be set with the
  // environment variable |CACHEIR_LOG_FLUSH|.
  uint32_t spewInterval_;

  CacheIRSpewer();
  ~CacheIRSpewer();

  bool enabled() { return json_.isSome(); }

  // These methods can only be called when enabled() is true.
  Mutex& lock() {
    MOZ_ASSERT(enabled());
    return outputLock_;
  }

  void beginCache(const IRGenerator& generator);
  void valueProperty(const char* name, const Value& v);
  void opcodeProperty(const char* name, const JSOp op);
  void jstypeProperty(const char* name, const JSType type);
  void cacheIRSequence(CacheIRReader& reader);
  void attached(const char* name);
  void endCache();

 public:
  static CacheIRSpewer& singleton() { return cacheIRspewer; }
  bool init(const char* name);

  class MOZ_RAII Guard {
    CacheIRSpewer& sp_;
    const IRGenerator& gen_;
    const char* name_;

   public:
    Guard(const IRGenerator& gen, const char* name)
        : sp_(CacheIRSpewer::singleton()), gen_(gen), name_(name) {
      if (sp_.enabled()) {
        sp_.lock().lock();
        sp_.beginCache(gen_);
      }
    }

    ~Guard() {
      if (sp_.enabled()) {
        const CacheIRWriter& writer = gen_.writerRef();
        if (!writer.failed() && writer.codeLength() > 0) {
          CacheIRReader reader(writer);
          sp_.cacheIRSequence(reader);
        }
        if (name_ != nullptr) {
          sp_.attached(name_);
        }
        sp_.endCache();
        if (sp_.guardCount_++ % sp_.spewInterval_ == 0) {
          sp_.output_.flush();
        }
        sp_.lock().unlock();
      }
    }

    void valueProperty(const char* name, const Value& v) const {
      sp_.valueProperty(name, v);
    }

    void opcodeProperty(const char* name, const JSOp op) const {
      sp_.opcodeProperty(name, op);
    }

    void jstypeProperty(const char* name, const JSType type) const {
      sp_.jstypeProperty(name, type);
    }

    explicit operator bool() const { return sp_.enabled(); }
  };
};

extern void SpewCacheIROps(GenericPrinter& out, const char* prefix,
                           const CacheIRStubInfo* info);

}  // namespace jit
}  // namespace js

#endif /* JS_CACHEIR_SPEW */

#endif /* jit_CacheIRSpewer_h */
