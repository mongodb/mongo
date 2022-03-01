/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/StencilXdr.h"  // StencilXDR

#include "mozilla/OperatorNewExtensions.h"  // mozilla::KnownNotNull
#include "mozilla/Variant.h"                // mozilla::AsVariant

#include <stddef.h>     // size_t
#include <stdint.h>     // uint8_t, uint16_t, uint32_t
#include <type_traits>  // std::has_unique_object_representations
#include <utility>      // std::forward

#include "ds/LifoAlloc.h"                  // LifoAlloc
#include "frontend/BytecodeCompilation.h"  // CanLazilyParse
#include "frontend/CompilationStencil.h"  // CompilationStencil, ExtensibleCompilationStencil
#include "frontend/ScriptIndex.h"  // ScriptIndex
#include "vm/ErrorReporting.h"     // ErrorMetadata, ReportCompileErrorUTF8
#include "vm/Scope.h"              // SizeOfParserScopeData
#include "vm/StencilEnums.h"       // js::ImmutableScriptFlagsEnum

using namespace js;
using namespace js::frontend;

template <typename NameType>
struct CanEncodeNameType {
  static constexpr bool value = false;
};

template <>
struct CanEncodeNameType<TaggedParserAtomIndex> {
  static constexpr bool value = true;
};

template <XDRMode mode, typename T, size_t N, class AP>
static XDRResult XDRVectorUninitialized(XDRState<mode>* xdr,
                                        Vector<T, N, AP>& vec,
                                        uint32_t& length) {
  if (mode == XDR_ENCODE) {
    MOZ_ASSERT(vec.length() <= UINT32_MAX);
    length = vec.length();
  }

  MOZ_TRY(xdr->codeUint32(&length));

  if (mode == XDR_DECODE) {
    MOZ_ASSERT(vec.empty());
    if (!vec.resizeUninitialized(length)) {
      js::ReportOutOfMemory(xdr->cx());
      return xdr->fail(JS::TranscodeResult::Throw);
    }
  }

  return Ok();
}

template <XDRMode mode, typename T, size_t N, class AP>
static XDRResult XDRVectorInitialized(XDRState<mode>* xdr,
                                      Vector<T, N, AP>& vec, uint32_t length) {
  MOZ_ASSERT_IF(mode == XDR_ENCODE, length == vec.length());

  if (mode == XDR_DECODE) {
    MOZ_ASSERT(vec.empty());
    if (!vec.resize(length)) {
      js::ReportOutOfMemory(xdr->cx());
      return xdr->fail(JS::TranscodeResult::Throw);
    }
  }

  return Ok();
}

template <XDRMode mode, typename T, size_t N, class AP>
static XDRResult XDRVectorInitialized(XDRState<mode>* xdr,
                                      Vector<T, N, AP>& vec) {
  uint32_t length;
  if (mode == XDR_ENCODE) {
    MOZ_ASSERT(vec.length() <= UINT32_MAX);
    length = vec.length();
  }

  MOZ_TRY(xdr->codeUint32(&length));

  return XDRVectorInitialized(xdr, vec, length);
}

template <XDRMode mode, typename T, size_t N, class AP>
static XDRResult XDRVectorContent(XDRState<mode>* xdr, Vector<T, N, AP>& vec) {
  static_assert(CanCopyDataToDisk<T>::value,
                "Vector content cannot be bulk-copied to disk.");

  uint32_t length;
  MOZ_TRY(XDRVectorUninitialized(xdr, vec, length));
  MOZ_TRY(xdr->codeBytes(vec.begin(), sizeof(T) * length));

  return Ok();
}

template <XDRMode mode, typename T>
static XDRResult XDRSpanInitialized(XDRState<mode>* xdr, LifoAlloc& alloc,
                                    mozilla::Span<T>& span, uint32_t size) {
  MOZ_ASSERT_IF(mode == XDR_ENCODE, size == span.size());

  if (mode == XDR_DECODE) {
    MOZ_ASSERT(span.empty());
    if (size > 0) {
      auto* p = alloc.template newArrayUninitialized<T>(size);
      if (!p) {
        js::ReportOutOfMemory(xdr->cx());
        return xdr->fail(JS::TranscodeResult::Throw);
      }
      span = mozilla::Span(p, size);

      for (size_t i = 0; i < size; i++) {
        new (mozilla::KnownNotNull, &span[i]) T();
      }
    }
  }

  return Ok();
}

