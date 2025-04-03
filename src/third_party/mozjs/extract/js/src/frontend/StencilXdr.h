/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_StencilXdr_h
#define frontend_StencilXdr_h

#include "mozilla/RefPtr.h"  // RefPtr

#include "frontend/ParserAtom.h"  // ParserAtom, ParserAtomSpan
#include "frontend/Stencil.h"  // BitIntStencil, ScopeStencil, BaseParserScopeData
#include "vm/Xdr.h"            // XDRMode, XDRResult, XDRState

namespace JS {

class DecodeOptions;

}  // namespace JS

namespace js {

class LifoAlloc;
class ObjLiteralStencil;
class ScriptSource;
class SharedImmutableScriptData;

class XDRStencilDecoder;
class XDRStencilEncoder;

namespace frontend {

struct CompilationStencil;
struct ExtensibleCompilationStencil;
struct SharedDataContainer;

// Check that we can copy data to disk and restore it in another instance of
// the program in a different address space.
template <typename DataT>
struct CanCopyDataToDisk {
  // Check that the object is fully packed, to save disk space.
  // Note that we do not use the MSVC std::has_unique_object_representations,
  // because it does not work as desired: it rejects any class that deletes its
  // move constructor as well as any class that deletes both its copy
  // constructor and copy assignment operator.
#if defined(__cpp_lib_has_unique_object_representations) && !defined(_MSC_VER)
  static constexpr bool unique_repr =
      std::has_unique_object_representations<DataT>();
#else
  static constexpr bool unique_repr = true;
#endif

  // Approximation which assumes that 32bits variant of the class would not
  // have pointers if the 64bits variant does not have pointer.
  static constexpr bool no_pointer =
      alignof(DataT) < alignof(void*) || sizeof(void*) == sizeof(uint32_t);

  static constexpr bool value = unique_repr && no_pointer;
};

// This is just a namespace class that can be used in friend declarations,
// so that the statically declared XDR methods within have access to the
// relevant struct internals.
class StencilXDR {
 private:
  template <XDRMode mode>
  [[nodiscard]] static XDRResult codeSourceUnretrievableUncompressed(
      XDRState<mode>* xdr, ScriptSource* ss, uint8_t sourceCharSize,
      uint32_t uncompressedLength);

  template <typename Unit,
            template <typename U, SourceRetrievable CanRetrieve> class Data,
            XDRMode mode>
  static void codeSourceRetrievable(ScriptSource* ss);

  template <typename Unit, XDRMode mode>
  [[nodiscard]] static XDRResult codeSourceUncompressedData(
      XDRState<mode>* const xdr, ScriptSource* const ss);

  template <typename Unit, XDRMode mode>
  [[nodiscard]] static XDRResult codeSourceCompressedData(
      XDRState<mode>* const xdr, ScriptSource* const ss);

  template <typename Unit, XDRMode mode>
  static void codeSourceRetrievableData(ScriptSource* ss);

  template <XDRMode mode>
  [[nodiscard]] static XDRResult codeSourceData(XDRState<mode>* const xdr,
                                                ScriptSource* const ss);

 public:
  template <XDRMode mode>
  static XDRResult codeSource(XDRState<mode>* xdr,
                              const JS::DecodeOptions* maybeOptions,
                              RefPtr<ScriptSource>& source);

  template <XDRMode mode>
  static XDRResult codeBigInt(XDRState<mode>* xdr, LifoAlloc& alloc,
                              BigIntStencil& stencil);

  template <XDRMode mode>
  static XDRResult codeObjLiteral(XDRState<mode>* xdr, LifoAlloc& alloc,
                                  ObjLiteralStencil& stencil);

  template <XDRMode mode>
  static XDRResult codeScopeData(XDRState<mode>* xdr, LifoAlloc& alloc,
                                 ScopeStencil& stencil,
                                 BaseParserScopeData*& baseScopeData);

