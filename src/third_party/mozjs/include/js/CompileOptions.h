/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Options for JavaScript compilation.
 *
 * In the most common use case, a CompileOptions instance is allocated on the
 * stack, and holds non-owning references to non-POD option values: strings,
 * principals, objects, and so on.  The code declaring the instance guarantees
 * that such option values will outlive the CompileOptions itself: objects are
 * otherwise rooted, principals have had their reference counts bumped, and
 * strings won't be freed until the CompileOptions goes out of scope.  In this
 * situation, CompileOptions only refers to things others own, so it can be
 * lightweight.
 *
 * In some cases, however, we need to hold compilation options with a
 * non-stack-like lifetime.  For example, JS::CompileOffThread needs to save
 * compilation options where a worker thread can find them, then return
 * immediately.  The worker thread will come along at some later point, and use
 * the options.
 *
 * The compiler itself just needs to be able to access a collection of options;
 * it doesn't care who owns them, or what's keeping them alive.  It does its
 * own addrefs/copies/tracing/etc.
 *
 * Furthermore, in some cases compile options are propagated from one entity to
 * another (e.g. from a script to a function defined in that script).  This
 * involves copying over some, but not all, of the options.
 *
 * So we have a class hierarchy that reflects these four use cases:
 *
 * - TransitiveCompileOptions is the common base class, representing options
 *   that should get propagated from a script to functions defined in that
 *   script.  This class is abstract and is only ever used as a subclass.
 *
 * - ReadOnlyCompileOptions is the only subclass of TransitiveCompileOptions,
 *   representing a full set of compile options.  It can be used by code that
 *   simply needs to access options set elsewhere, like the compiler.  This
 *   class too is abstract and is only ever used as a subclass.
 *
 * - The usual CompileOptions class must be stack-allocated, and holds
 *   non-owning references to the filename, element, and so on. It's derived
 *   from ReadOnlyCompileOptions, so the compiler can use it.
 *
 * - OwningCompileOptions roots / copies / reference counts of all its values,
 *   and unroots / frees / releases them when it is destructed. It too is
 *   derived from ReadOnlyCompileOptions, so the compiler accepts it.
 */

#ifndef js_CompileOptions_h
#define js_CompileOptions_h

#include "mozilla/MemoryReporting.h"  // mozilla::MallocSizeOf

#include <stddef.h>  // size_t
#include <stdint.h>  // uint8_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/RootingAPI.h"  // JS::PersistentRooted, JS::Rooted
#include "js/Value.h"

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSObject;
class JS_PUBLIC_API JSScript;
class JS_PUBLIC_API JSString;

namespace JS {

enum class AsmJSOption : uint8_t {
  Enabled,
  DisabledByAsmJSPref,
  DisabledByLinker,
  DisabledByNoWasmCompiler,
  DisabledByDebugger,
};

/**
 * The common base class for the CompileOptions hierarchy.
 *
 * Use this in code that needs to propagate compile options from one
 * compilation unit to another.
 */
class JS_PUBLIC_API TransitiveCompileOptions {
 protected:
  /**
   * The Web Platform allows scripts to be loaded from arbitrary cross-origin
   * sources. This allows an attack by which a malicious website loads a
   * sensitive file (say, a bank statement) cross-origin (using the user's
   * cookies), and sniffs the generated syntax errors (via a window.onerror
   * handler) for juicy morsels of its contents.
   *
   * To counter this attack, HTML5 specifies that script errors should be
   * sanitized ("muted") when the script is not same-origin with the global
   * for which it is loaded. Callers should set this flag for cross-origin
   * scripts, and it will be propagated appropriately to child scripts and
   * passed back in JSErrorReports.
   */
  bool mutedErrors_ = false;

  // Either the Realm configuration or specialized VM operating modes may
  // disallow syntax-parse altogether. These conditions are checked in the
  // CompileOptions constructor.
  bool forceFullParse_ = false;

  // Either the Realm configuration or the compile request may force
  // strict-mode.
  bool forceStrictMode_ = false;

  // The context has specified that source pragmas should be parsed.
  bool sourcePragmas_ = true;

  const char* filename_ = nullptr;
  const char* introducerFilename_ = nullptr;
  const char16_t* sourceMapURL_ = nullptr;

  // Flag used to bypass the filename validation callback.
  // See also SetFilenameValidationCallback.
  bool skipFilenameValidation_ = false;

 public:
  // POD options.
  bool selfHostingMode = false;
  AsmJSOption asmJSOption = AsmJSOption::DisabledByAsmJSPref;
  bool throwOnAsmJSValidationFailureOption = false;
  bool forceAsync = false;
  bool discardSource = false;
  bool sourceIsLazy = false;
  bool allowHTMLComments = true;
  bool hideScriptFromDebugger = false;