template <XDRMode mode, typename T>
static XDRResult XDRSpanContent(XDRState<mode>* xdr, mozilla::Span<T>& span,
                                uint32_t size) {
  static_assert(CanCopyDataToDisk<T>::value,
                "Span cannot be bulk-copied to disk.");
  MOZ_ASSERT_IF(mode == XDR_ENCODE, size == span.size());

  if (size) {
    MOZ_TRY(xdr->align32());

    T* data;
    if (mode == XDR_ENCODE) {
      data = span.data();
    }
    MOZ_TRY(xdr->borrowedData(&data, sizeof(T) * size));
    if (mode == XDR_DECODE) {
      span = mozilla::Span(data, size);
    }
  }

  return Ok();
}

template <XDRMode mode, typename T>
static XDRResult XDRSpanContent(XDRState<mode>* xdr, mozilla::Span<T>& span) {
  uint32_t size;
  if (mode == XDR_ENCODE) {
    MOZ_ASSERT(span.size() <= UINT32_MAX);
    size = span.size();
  }

  MOZ_TRY(xdr->codeUint32(&size));

  return XDRSpanContent(xdr, span, size);
}

template <XDRMode mode>
/* static */ XDRResult StencilXDR::codeBigInt(XDRState<mode>* xdr,
                                              BigIntStencil& stencil) {
  uint32_t size;
  if (mode == XDR_ENCODE) {
    size = stencil.source_.size();
  }
  MOZ_TRY(xdr->codeUint32(&size));

  return XDRSpanContent(xdr, stencil.source_, size);
}

template <XDRMode mode>
/* static */ XDRResult StencilXDR::codeObjLiteral(XDRState<mode>* xdr,
                                                  ObjLiteralStencil& stencil) {
  uint8_t flags = 0;

  if (mode == XDR_ENCODE) {
    flags = stencil.flags_.toRaw();
  }
  MOZ_TRY(xdr->codeUint8(&flags));
  if (mode == XDR_DECODE) {
    stencil.flags_.setRaw(flags);
  }

  MOZ_TRY(xdr->codeUint32(&stencil.propertyCount_));

  MOZ_TRY(XDRSpanContent(xdr, stencil.code_));

  return Ok();
}

template <typename ScopeT>
/* static */ void AssertScopeSpecificDataIsEncodable() {
  using ScopeDataT = typename ScopeT::ParserData;

  static_assert(CanEncodeNameType<typename ScopeDataT::NameType>::value);
  static_assert(CanCopyDataToDisk<ScopeDataT>::value,
                "ScopeData cannot be bulk-copied to disk");
}

template <XDRMode mode>
/* static */ XDRResult StencilXDR::codeScopeData(
    XDRState<mode>* xdr, ScopeStencil& stencil,
    BaseParserScopeData*& baseScopeData) {
  // WasmInstanceScope & WasmFunctionScope should not appear in stencils.
  MOZ_ASSERT(stencil.kind_ != ScopeKind::WasmInstance);
  MOZ_ASSERT(stencil.kind_ != ScopeKind::WasmFunction);
  if (stencil.kind_ == ScopeKind::With) {
    return Ok();
  }

  MOZ_TRY(xdr->align32());

  static_assert(offsetof(BaseParserScopeData, length) == 0,
                "length should be the first field");
  uint32_t length;
  if (mode == XDR_ENCODE) {
    length = baseScopeData->length;
  } else {
    MOZ_TRY(xdr->peekUint32(&length));
  }

  AssertScopeSpecificDataIsEncodable<FunctionScope>();
  AssertScopeSpecificDataIsEncodable<VarScope>();
  AssertScopeSpecificDataIsEncodable<LexicalScope>();
  AssertScopeSpecificDataIsEncodable<ClassBodyScope>();
  AssertScopeSpecificDataIsEncodable<EvalScope>();
  AssertScopeSpecificDataIsEncodable<GlobalScope>();
  AssertScopeSpecificDataIsEncodable<ModuleScope>();

  // In both decoding and encoding, stencil.kind_ is now known, and
  // can be assumed.  This allows the encoding to write out the bytes
  // for the specialized scope-data type without needing to encode
  // a distinguishing prefix.
  uint32_t totalLength = SizeOfParserScopeData(stencil.kind_, length);
  MOZ_TRY(xdr->borrowedData(&baseScopeData, totalLength));

  return Ok();
}