  template <XDRMode mode>
  static XDRResult codeSharedData(XDRState<mode>* xdr,
                                  RefPtr<SharedImmutableScriptData>& sisd);

  template <XDRMode mode>
  static XDRResult codeSharedDataContainer(XDRState<mode>* xdr,
                                           SharedDataContainer& sharedData);

  template <XDRMode mode>
  static XDRResult codeParserAtom(XDRState<mode>* xdr, LifoAlloc& alloc,
                                  ParserAtom** atomp);

  template <XDRMode mode>
  static XDRResult codeParserAtomSpan(XDRState<mode>* xdr, LifoAlloc& alloc,
                                      ParserAtomSpan& parserAtomData);

  template <XDRMode mode>
  static XDRResult codeModuleRequest(XDRState<mode>* xdr,
                                     StencilModuleRequest& stencil);

  template <XDRMode mode>
  static XDRResult codeModuleRequestVector(
      XDRState<mode>* xdr, StencilModuleMetadata::RequestVector& vector);

  template <XDRMode mode>
  static XDRResult codeModuleEntry(XDRState<mode>* xdr,
                                   StencilModuleEntry& stencil);

  template <XDRMode mode>
  static XDRResult codeModuleEntryVector(
      XDRState<mode>* xdr, StencilModuleMetadata::EntryVector& vector);

  template <XDRMode mode>
  static XDRResult codeModuleMetadata(XDRState<mode>* xdr,
                                      StencilModuleMetadata& stencil);

  static XDRResult checkCompilationStencil(XDRStencilEncoder* encoder,
                                           const CompilationStencil& stencil);

  static XDRResult checkCompilationStencil(
      const ExtensibleCompilationStencil& stencil);

  template <XDRMode mode>
  static XDRResult codeCompilationStencil(XDRState<mode>* xdr,
                                          CompilationStencil& stencil);
};

} /* namespace frontend */

/*
 * The structure of the Stencil XDR buffer is:
 *
 * 1. Version
 * 2. length of content
 * 3. checksum of content
 * 4. content
 *   a. ScriptSource
 *   b. CompilationStencil
 */

/*
 * The stencil decoder accepts `range` as input.
 *
 * The decoded stencils are outputted to the default-initialized
 * `stencil` parameter of `codeStencil` method.
 *
 * The decoded stencils borrow the input `buffer`/`range`, and the consumer
 * has to keep the buffer alive while the decoded stencils are alive.
 */
class XDRStencilDecoder : public XDRState<XDR_DECODE> {
  using Base = XDRState<XDR_DECODE>;

 public:
  XDRStencilDecoder(FrontendContext* fc, const JS::TranscodeRange& range)
      : Base(fc, range) {
    MOZ_ASSERT(JS::IsTranscodingBytecodeAligned(range.begin().get()));
  }

  XDRResult codeStencil(const JS::DecodeOptions& options,
                        frontend::CompilationStencil& stencil);

  const JS::DecodeOptions& options() {
    MOZ_ASSERT(options_);
    return *options_;
  }

 private:
  const JS::DecodeOptions* options_ = nullptr;
};

class XDRStencilEncoder : public XDRState<XDR_ENCODE> {
  using Base = XDRState<XDR_ENCODE>;

 public:
  XDRStencilEncoder(FrontendContext* fc, JS::TranscodeBuffer& buffer)
      : Base(fc, buffer, buffer.length()) {
    // NOTE: If buffer is empty, buffer.begin() doesn't point valid buffer.
    MOZ_ASSERT_IF(!buffer.empty(),
                  JS::IsTranscodingBytecodeAligned(buffer.begin()));
    MOZ_ASSERT(JS::IsTranscodingBytecodeOffsetAligned(buffer.length()));
  }

  XDRResult codeStencil(const RefPtr<ScriptSource>& source,
                        const frontend::CompilationStencil& stencil);

  XDRResult codeStencil(const frontend::CompilationStencil& stencil);
};

} /* namespace js */

#endif /* frontend_StencilXdr_h */