  // If set, this script will be hidden from the debugger. The requirement
  // is that once compilation is finished, a call to UpdateDebugMetadata will
  // be made, which will update the SSO with the appropiate debug metadata,
  // and expose the script to the debugger (if hideScriptFromDebugger isn't set)
  bool deferDebugMetadata = false;

  bool hideFromNewScriptInitial() const {
    return deferDebugMetadata || hideScriptFromDebugger;
  }

  bool nonSyntacticScope = false;
  bool privateClassFields = false;
  bool privateClassMethods = false;
  bool topLevelAwait = true;

  bool classStaticBlocks = false;

  // True if transcoding to XDR should use Stencil instead of JSScripts.
  bool useStencilXDR = false;

  // True if off-thread parsing should use a parse GlobalObject in order to
  // directly allocate to the GC from a helper thread. If false, transfer the
  // CompilationStencil back to main thread before allocating GC objects.
  bool useOffThreadParseGlobal = true;

  // When decoding from XDR, borrow ImmutableScriptData from the XDR buffer
  // instead of copying out of it.
  //
  // NOTE: When using this mode, the XDR buffer must live until JS_Shutdown is
  // called. There is currently no mechanism to release the data sooner.
  bool usePinnedBytecode = false;

  /**
   * |introductionType| is a statically allocated C string: one of "eval",
   * "Function", or "GeneratorFunction".
   */
  const char* introductionType = nullptr;

  unsigned introductionLineno = 0;
  uint32_t introductionOffset = 0;
  bool hasIntroductionInfo = false;

  bool useFdlibmForSinCosTan = false;

 protected:
  TransitiveCompileOptions() = default;

  // Set all POD options (those not requiring reference counts, copies,
  // rooting, or other hand-holding) to their values in |rhs|.
  void copyPODTransitiveOptions(const TransitiveCompileOptions& rhs);

 public:
  // Read-only accessors for non-POD options. The proper way to set these
  // depends on the derived type.
  bool mutedErrors() const { return mutedErrors_; }
  bool forceFullParse() const { return forceFullParse_; }
  bool forceStrictMode() const { return forceStrictMode_; }
  bool skipFilenameValidation() const { return skipFilenameValidation_; }
  bool sourcePragmas() const { return sourcePragmas_; }
  const char* filename() const { return filename_; }
  const char* introducerFilename() const { return introducerFilename_; }
  const char16_t* sourceMapURL() const { return sourceMapURL_; }

  TransitiveCompileOptions(const TransitiveCompileOptions&) = delete;
  TransitiveCompileOptions& operator=(const TransitiveCompileOptions&) = delete;
};

/**
 * The class representing a full set of compile options.
 *
 * Use this in code that only needs to access compilation options created
 * elsewhere, like the compiler.  Don't instantiate this class (the constructor
 * is protected anyway); instead, create instances only of the derived classes:
 * CompileOptions and OwningCompileOptions.
 */
class JS_PUBLIC_API ReadOnlyCompileOptions : public TransitiveCompileOptions {
 public:
  // POD options.
  unsigned lineno = 1;
  unsigned column = 0;

  // The offset within the ScriptSource's full uncompressed text of the first
  // character we're presenting for compilation with this CompileOptions.
  //
  // When we compile a lazy script, we pass the compiler only the substring of
  // the source the lazy function occupies. With chunked decompression, we may
  // not even have the complete uncompressed source present in memory. But parse
  // node positions are offsets within the ScriptSource's full text, and
  // BaseScript indicate their substring of the full source by its starting and
  // ending offsets within the full text. This scriptSourceOffset field lets the
  // frontend convert between these offsets and offsets within the substring
  // presented for compilation.
  unsigned scriptSourceOffset = 0;

  // These only apply to non-function scripts.
  bool isRunOnce = false;
  bool noScriptRval = false;

 protected:
  ReadOnlyCompileOptions() = default;

  void copyPODNonTransitiveOptions(const ReadOnlyCompileOptions& rhs);

  ReadOnlyCompileOptions(const ReadOnlyCompileOptions&) = delete;
  ReadOnlyCompileOptions& operator=(const ReadOnlyCompileOptions&) = delete;
};

/**
 * Compilation options, with dynamic lifetime. An instance of this type
 * makes a copy of / holds / roots all dynamically allocated resources
 * (principals; elements; strings) that it refers to. Its destructor frees
 * / drops / unroots them. This is heavier than CompileOptions, below, but
 * unlike CompileOptions, it can outlive any given stack frame.
 *
 * Note that this *roots* any JS values it refers to - they're live
 * unconditionally. Thus, instances of this type can't be owned, directly
 * or indirectly, by a JavaScript object: if any value that this roots ever
 * comes to refer to the object that owns this, then the whole cycle, and
 * anything else it entrains, will never be freed.
 */
class JS_PUBLIC_API OwningCompileOptions final : public ReadOnlyCompileOptions {
 public:
  // A minimal constructor, for use with OwningCompileOptions::copy.
  explicit OwningCompileOptions(JSContext* cx);
  ~OwningCompileOptions();