template <XDRMode mode>
/* static */
XDRResult StencilXDR::codeSharedData(XDRState<mode>* xdr,
                                     RefPtr<SharedImmutableScriptData>& sisd) {
  if (mode == XDR_ENCODE) {
    MOZ_TRY(XDRImmutableScriptData<mode>(xdr, *sisd));
  } else {
    JSContext* cx = xdr->cx();
    UniquePtr<SharedImmutableScriptData> data(
        SharedImmutableScriptData::create(cx));
    if (!data) {
      return xdr->fail(JS::TranscodeResult::Throw);
    }
    MOZ_TRY(XDRImmutableScriptData<mode>(xdr, *data));
    sisd = data.release();

    if (!SharedImmutableScriptData::shareScriptData(cx, sisd)) {
      return xdr->fail(JS::TranscodeResult::Throw);
    }
  }

  return Ok();
}

// Called from js::XDRScript.
template /* static */ XDRResult StencilXDR::codeSharedData(
    XDRState<XDR_ENCODE>* xdr, RefPtr<SharedImmutableScriptData>& sisd);
template /* static */ XDRResult StencilXDR::codeSharedData(
    XDRState<XDR_DECODE>* xdr, RefPtr<SharedImmutableScriptData>& sisd);

template <XDRMode mode>
/* static */ XDRResult StencilXDR::codeSharedDataContainer(
    XDRState<mode>* xdr, SharedDataContainer& sharedData) {
  if (mode == XDR_ENCODE) {
    if (sharedData.isBorrow()) {
      return codeSharedDataContainer(xdr, *sharedData.asBorrow());
    }
  }

  enum class Kind : uint8_t {
    Single,
    Vector,
    Map,
  };

  uint8_t kind;
  if (mode == XDR_ENCODE) {
    if (sharedData.isSingle()) {
      kind = uint8_t(Kind::Single);
    } else if (sharedData.isVector()) {
      kind = uint8_t(Kind::Vector);
    } else {
      MOZ_ASSERT(sharedData.isMap());
      kind = uint8_t(Kind::Map);
    }
  }
  MOZ_TRY(xdr->codeUint8(&kind));

  switch (Kind(kind)) {
    case Kind::Single: {
      RefPtr<SharedImmutableScriptData> ref;
      if (mode == XDR_ENCODE) {
        ref = sharedData.asSingle();
      }
      MOZ_TRY(codeSharedData<mode>(xdr, ref));
      if (mode == XDR_DECODE) {
        sharedData.setSingle(ref.forget());
      }
      break;
    }

    case Kind::Vector: {
      if (mode == XDR_DECODE) {
        if (!sharedData.initVector(xdr->cx())) {
          return xdr->fail(JS::TranscodeResult::Throw);
        }
      }
      auto& vec = *sharedData.asVector();
      MOZ_TRY(XDRVectorInitialized(xdr, vec));
      for (auto& entry : vec) {
        // NOTE: There can be nullptr, even if we don't perform syntax parsing,
        //       because of constant folding.
        uint8_t exists;
        if (mode == XDR_ENCODE) {
          exists = !!entry;
        }

        MOZ_TRY(xdr->codeUint8(&exists));

        if (exists) {
          MOZ_TRY(codeSharedData<mode>(xdr, entry));
        }
      }
      break;
    }

    case Kind::Map: {
      if (mode == XDR_DECODE) {
        if (!sharedData.initMap(xdr->cx())) {
          return xdr->fail(JS::TranscodeResult::Throw);
        }
      }
      auto& map = *sharedData.asMap();
      uint32_t count;
      if (mode == XDR_ENCODE) {
        count = map.count();
      }
      MOZ_TRY(xdr->codeUint32(&count));
      if (mode == XDR_DECODE) {
        if (!map.reserve(count)) {
          js::ReportOutOfMemory(xdr->cx());
          return xdr->fail(JS::TranscodeResult::Throw);
        }
      }

      if (mode == XDR_ENCODE) {
        for (auto iter = map.iter(); !iter.done(); iter.next()) {
          uint32_t index = iter.get().key().index;
          auto& data = iter.get().value();
          MOZ_TRY(xdr->codeUint32(&index));
          MOZ_TRY(codeSharedData<mode>(xdr, data));
        }
      } else {
        for (uint32_t i = 0; i < count; i++) {
          ScriptIndex index;
          MOZ_TRY(xdr->codeUint32(&index.index));

          RefPtr<SharedImmutableScriptData> data;
          MOZ_TRY(codeSharedData<mode>(xdr, data));

          if (!map.putNew(index, data)) {
            js::ReportOutOfMemory(xdr->cx());
            return xdr->fail(JS::TranscodeResult::Throw);
          }
        }
      }

      break;
    }
  }

  return Ok();
}

