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

#include "mozilla/Assertions.h"       // MOZ_ASSERT
#include "mozilla/MemoryReporting.h"  // mozilla::MallocSizeOf

#include <stddef.h>  // size_t
#include <stdint.h>  // uint8_t, uint32_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/TypeDecls.h"  // JS::MutableHandle (fwd)

namespace JS {

enum class AsmJSOption : uint8_t {
  Enabled,
  DisabledByAsmJSPref,
  DisabledByLinker,
  DisabledByNoWasmCompiler,
  DisabledByDebugger,
};

#define FOREACH_DELAZIFICATION_STRATEGY(_)                                     \
  /* Do not delazify anything eagerly. */                                      \
  _(OnDemandOnly)                                                              \
                                                                               \
  /*                                                                           \
   * Compare the stencil produced by concurrent depth first delazification and \
   * on-demand delazification. Any differences would crash SpiderMonkey with   \
   * an assertion.                                                             \
   */                                                                          \
  _(CheckConcurrentWithOnDemand)                                               \
                                                                               \
  /*                                                                           \
   * Delazifiy functions in a depth first traversal of the functions.          \
   */                                                                          \
  _(ConcurrentDepthFirst)                                                      \
                                                                               \
  /*                                                                           \
   * Delazifiy functions strating with the largest function first.             \
   */                                                                          \
  _(ConcurrentLargeFirst)                                                      \
                                                                               \
  /*                                                                           \
   * Parse everything eagerly, from the first parse.                           \
   *                                                                           \
   * NOTE: Either the Realm configuration or specialized VM operating modes    \
   * may disallow syntax-parse altogether. These conditions are checked in the \
   * CompileOptions constructor.                                               \
   */                                                                          \
  _(ParseEverythingEagerly)

enum class DelazificationOption : uint8_t {
#define _ENUM_ENTRY(Name) Name,
  FOREACH_DELAZIFICATION_STRATEGY(_ENUM_ENTRY)
#undef _ENUM_ENTRY
};

class JS_PUBLIC_API InstantiateOptions;
class JS_PUBLIC_API DecodeOptions;

/**
 * The common base class for the CompileOptions hierarchy.
 *
 * Use this in code that needs to propagate compile options from one
 * compilation unit to another.
 */
class JS_PUBLIC_API TransitiveCompileOptions {
  friend class JS_PUBLIC_API DecodeOptions;

 protected:
  // non-POD options:

  // UTF-8 encoded file name.
  const char* filename_ = nullptr;

  // UTF-8 encoded introducer file name.
  const char* introducerFilename_ = nullptr;

  const char16_t* sourceMapURL_ = nullptr;

  // POD options:
  // WARNING: When adding new fields, don't forget to add them to
  //          copyPODTransitiveOptions.

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

  // Either the Realm configuration or the compile request may force
  // strict-mode.
  bool forceStrictMode_ = false;

  // The Realm of this script is configured to resist fingerprinting.
  bool shouldResistFingerprinting_ = false;

  // The context has specified that source pragmas should be parsed.
  bool sourcePragmas_ = true;

  // Flag used to bypass the filename validation callback.
  // See also SetFilenameValidationCallback.
  bool skipFilenameValidation_ = false;

  bool hideScriptFromDebugger_ = false;

  // If set, this script will be hidden from the debugger. The requirement
  // is that once compilation is finished, a call to UpdateDebugMetadata will
  // be made, which will update the SSO with the appropiate debug metadata,
  // and expose the script to the debugger (if hideScriptFromDebugger_ isn't
  // set)
  bool deferDebugMetadata_ = false;

  // Off-thread delazification strategy is used to tell off-thread tasks how the
  // delazification should be performed. Multiple strategies are available in
  // order to test different approaches to the concurrent delazification.
  DelazificationOption eagerDelazificationStrategy_ =
      DelazificationOption::OnDemandOnly;

  friend class JS_PUBLIC_API InstantiateOptions;

 public:
  bool selfHostingMode = false;
  AsmJSOption asmJSOption = AsmJSOption::DisabledByAsmJSPref;
  bool throwOnAsmJSValidationFailureOption = false;
  bool forceAsync = false;
  bool discardSource = false;
  bool sourceIsLazy = false;
  bool allowHTMLComments = true;
  bool nonSyntacticScope = false;

  // Top-level await is enabled by default but is not supported for chrome
  // modules loaded with ChromeUtils.importModule.
  bool topLevelAwait = true;

  bool importAssertions = false;

  // When decoding from XDR into a Stencil, directly reference data in the
  // buffer (where possible) instead of copying it. This is an optional
  // performance optimization, and may also reduce memory if the buffer is going
  // remain alive anyways.
  //
  // NOTE: The XDR buffer must remain alive as long as the Stencil does. Special
  //       care must be taken that there are no addition shared references to
  //       the Stencil.
  //
  // NOTE: Instantiated GC things may still outlive the buffer as long as the
  //       Stencil was cleaned up. This is covers a typical case where a decoded
  //       Stencil is instantiated once and then thrown away.
  bool borrowBuffer = false;

