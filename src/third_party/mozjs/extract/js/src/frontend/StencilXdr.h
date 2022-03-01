/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_StencilXdr_h
#define frontend_StencilXdr_h

#include "mozilla/RefPtr.h"  // RefPtr

#include "frontend/CompilationStencil.h"  // SharedDataContainer
#include "frontend/ObjLiteral.h"          // ObjLiteralStencil
#include "frontend/ParserAtom.h"          // ParserAtom, ParserAtomSpan
#include "frontend/Stencil.h"  // BitIntStencil, ScopeStencil, BaseParserScopeData
#include "vm/SharedStencil.h"  // SharedImmutableScriptData
#include "vm/Xdr.h"            // XDRMode, XDRResult, XDRState

namespace js {

class LifoAlloc;

namespace frontend {

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
 public:
  template <XDRMode mode>
  static XDRResult codeBigInt(XDRState<mode>* xdr, BigIntStencil& stencil);

  template <XDRMode mode>
  static XDRResult codeObjLiteral(XDRState<mode>* xdr,
                                  ObjLiteralStencil& stencil);

  template <XDRMode mode>
  static XDRResult codeScopeData(XDRState<mode>* xdr, ScopeStencil& stencil,
                                 BaseParserScopeData*& baseScopeData);

  template <XDRMode mode>
  static XDRResult codeSharedData(XDRState<mode>* xdr,
                                  RefPtr<SharedImmutableScriptData>& sisd);

  template <XDRMode mode>
  static XDRResult codeSharedDataContainer(XDRState<mode>* xdr,
                                           SharedDataContainer& sharedData);

  template <XDRMode mode>
  static XDRResult codeParserAtom(XDRState<mode>* xdr, ParserAtom** atomp);

  template <XDRMode mode>
  static XDRResult codeParserAtomSpan(XDRState<mode>* xdr, LifoAlloc& alloc,
                                      ParserAtomSpan& parserAtomData);

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
} /* namespace js */

#endif /* frontend_StencilXdr_h */