template <XDRMode mode>
/* static */ XDRResult StencilXDR::codeParserAtom(XDRState<mode>* xdr,
                                                  ParserAtom** atomp) {
  static_assert(CanCopyDataToDisk<ParserAtom>::value,
                "ParserAtom cannot be bulk-copied to disk.");

  MOZ_TRY(xdr->align32());

  const ParserAtom* header;
  if (mode == XDR_ENCODE) {
    header = *atomp;
  } else {
    MOZ_TRY(xdr->peekData(&header));
  }

  const uint32_t CharSize =
      header->hasLatin1Chars() ? sizeof(JS::Latin1Char) : sizeof(char16_t);
  uint32_t totalLength = sizeof(ParserAtom) + (CharSize * header->length());

  MOZ_TRY(xdr->borrowedData(atomp, totalLength));

  return Ok();
}

template <XDRMode mode>
static XDRResult XDRAtomCount(XDRState<mode>* xdr, uint32_t* atomCount) {
  return xdr->codeUint32(atomCount);
}

template <XDRMode mode>
/* static */ XDRResult StencilXDR::codeParserAtomSpan(
    XDRState<mode>* xdr, LifoAlloc& alloc, ParserAtomSpan& parserAtomData) {
  if (mode == XDR_ENCODE) {
    uint32_t atomVectorLength = parserAtomData.size();
    MOZ_TRY(XDRAtomCount(xdr, &atomVectorLength));

    uint32_t atomCount = 0;
    for (const auto& entry : parserAtomData) {
      if (!entry) {
        continue;
      }
      if (entry->isUsedByStencil()) {
        atomCount++;
      }
    }
    MOZ_TRY(XDRAtomCount(xdr, &atomCount));

    for (uint32_t i = 0; i < atomVectorLength; i++) {
      auto& entry = parserAtomData[i];
      if (!entry) {
        continue;
      }
      if (entry->isUsedByStencil()) {
        MOZ_TRY(xdr->codeUint32(&i));
        MOZ_TRY(codeParserAtom(xdr, &entry));
      }
    }

    return Ok();
  }

  uint32_t atomVectorLength;
  MOZ_TRY(XDRAtomCount(xdr, &atomVectorLength));

  frontend::ParserAtomSpanBuilder builder(parserAtomData);
  if (!builder.allocate(xdr->cx(), alloc, atomVectorLength)) {
    return xdr->fail(JS::TranscodeResult::Throw);
  }

  uint32_t atomCount;
  MOZ_TRY(XDRAtomCount(xdr, &atomCount));

  for (uint32_t i = 0; i < atomCount; i++) {
    frontend::ParserAtom* entry = nullptr;
    uint32_t index;
    MOZ_TRY(xdr->codeUint32(&index));
    MOZ_TRY(codeParserAtom(xdr, &entry));
    if (mode == XDR_DECODE) {
      if (index >= atomVectorLength) {
        return xdr->fail(JS::TranscodeResult::Failure_BadDecode);
      }
    }
    builder.set(frontend::ParserAtomIndex(index), entry);
  }

  return Ok();
}