  // Similar to `borrowBuffer`, but additionally the JSRuntime may directly
  // reference data in the buffer for JS bytecode. The `borrowBuffer` flag must
  // be set if this is set. This can be a memory optimization in multi-process
  // architectures where a (read-only) XDR buffer is mapped into multiple
  // processes.
  //
  // NOTE: When using this mode, the XDR buffer must live until JS_Shutdown is
  // called. There is currently no mechanism to release the data sooner.
  bool usePinnedBytecode = false;

  // When performing off-thread task that generates JS::Stencil as output,
  // allocate JS::InstantiationStorage off main thread to reduce the
  // main thread allocation.
  bool allocateInstantiationStorage = false;

  // De-optimize ES module's top-level `var`s, in order to define all of them
  // on the ModuleEnvironmentObject, instead of local slot.
  //
  // This is used for providing all global variables in Cu.import return value
  // (see bug 1766761 for more details), and this is temporary solution until
  // ESM-ification finishes.
  //
  // WARNING: This option will eventually be removed.
  bool deoptimizeModuleGlobalVars = false;

  /**
   * |introductionType| is a statically allocated C string. See JSScript.h
   * for more information.
   */
  const char* introductionType = nullptr;

  unsigned introductionLineno = 0;
  uint32_t introductionOffset = 0;
  bool hasIntroductionInfo = false;

  // WARNING: When adding new fields, don't forget to add them to
  //          copyPODTransitiveOptions.

 protected:
  TransitiveCompileOptions() = default;

  // Set all POD options (those not requiring reference counts, copies,
  // rooting, or other hand-holding) to their values in |rhs|.
  void copyPODTransitiveOptions(const TransitiveCompileOptions& rhs);

  bool isEagerDelazificationEqualTo(DelazificationOption val) const {
    return eagerDelazificationStrategy() == val;
  }

  template <DelazificationOption... Values>
  bool eagerDelazificationIsOneOf() const {
    return (isEagerDelazificationEqualTo(Values) || ...);
  }

 public:
  // Read-only accessors for non-POD options. The proper way to set these
  // depends on the derived type.
  bool mutedErrors() const { return mutedErrors_; }
  bool shouldResistFingerprinting() const {
    return shouldResistFingerprinting_;
  }
  bool forceFullParse() const {
    return eagerDelazificationIsOneOf<
        DelazificationOption::ParseEverythingEagerly>();
  }
  bool forceStrictMode() const { return forceStrictMode_; }
  bool consumeDelazificationCache() const {
    return eagerDelazificationIsOneOf<
        DelazificationOption::ConcurrentDepthFirst,
        DelazificationOption::ConcurrentLargeFirst>();
  }
  bool populateDelazificationCache() const {
    return eagerDelazificationIsOneOf<
        DelazificationOption::CheckConcurrentWithOnDemand,
        DelazificationOption::ConcurrentDepthFirst,
        DelazificationOption::ConcurrentLargeFirst>();
  }
  bool waitForDelazificationCache() const {
    return eagerDelazificationIsOneOf<
        DelazificationOption::CheckConcurrentWithOnDemand>();
  }
  bool checkDelazificationCache() const {
    return eagerDelazificationIsOneOf<
        DelazificationOption::CheckConcurrentWithOnDemand>();
  }
  DelazificationOption eagerDelazificationStrategy() const {
    return eagerDelazificationStrategy_;
  }
  bool sourcePragmas() const { return sourcePragmas_; }
  const char* filename() const { return filename_; }
  const char* introducerFilename() const { return introducerFilename_; }
  const char16_t* sourceMapURL() const { return sourceMapURL_; }

  TransitiveCompileOptions(const TransitiveCompileOptions&) = delete;
  TransitiveCompileOptions& operator=(const TransitiveCompileOptions&) = delete;

#if defined(DEBUG) || defined(JS_JITSPEW)
  template <typename Printer>
  void dumpWith(Printer& print) const {
#  define PrintFields_(Name) print(#Name, Name)
    PrintFields_(filename_);
    PrintFields_(introducerFilename_);
    PrintFields_(sourceMapURL_);
    PrintFields_(mutedErrors_);
    PrintFields_(forceStrictMode_);
    PrintFields_(shouldResistFingerprinting_);
    PrintFields_(sourcePragmas_);
    PrintFields_(skipFilenameValidation_);
    PrintFields_(hideScriptFromDebugger_);
    PrintFields_(deferDebugMetadata_);
    PrintFields_(eagerDelazificationStrategy_);
    PrintFields_(selfHostingMode);
    PrintFields_(asmJSOption);
    PrintFields_(throwOnAsmJSValidationFailureOption);
    PrintFields_(forceAsync);
    PrintFields_(discardSource);
    PrintFields_(sourceIsLazy);
    PrintFields_(allowHTMLComments);
    PrintFields_(nonSyntacticScope);
    PrintFields_(topLevelAwait);
    PrintFields_(importAssertions);
    PrintFields_(borrowBuffer);
    PrintFields_(usePinnedBytecode);
    PrintFields_(allocateInstantiationStorage);
    PrintFields_(deoptimizeModuleGlobalVars);
    PrintFields_(introductionType);
    PrintFields_(introductionLineno);
    PrintFields_(introductionOffset);
    PrintFields_(hasIntroductionInfo);
#  undef PrintFields_
  }
#endif  // defined(DEBUG) || defined(JS_JITSPEW)
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