  /** Set this to a copy of |rhs|.  Return false on OOM. */
  bool copy(JSContext* cx, const ReadOnlyCompileOptions& rhs);

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;

  OwningCompileOptions& setIsRunOnce(bool once) {
    isRunOnce = once;
    return *this;
  }

  OwningCompileOptions& setForceStrictMode() {
    forceStrictMode_ = true;
    return *this;
  }

  OwningCompileOptions& setModule() {
    // ES6 10.2.1 Module code is always strict mode code.
    setForceStrictMode();
    setIsRunOnce(true);
    allowHTMLComments = false;
    return *this;
  }

 private:
  void release();

  OwningCompileOptions(const OwningCompileOptions&) = delete;
  OwningCompileOptions& operator=(const OwningCompileOptions&) = delete;
};

/**
 * Compilation options stored on the stack. An instance of this type
 * simply holds references to dynamically allocated resources (element;
 * filename; source map URL) that are owned by something else. If you
 * create an instance of this type, it's up to you to guarantee that
 * everything you store in it will outlive it.
 */
class MOZ_STACK_CLASS JS_PUBLIC_API CompileOptions final
    : public ReadOnlyCompileOptions {
 public:
  // Default options determined using the JSContext.
  explicit CompileOptions(JSContext* cx);

  // Copy both the transitive and the non-transitive options from another
  // options object.
  CompileOptions(JSContext* cx, const ReadOnlyCompileOptions& rhs)
      : ReadOnlyCompileOptions() {
    copyPODNonTransitiveOptions(rhs);
    copyPODTransitiveOptions(rhs);

    filename_ = rhs.filename();
    introducerFilename_ = rhs.introducerFilename();
    sourceMapURL_ = rhs.sourceMapURL();
  }

  CompileOptions& setFile(const char* f) {
    filename_ = f;
    return *this;
  }

  CompileOptions& setLine(unsigned l) {
    lineno = l;
    return *this;
  }

  CompileOptions& setFileAndLine(const char* f, unsigned l) {
    filename_ = f;
    lineno = l;
    return *this;
  }

  CompileOptions& setSourceMapURL(const char16_t* s) {
    sourceMapURL_ = s;
    return *this;
  }

  CompileOptions& setMutedErrors(bool mute) {
    mutedErrors_ = mute;
    return *this;
  }

  CompileOptions& setColumn(unsigned c) {
    column = c;
    return *this;
  }

  CompileOptions& setScriptSourceOffset(unsigned o) {
    scriptSourceOffset = o;
    return *this;
  }

  CompileOptions& setIsRunOnce(bool once) {
    isRunOnce = once;
    return *this;
  }

  CompileOptions& setNoScriptRval(bool nsr) {
    noScriptRval = nsr;
    return *this;
  }

  CompileOptions& setSkipFilenameValidation(bool b) {
    skipFilenameValidation_ = b;
    return *this;
  }

  CompileOptions& setSelfHostingMode(bool shm) {
    selfHostingMode = shm;
    return *this;
  }

  CompileOptions& setSourceIsLazy(bool l) {
    sourceIsLazy = l;
    return *this;
  }

  CompileOptions& setNonSyntacticScope(bool n) {
    nonSyntacticScope = n;
    return *this;
  }

  CompileOptions& setIntroductionType(const char* t) {
    introductionType = t;
    return *this;
  }

  CompileOptions& setdeferDebugMetadata(bool v = true) {
    deferDebugMetadata = v;
    return *this;
  }

  CompileOptions& setIntroductionInfo(const char* introducerFn,
                                      const char* intro, unsigned line,
                                      uint32_t offset) {
    introducerFilename_ = introducerFn;
    introductionType = intro;
    introductionLineno = line;
    introductionOffset = offset;
    hasIntroductionInfo = true;
    return *this;
  }

  // Set introduction information according to any currently executing script.
  CompileOptions& setIntroductionInfoToCaller(
      JSContext* cx, const char* introductionType,
      JS::MutableHandle<JSScript*> introductionScript);

  CompileOptions& setDiscardSource() {
    discardSource = true;
    return *this;
  }

  CompileOptions& setForceFullParse() {
    forceFullParse_ = true;
    return *this;
  }

  CompileOptions& setForceStrictMode() {
    forceStrictMode_ = true;
    return *this;
  }

  CompileOptions& setModule() {
    // ES6 10.2.1 Module code is always strict mode code.
    setForceStrictMode();
    setIsRunOnce(true);
    allowHTMLComments = false;
    return *this;
  }

  CompileOptions(const CompileOptions& rhs) = delete;
  CompileOptions& operator=(const CompileOptions& rhs) = delete;
};

}  // namespace JS

#endif /* js_CompileOptions_h */