template <XDRMode mode>
/* static */ XDRResult StencilXDR::codeModuleMetadata(
    XDRState<mode>* xdr, StencilModuleMetadata& stencil) {
  MOZ_TRY(XDRVectorContent(xdr, stencil.requestedModules));
  MOZ_TRY(XDRVectorContent(xdr, stencil.importEntries));
  MOZ_TRY(XDRVectorContent(xdr, stencil.localExportEntries));
  MOZ_TRY(XDRVectorContent(xdr, stencil.indirectExportEntries));
  MOZ_TRY(XDRVectorContent(xdr, stencil.starExportEntries));
  MOZ_TRY(XDRVectorContent(xdr, stencil.functionDecls));

  uint8_t isAsync = 0;
  if (mode == XDR_ENCODE) {
    if (stencil.isAsync) {
      isAsync = stencil.isAsync ? 1 : 0;
    }
  }

  MOZ_TRY(xdr->codeUint8(&isAsync));

  if (mode == XDR_DECODE) {
    stencil.isAsync = isAsync == 1;
  }

  return Ok();
}

template <XDRMode mode>
XDRResult XDRCompilationStencilSpanSize(
    XDRState<mode>* xdr, uint32_t* scriptSize, uint32_t* gcThingSize,
    uint32_t* scopeSize, uint32_t* scriptExtraSize, uint32_t* regExpSize,
    uint32_t* bigIntSize, uint32_t* objLiteralSize) {
  // Compress the series of span sizes, to avoid consuming extra space for
  // unused/small span sizes.
  // There will be align32 shortly after this section, so try to make the
  // padding smaller.

  enum XDRSpanSizeKind {
    // All of the size values fit in 1 byte each. The entire section takes 7
    // bytes, and expect no padding.
    All8Kind,

    // Other cases. All of the size values fit in 4 bytes each. Expect 3 bytes
    // padding for `sizeKind`.
    All32Kind,
  };

  uint8_t sizeKind = All32Kind;
  if (mode == XDR_ENCODE) {
    uint32_t mask = (*scriptSize) | (*gcThingSize) | (*scopeSize) |
                    (*scriptExtraSize) | (*regExpSize) | (*bigIntSize) |
                    (*objLiteralSize);

    if (mask <= 0xff) {
      sizeKind = All8Kind;
    }
  }
  MOZ_TRY(xdr->codeUint8(&sizeKind));

  if (sizeKind == All32Kind) {
    MOZ_TRY(xdr->codeUint32(scriptSize));
    MOZ_TRY(xdr->codeUint32(gcThingSize));
    MOZ_TRY(xdr->codeUint32(scopeSize));
    MOZ_TRY(xdr->codeUint32(scriptExtraSize));
    MOZ_TRY(xdr->codeUint32(regExpSize));
    MOZ_TRY(xdr->codeUint32(bigIntSize));
    MOZ_TRY(xdr->codeUint32(objLiteralSize));
  } else {
    uint8_t scriptSize8 = 0;
    uint8_t gcThingSize8 = 0;
    uint8_t scopeSize8 = 0;
    uint8_t scriptExtraSize8 = 0;
    uint8_t regExpSize8 = 0;
    uint8_t bigIntSize8 = 0;
    uint8_t objLiteralSize8 = 0;

    if (mode == XDR_ENCODE) {
      scriptSize8 = uint8_t(*scriptSize);
      gcThingSize8 = uint8_t(*gcThingSize);
      scopeSize8 = uint8_t(*scopeSize);
      scriptExtraSize8 = uint8_t(*scriptExtraSize);
      regExpSize8 = uint8_t(*regExpSize);
      bigIntSize8 = uint8_t(*bigIntSize);
      objLiteralSize8 = uint8_t(*objLiteralSize);
    }

    MOZ_TRY(xdr->codeUint8(&scriptSize8));
    MOZ_TRY(xdr->codeUint8(&gcThingSize8));
    MOZ_TRY(xdr->codeUint8(&scopeSize8));
    MOZ_TRY(xdr->codeUint8(&scriptExtraSize8));
    MOZ_TRY(xdr->codeUint8(&regExpSize8));
    MOZ_TRY(xdr->codeUint8(&bigIntSize8));
    MOZ_TRY(xdr->codeUint8(&objLiteralSize8));

    if (mode == XDR_DECODE) {
      *scriptSize = scriptSize8;
      *gcThingSize = gcThingSize8;
      *scopeSize = scopeSize8;
      *scriptExtraSize = scriptExtraSize8;
      *regExpSize = regExpSize8;
      *bigIntSize = bigIntSize8;
      *objLiteralSize = objLiteralSize8;
    }
  }

  return Ok();
}