 public:
#if defined(DEBUG) || defined(JS_JITSPEW)
  template <typename Printer>
  void dumpWith(Printer& print) const {
    this->TransitiveCompileOptions::dumpWith(print);
#  define PrintFields_(Name) print(#Name, Name)
    PrintFields_(lineno);
    PrintFields_(column);
    PrintFields_(scriptSourceOffset);
    PrintFields_(isRunOnce);
    PrintFields_(noScriptRval);
#  undef PrintFields_
  }
#endif  // defined(DEBUG) || defined(JS_JITSPEW)
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

  // Construct CompileOptions for FrontendContext-APIs.
  struct ForFrontendContext {};
  explicit CompileOptions(const ForFrontendContext&)
      : ReadOnlyCompileOptions() {}

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

  CompileOptions& setDeferDebugMetadata(bool v = true) {
    deferDebugMetadata_ = v;
    return *this;
  }

  CompileOptions& setHideScriptFromDebugger(bool v = true) {
    hideScriptFromDebugger_ = v;
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
    eagerDelazificationStrategy_ = DelazificationOption::ParseEverythingEagerly;
    return *this;
  }

  CompileOptions& setEagerDelazificationStrategy(
      DelazificationOption strategy) {
    // forceFullParse is at the moment considered as a non-overridable strategy.
    MOZ_RELEASE_ASSERT(eagerDelazificationStrategy_ !=
                           DelazificationOption::ParseEverythingEagerly ||
                       strategy ==
                           DelazificationOption::ParseEverythingEagerly);
    eagerDelazificationStrategy_ = strategy;
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

/**
 * Subset of CompileOptions fields used while instantiating Stencils.
 */
class JS_PUBLIC_API InstantiateOptions {
 public:
  bool skipFilenameValidation = false;
  bool hideScriptFromDebugger = false;
  bool deferDebugMetadata = false;

  InstantiateOptions() = default;

  explicit InstantiateOptions(const ReadOnlyCompileOptions& options)
      : skipFilenameValidation(options.skipFilenameValidation_),
        hideScriptFromDebugger(options.hideScriptFromDebugger_),
        deferDebugMetadata(options.deferDebugMetadata_) {}

  void copyTo(CompileOptions& options) const {
    options.skipFilenameValidation_ = skipFilenameValidation;
    options.hideScriptFromDebugger_ = hideScriptFromDebugger;
    options.deferDebugMetadata_ = deferDebugMetadata;
  }

  bool hideFromNewScriptInitial() const {
    return deferDebugMetadata || hideScriptFromDebugger;
  }

#ifdef DEBUG
  // Assert that all fields have default value.
  //
  // This can be used when instantiation is performed as separate step than
  // compile-to-stencil, and CompileOptions isn't available there.
  void assertDefault() const {
    MOZ_ASSERT(skipFilenameValidation == false);
    MOZ_ASSERT(hideScriptFromDebugger == false);
    MOZ_ASSERT(deferDebugMetadata == false);
  }
#endif
};

/**
 * Subset of CompileOptions fields used while decoding Stencils.
 */
class JS_PUBLIC_API DecodeOptions {
 public:
  bool borrowBuffer = false;
  bool usePinnedBytecode = false;
  bool allocateInstantiationStorage = false;
  bool forceAsync = false;

  const char* introducerFilename = nullptr;

  // See `TransitiveCompileOptions::introductionType` field for details.
  const char* introductionType = nullptr;

  unsigned introductionLineno = 0;
  uint32_t introductionOffset = 0;

  DecodeOptions() = default;

  explicit DecodeOptions(const ReadOnlyCompileOptions& options)
      : borrowBuffer(options.borrowBuffer),
        usePinnedBytecode(options.usePinnedBytecode),
        allocateInstantiationStorage(options.allocateInstantiationStorage),
        forceAsync(options.forceAsync),
        introducerFilename(options.introducerFilename()),
        introductionType(options.introductionType),
        introductionLineno(options.introductionLineno),
        introductionOffset(options.introductionOffset) {}

  void copyTo(CompileOptions& options) const {
    options.borrowBuffer = borrowBuffer;
    options.usePinnedBytecode = usePinnedBytecode;
    options.allocateInstantiationStorage = allocateInstantiationStorage;
    options.forceAsync = forceAsync;
    options.introducerFilename_ = introducerFilename;
    options.introductionType = introductionType;
    options.introductionLineno = introductionLineno;
    options.introductionOffset = introductionOffset;
  }
};

}  // namespace JS

#endif /* js_CompileOptions_h */