// Marker between each section inside CompilationStencil.
//
// These values should meet the following requirement:
//   * No same value (differ more than single bit flip)
//   * Bit pattern that won't frequently appear inside other XDR data
//
// Currently they're randomly chosen prime numbers that doesn't have same
// byte pattern.
enum class SectionMarker : uint32_t {
  ParserAtomData = 0xD9C098D3,
  ScopeData = 0x892C25EF,
  ScopeNames = 0x638C4FB3,
  RegExpData = 0xB030C2AF,
  BigIntData = 0x4B24F449,
  ObjLiteralData = 0x9AFAAE45,
  SharedData = 0xAAD52687,
  GCThingData = 0x1BD8F533,
  ScriptData = 0x840458FF,
  ScriptExtra = 0xA90E489D,
  ModuleMetadata = 0x94FDCE6D,
  End = 0x16DDA135,
};

template <XDRMode mode>
static XDRResult CodeMarker(XDRState<mode>* xdr, SectionMarker marker) {
  return xdr->codeMarker(uint32_t(marker));
}

static void ReportStencilXDRError(JSContext* cx, ErrorMetadata&& metadata,
                                  int errorNumber, ...) {
  va_list args;
  va_start(args, errorNumber);
  ReportCompileErrorUTF8(cx, std::move(metadata), /* notes = */ nullptr,
                         errorNumber, &args);
  va_end(args);
}

template <XDRMode mode>
/* static */ XDRResult StencilXDR::codeCompilationStencil(
    XDRState<mode>* xdr, CompilationStencil& stencil) {
  MOZ_ASSERT(!stencil.asmJS);

  if (mode == XDR_DECODE) {
    stencil.hasExternalDependency = true;
  }

  MOZ_TRY(CodeMarker(xdr, SectionMarker::ParserAtomData));
  MOZ_TRY(codeParserAtomSpan(xdr, stencil.alloc, stencil.parserAtomData));

  uint8_t canLazilyParse = 0;

  if (mode == XDR_ENCODE) {
    canLazilyParse = stencil.canLazilyParse;
  }
  MOZ_TRY(xdr->codeUint8(&canLazilyParse));
  if (mode == XDR_DECODE) {
    stencil.canLazilyParse = canLazilyParse;
    MOZ_ASSERT(xdr->hasOptions());
    if (stencil.canLazilyParse != CanLazilyParse(xdr->options())) {
      ErrorMetadata metadata;
      metadata.filename = "<unknown>";
      metadata.lineNumber = 1;
      metadata.columnNumber = 0;
      metadata.isMuted = false;
      ReportStencilXDRError(xdr->cx(), std::move(metadata),
                            JSMSG_STENCIL_OPTIONS_MISMATCH);
      return xdr->fail(JS::TranscodeResult::Throw);
    }
  }

  MOZ_TRY(xdr->codeUint32(&stencil.functionKey));

  uint32_t scriptSize, gcThingSize, scopeSize, scriptExtraSize;
  uint32_t regExpSize, bigIntSize, objLiteralSize;
  if (mode == XDR_ENCODE) {
    scriptSize = stencil.scriptData.size();
    gcThingSize = stencil.gcThingData.size();
    scopeSize = stencil.scopeData.size();
    MOZ_ASSERT(scopeSize == stencil.scopeNames.size());

    scriptExtraSize = stencil.scriptExtra.size();

    regExpSize = stencil.regExpData.size();
    bigIntSize = stencil.bigIntData.size();
    objLiteralSize = stencil.objLiteralData.size();
  }
  MOZ_TRY(XDRCompilationStencilSpanSize(
      xdr, &scriptSize, &gcThingSize, &scopeSize, &scriptExtraSize, &regExpSize,
      &bigIntSize, &objLiteralSize));

  // All of the vector-indexed data elements referenced by the
  // main script tree must be materialized first.

  MOZ_TRY(CodeMarker(xdr, SectionMarker::ScopeData));
  MOZ_TRY(XDRSpanContent(xdr, stencil.scopeData, scopeSize));

  MOZ_TRY(CodeMarker(xdr, SectionMarker::ScopeNames));
  MOZ_TRY(
      XDRSpanInitialized(xdr, stencil.alloc, stencil.scopeNames, scopeSize));
  MOZ_ASSERT(stencil.scopeData.size() == stencil.scopeNames.size());
  for (uint32_t i = 0; i < scopeSize; i++) {
    MOZ_TRY(codeScopeData(xdr, stencil.scopeData[i], stencil.scopeNames[i]));
  }

  MOZ_TRY(CodeMarker(xdr, SectionMarker::RegExpData));
  MOZ_TRY(XDRSpanContent(xdr, stencil.regExpData, regExpSize));

  MOZ_TRY(CodeMarker(xdr, SectionMarker::BigIntData));
  MOZ_TRY(
      XDRSpanInitialized(xdr, stencil.alloc, stencil.bigIntData, bigIntSize));
  for (auto& entry : stencil.bigIntData) {
    MOZ_TRY(codeBigInt(xdr, entry));
  }

  MOZ_TRY(CodeMarker(xdr, SectionMarker::ObjLiteralData));
  MOZ_TRY(XDRSpanInitialized(xdr, stencil.alloc, stencil.objLiteralData,
                             objLiteralSize));
  for (auto& entry : stencil.objLiteralData) {
    MOZ_TRY(codeObjLiteral(xdr, entry));
  }

  MOZ_TRY(CodeMarker(xdr, SectionMarker::SharedData));
  MOZ_TRY(codeSharedDataContainer(xdr, stencil.sharedData));

  MOZ_TRY(CodeMarker(xdr, SectionMarker::GCThingData));
  MOZ_TRY(XDRSpanContent(xdr, stencil.gcThingData, gcThingSize));

  // Now serialize the vector of ScriptStencils.
  MOZ_TRY(CodeMarker(xdr, SectionMarker::ScriptData));
  MOZ_TRY(XDRSpanContent(xdr, stencil.scriptData, scriptSize));

  MOZ_TRY(CodeMarker(xdr, SectionMarker::ScriptExtra));
  MOZ_TRY(XDRSpanContent(xdr, stencil.scriptExtra, scriptExtraSize));

  // We don't support coding non-initial CompilationStencil.
  MOZ_ASSERT(stencil.isInitialStencil());

  if (stencil.scriptExtra[CompilationStencil::TopLevelIndex].isModule()) {
    if (mode == XDR_DECODE) {
      stencil.moduleMetadata =
          xdr->cx()->template new_<StencilModuleMetadata>();
      if (!stencil.moduleMetadata) {
        return xdr->fail(JS::TranscodeResult::Throw);
      }
    }

    MOZ_TRY(CodeMarker(xdr, SectionMarker::ModuleMetadata));
    MOZ_TRY(codeModuleMetadata(xdr, *stencil.moduleMetadata));
  }

  MOZ_TRY(CodeMarker(xdr, SectionMarker::End));

  return Ok();
}

template /* static */ XDRResult StencilXDR::codeCompilationStencil(
    XDRState<XDR_ENCODE>* xdr, CompilationStencil& stencil);

template /* static */ XDRResult StencilXDR::codeCompilationStencil(
    XDRState<XDR_DECODE>* xdr, CompilationStencil& stencil);

/* static */ XDRResult StencilXDR::checkCompilationStencil(
    XDRStencilEncoder* encoder, const CompilationStencil& stencil) {
  if (stencil.asmJS) {
    return encoder->fail(JS::TranscodeResult::Failure_AsmJSNotSupported);
  }

  return Ok();
}

/* static */ XDRResult StencilXDR::checkCompilationStencil(
    const ExtensibleCompilationStencil& stencil) {
  if (stencil.asmJS) {
    return mozilla::Err(JS::TranscodeResult::Failure_AsmJSNotSupported);
  }

  return Ok();
}
