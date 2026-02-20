/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/StencilXdr.h"  // StencilXDR

#include "mozilla/ArrayUtils.h"             // mozilla::ArrayEqual
#include "mozilla/OperatorNewExtensions.h"  // mozilla::KnownNotNull
#include "mozilla/RefPtr.h"                 // RefPtr
#include "mozilla/ScopeExit.h"              // mozilla::MakeScopeExit
#include "mozilla/Try.h"                    // MOZ_TRY

#include <stddef.h>     // size_t
#include <stdint.h>     // uint8_t, uint16_t, uint32_t
#include <type_traits>  // std::has_unique_object_representations
#include <utility>      // std::forward

#include "ds/LifoAlloc.h"                 // LifoAlloc
#include "frontend/CompilationStencil.h"  // CompilationStencil, ExtensibleCompilationStencil
#include "frontend/FrontendContext.h"  // FrontendContext, AutoReportFrontendContext
#include "frontend/ScriptIndex.h"      // ScriptIndex
#include "js/CompileOptions.h"         // JS::ReadOnlyDecodeOptions
#include "js/experimental/JSStencil.h"  // ScriptIndex
#include "js/Transcoding.h"  // JS::TranscodeBuffer, JS::TranscodeRange, JS::TranscodeResult
#include "vm/JSScript.h"      // ScriptSource
#include "vm/Scope.h"         // SizeOfParserScopeData
#include "vm/StencilEnums.h"  // js::ImmutableScriptFlagsEnum

using namespace js;
using namespace js::frontend;

using mozilla::Utf8Unit;

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
      js::ReportOutOfMemory(xdr->fc());
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
      js::ReportOutOfMemory(xdr->fc());
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
        js::ReportOutOfMemory(xdr->fc());
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
static XDRResult XDRSpanContent(XDRState<mode>* xdr, LifoAlloc& alloc,
                                mozilla::Span<T>& span, uint32_t size) {
  static_assert(CanCopyDataToDisk<T>::value,
                "Span cannot be bulk-copied to disk.");
  MOZ_ASSERT_IF(mode == XDR_ENCODE, size == span.size());

  if (size) {
    MOZ_TRY(xdr->align32());

    T* data;
    if constexpr (mode == XDR_ENCODE) {
      data = span.data();
      MOZ_TRY(xdr->codeBytes(data, sizeof(T) * size));
    } else {
      const auto& options = static_cast<XDRStencilDecoder*>(xdr)->options();
      if (options.borrowBuffer) {
        MOZ_TRY(xdr->borrowedData(&data, sizeof(T) * size));
      } else {
        data = alloc.template newArrayUninitialized<T>(size);
        if (!data) {
          js::ReportOutOfMemory(xdr->fc());
          return xdr->fail(JS::TranscodeResult::Throw);
        }
        MOZ_TRY(xdr->codeBytes(data, sizeof(T) * size));
      }
    }
    if (mode == XDR_DECODE) {
      span = mozilla::Span(data, size);
    }
  }

  return Ok();
}

template <XDRMode mode, typename T>
static XDRResult XDRSpanContent(XDRState<mode>* xdr, LifoAlloc& alloc,
                                mozilla::Span<T>& span) {
  uint32_t size;
  if (mode == XDR_ENCODE) {
    MOZ_ASSERT(span.size() <= UINT32_MAX);
    size = span.size();
  }

  MOZ_TRY(xdr->codeUint32(&size));

  return XDRSpanContent(xdr, alloc, span, size);
}

template <XDRMode mode>
/* static */ XDRResult StencilXDR::codeBigInt(XDRState<mode>* xdr,
                                              LifoAlloc& alloc,
                                              BigIntStencil& stencil) {
  uint32_t size;
  if (mode == XDR_ENCODE) {
    size = stencil.bigInt_.match(
        [](mozilla::Span<char16_t> source) { return source.size(); },
        [](int64_t) { return size_t(0); });
  }
  MOZ_TRY(xdr->codeUint32(&size));

  // Zero-length size indicates inline storage for int64-sized BigInts.
  if (size == 0) {
    uint64_t num;
    if (mode == XDR_ENCODE) {
      num = static_cast<uint64_t>(stencil.bigInt_.as<int64_t>());
    }
    MOZ_TRY(xdr->codeUint64(&num));
    if (mode == XDR_DECODE) {
      stencil.bigInt_.as<int64_t>() = static_cast<int64_t>(num);
    }
    return Ok();
  }

  return XDRSpanContent(xdr, alloc, stencil.source(), size);
}

template <XDRMode mode>
/* static */ XDRResult StencilXDR::codeObjLiteral(XDRState<mode>* xdr,
                                                  LifoAlloc& alloc,
                                                  ObjLiteralStencil& stencil) {
  uint8_t kindAndFlags = 0;

  if (mode == XDR_ENCODE) {
    static_assert(sizeof(ObjLiteralKindAndFlags) == sizeof(uint8_t));
    kindAndFlags = stencil.kindAndFlags_.toRaw();
  }
  MOZ_TRY(xdr->codeUint8(&kindAndFlags));
  if (mode == XDR_DECODE) {
    stencil.kindAndFlags_.setRaw(kindAndFlags);
  }

  MOZ_TRY(xdr->codeUint32(&stencil.propertyCount_));

  MOZ_TRY(XDRSpanContent(xdr, alloc, stencil.code_));

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
    XDRState<mode>* xdr, LifoAlloc& alloc, ScopeStencil& stencil,
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
  if constexpr (mode == XDR_ENCODE) {
    MOZ_TRY(xdr->codeBytes(baseScopeData, totalLength));
  } else {
    const auto& options = static_cast<XDRStencilDecoder*>(xdr)->options();
    if (options.borrowBuffer) {
      MOZ_TRY(xdr->borrowedData(&baseScopeData, totalLength));
    } else {
      baseScopeData =
          reinterpret_cast<BaseParserScopeData*>(alloc.alloc(totalLength));
      if (!baseScopeData) {
        js::ReportOutOfMemory(xdr->fc());
        return xdr->fail(JS::TranscodeResult::Throw);
      }
      MOZ_TRY(xdr->codeBytes(baseScopeData, totalLength));
    }
  }

  return Ok();
}

template <XDRMode mode>
/* static */
XDRResult StencilXDR::codeSharedData(XDRState<mode>* xdr,
                                     RefPtr<SharedImmutableScriptData>& sisd) {
  static_assert(frontend::CanCopyDataToDisk<ImmutableScriptData>::value,
                "ImmutableScriptData cannot be bulk-copied to disk");
  static_assert(frontend::CanCopyDataToDisk<jsbytecode>::value,
                "jsbytecode cannot be bulk-copied to disk");
  static_assert(frontend::CanCopyDataToDisk<SrcNote>::value,
                "SrcNote cannot be bulk-copied to disk");
  static_assert(frontend::CanCopyDataToDisk<ScopeNote>::value,
                "ScopeNote cannot be bulk-copied to disk");
  static_assert(frontend::CanCopyDataToDisk<TryNote>::value,
                "TryNote cannot be bulk-copied to disk");

  uint32_t size;
  uint32_t hash;
  if (mode == XDR_ENCODE) {
    if (sisd) {
      size = sisd->immutableDataLength();
      hash = sisd->hash();
    } else {
      size = 0;
      hash = 0;
    }
  }
  MOZ_TRY(xdr->codeUint32(&size));

  // A size of zero is used when the `sisd` is nullptr. This can occur for
  // certain outer container modes. In this case, there is no further
  // transcoding to do.
  if (!size) {
    MOZ_ASSERT(!sisd);
    return Ok();
  }

  MOZ_TRY(xdr->align32());
  static_assert(alignof(ImmutableScriptData) <= alignof(uint32_t));

  MOZ_TRY(xdr->codeUint32(&hash));

  if constexpr (mode == XDR_ENCODE) {
    uint8_t* data = const_cast<uint8_t*>(sisd->get()->immutableData().data());
    MOZ_ASSERT(data == reinterpret_cast<const uint8_t*>(sisd->get()),
               "Decode below relies on the data placement");
    MOZ_TRY(xdr->codeBytes(data, size));
  } else {
    sisd = SharedImmutableScriptData::create(xdr->fc());
    if (!sisd) {
      return xdr->fail(JS::TranscodeResult::Throw);
    }

    const auto& options = static_cast<XDRStencilDecoder*>(xdr)->options();
    if (options.usePinnedBytecode) {
      MOZ_ASSERT(options.borrowBuffer);
      ImmutableScriptData* isd;
      MOZ_TRY(xdr->borrowedData(&isd, size));
      sisd->setExternal(isd, hash);
    } else {
      auto isd = ImmutableScriptData::new_(xdr->fc(), size);
      if (!isd) {
        return xdr->fail(JS::TranscodeResult::Throw);
      }
      uint8_t* data = reinterpret_cast<uint8_t*>(isd.get());
      MOZ_TRY(xdr->codeBytes(data, size));
      sisd->setOwn(std::move(isd), hash);
    }

    if (!sisd->get()->validateLayout(size)) {
      MOZ_ASSERT(false, "Bad ImmutableScriptData");
      return xdr->fail(JS::TranscodeResult::Failure_BadDecode);
    }
  }

  if (mode == XDR_DECODE) {
    if (!SharedImmutableScriptData::shareScriptData(xdr->fc(), sisd)) {
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

  enum Kind {
    Single,
    Vector,
    Map,
  };

  Kind kind;
  if (mode == XDR_ENCODE) {
    if (sharedData.isSingle()) {
      kind = Kind::Single;
    } else if (sharedData.isVector()) {
      kind = Kind::Vector;
    } else {
      MOZ_ASSERT(sharedData.isMap());
      kind = Kind::Map;
    }
  }
  MOZ_TRY(xdr->codeEnum32(&kind));

  switch (kind) {
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
        if (!sharedData.initVector(xdr->fc())) {
          return xdr->fail(JS::TranscodeResult::Throw);
        }
      }
      auto& vec = *sharedData.asVector();
      MOZ_TRY(XDRVectorInitialized(xdr, vec));
      for (auto& entry : vec) {
        // NOTE: There can be nullptr, even if we don't perform syntax parsing,
        //       because of constant folding.
        MOZ_TRY(codeSharedData<mode>(xdr, entry));
      }
      break;
    }

    case Kind::Map: {
      if (mode == XDR_DECODE) {
        if (!sharedData.initMap(xdr->fc())) {
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
          js::ReportOutOfMemory(xdr->fc());
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
            js::ReportOutOfMemory(xdr->fc());
            return xdr->fail(JS::TranscodeResult::Throw);
          }
        }
      }

      break;
    }

    default:
      return xdr->fail(JS::TranscodeResult::Failure_BadDecode);
  }

  return Ok();
}

template <XDRMode mode>
/* static */ XDRResult StencilXDR::codeParserAtom(XDRState<mode>* xdr,
                                                  LifoAlloc& alloc,
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

  if constexpr (mode == XDR_ENCODE) {
    MOZ_TRY(xdr->codeBytes(*atomp, totalLength));
  } else {
    const auto& options = static_cast<XDRStencilDecoder*>(xdr)->options();
    if (options.borrowBuffer) {
      MOZ_TRY(xdr->borrowedData(atomp, totalLength));
    } else {
      *atomp = reinterpret_cast<ParserAtom*>(alloc.alloc(totalLength));
      if (!*atomp) {
        js::ReportOutOfMemory(xdr->fc());
        return xdr->fail(JS::TranscodeResult::Throw);
      }
      MOZ_TRY(xdr->codeBytes(*atomp, totalLength));
    }
  }

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
        MOZ_TRY(codeParserAtom(xdr, alloc, &entry));
      }
    }

    return Ok();
  }

  uint32_t atomVectorLength;
  MOZ_TRY(XDRAtomCount(xdr, &atomVectorLength));

  frontend::ParserAtomSpanBuilder builder(parserAtomData);
  if (!builder.allocate(xdr->fc(), alloc, atomVectorLength)) {
    return xdr->fail(JS::TranscodeResult::Throw);
  }

  uint32_t atomCount;
  MOZ_TRY(XDRAtomCount(xdr, &atomCount));

  for (uint32_t i = 0; i < atomCount; i++) {
    frontend::ParserAtom* entry = nullptr;
    uint32_t index;
    MOZ_TRY(xdr->codeUint32(&index));
    MOZ_TRY(codeParserAtom(xdr, alloc, &entry));
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
/* static */ XDRResult StencilXDR::codeModuleRequest(
    XDRState<mode>* xdr, StencilModuleRequest& stencil) {
  MOZ_TRY(xdr->codeUint32(stencil.specifier.rawDataRef()));
  MOZ_TRY(xdr->codeUint32(stencil.firstUnsupportedAttributeKey.rawDataRef()));
  MOZ_TRY(XDRVectorContent(xdr, stencil.attributes));

  return Ok();
}

template <XDRMode mode>
/* static */ XDRResult StencilXDR::codeModuleRequestVector(
    XDRState<mode>* xdr, StencilModuleMetadata::RequestVector& vector) {
  MOZ_TRY(XDRVectorInitialized(xdr, vector));

  for (auto& entry : vector) {
    MOZ_TRY(codeModuleRequest<mode>(xdr, entry));
  }

  return Ok();
}

template <XDRMode mode>
/* static */ XDRResult StencilXDR::codeModuleEntry(
    XDRState<mode>* xdr, StencilModuleEntry& stencil) {
  MOZ_TRY(xdr->codeUint32(&stencil.moduleRequest));
  MOZ_TRY(xdr->codeUint32(stencil.localName.rawDataRef()));
  MOZ_TRY(xdr->codeUint32(stencil.importName.rawDataRef()));
  MOZ_TRY(xdr->codeUint32(stencil.exportName.rawDataRef()));
  MOZ_TRY(xdr->codeUint32(&stencil.lineno));
  MOZ_TRY(xdr->codeUint32(stencil.column.addressOfValueForTranscode()));

  return Ok();
}

template <XDRMode mode>
/* static */ XDRResult StencilXDR::codeModuleEntryVector(
    XDRState<mode>* xdr, StencilModuleMetadata::EntryVector& vector) {
  MOZ_TRY(XDRVectorInitialized(xdr, vector));

  for (auto& entry : vector) {
    MOZ_TRY(codeModuleEntry<mode>(xdr, entry));
  }

  return Ok();
}

template <XDRMode mode>
/* static */ XDRResult StencilXDR::codeModuleMetadata(
    XDRState<mode>* xdr, StencilModuleMetadata& stencil) {
  MOZ_TRY(codeModuleRequestVector(xdr, stencil.moduleRequests));
  MOZ_TRY(codeModuleEntryVector(xdr, stencil.requestedModules));
  MOZ_TRY(codeModuleEntryVector(xdr, stencil.importEntries));
  MOZ_TRY(codeModuleEntryVector(xdr, stencil.localExportEntries));
  MOZ_TRY(codeModuleEntryVector(xdr, stencil.indirectExportEntries));
  MOZ_TRY(codeModuleEntryVector(xdr, stencil.starExportEntries));
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

template <XDRMode mode>
/* static */ XDRResult StencilXDR::codeCompilationStencil(
    XDRState<mode>* xdr, CompilationStencil& stencil) {
  MOZ_ASSERT(!stencil.hasAsmJS());

  if constexpr (mode == XDR_DECODE) {
    const auto& options = static_cast<XDRStencilDecoder*>(xdr)->options();
    if (options.borrowBuffer) {
      stencil.storageType = CompilationStencil::StorageType::Borrowed;
    } else {
      stencil.storageType = CompilationStencil::StorageType::Owned;
    }
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
  MOZ_TRY(XDRSpanContent(xdr, stencil.alloc, stencil.scopeData, scopeSize));

  MOZ_TRY(CodeMarker(xdr, SectionMarker::ScopeNames));
  MOZ_TRY(
      XDRSpanInitialized(xdr, stencil.alloc, stencil.scopeNames, scopeSize));
  MOZ_ASSERT(stencil.scopeData.size() == stencil.scopeNames.size());
  for (uint32_t i = 0; i < scopeSize; i++) {
    MOZ_TRY(codeScopeData(xdr, stencil.alloc, stencil.scopeData[i],
                          stencil.scopeNames[i]));
  }

  MOZ_TRY(CodeMarker(xdr, SectionMarker::RegExpData));
  MOZ_TRY(XDRSpanContent(xdr, stencil.alloc, stencil.regExpData, regExpSize));

  MOZ_TRY(CodeMarker(xdr, SectionMarker::BigIntData));
  MOZ_TRY(
      XDRSpanInitialized(xdr, stencil.alloc, stencil.bigIntData, bigIntSize));
  for (auto& entry : stencil.bigIntData) {
    MOZ_TRY(codeBigInt(xdr, stencil.alloc, entry));
  }

  MOZ_TRY(CodeMarker(xdr, SectionMarker::ObjLiteralData));
  MOZ_TRY(XDRSpanInitialized(xdr, stencil.alloc, stencil.objLiteralData,
                             objLiteralSize));
  for (auto& entry : stencil.objLiteralData) {
    MOZ_TRY(codeObjLiteral(xdr, stencil.alloc, entry));
  }

  MOZ_TRY(CodeMarker(xdr, SectionMarker::SharedData));
  MOZ_TRY(codeSharedDataContainer(xdr, stencil.sharedData));

  MOZ_TRY(CodeMarker(xdr, SectionMarker::GCThingData));
  MOZ_TRY(XDRSpanContent(xdr, stencil.alloc, stencil.gcThingData, gcThingSize));

  // Now serialize the vector of ScriptStencils.
  MOZ_TRY(CodeMarker(xdr, SectionMarker::ScriptData));
  MOZ_TRY(XDRSpanContent(xdr, stencil.alloc, stencil.scriptData, scriptSize));

  MOZ_TRY(CodeMarker(xdr, SectionMarker::ScriptExtra));
  MOZ_TRY(
      XDRSpanContent(xdr, stencil.alloc, stencil.scriptExtra, scriptExtraSize));

  // We don't support coding non-initial CompilationStencil.
  MOZ_ASSERT(stencil.isInitialStencil());

  if (stencil.scriptExtra[CompilationStencil::TopLevelIndex].isModule()) {
    if (mode == XDR_DECODE) {
      stencil.moduleMetadata =
          xdr->fc()->getAllocator()->template new_<StencilModuleMetadata>();
      if (!stencil.moduleMetadata) {
        return xdr->fail(JS::TranscodeResult::Throw);
      }
    }

    MOZ_TRY(CodeMarker(xdr, SectionMarker::ModuleMetadata));
    MOZ_TRY(codeModuleMetadata(xdr, *stencil.moduleMetadata));

    // codeModuleMetadata doesn't guarantee alignment.
    MOZ_TRY(xdr->align32());
  }

  MOZ_TRY(CodeMarker(xdr, SectionMarker::End));

  // The result should be aligned.
  //
  // NOTE:
  // If the top-level isn't a module, ScriptData/ScriptExtra sections
  // guarantee the alignment because there should be at least 1 item,
  // and XDRSpanContent adds alignment before span content, and the struct size
  // should also be aligned.
  static_assert(sizeof(ScriptStencil) % 4 == 0,
                "size of ScriptStencil should be aligned");
  static_assert(sizeof(ScriptStencilExtra) % 4 == 0,
                "size of ScriptStencilExtra should be aligned");
  MOZ_RELEASE_ASSERT(xdr->isAligned32());

  return Ok();
}

template <typename Unit>
struct UnretrievableSourceDecoder {
  XDRState<XDR_DECODE>* const xdr_;
  ScriptSource* const scriptSource_;
  const uint32_t uncompressedLength_;

 public:
  UnretrievableSourceDecoder(XDRState<XDR_DECODE>* xdr,
                             ScriptSource* scriptSource,
                             uint32_t uncompressedLength)
      : xdr_(xdr),
        scriptSource_(scriptSource),
        uncompressedLength_(uncompressedLength) {}

  XDRResult decode() {
    auto sourceUnits = xdr_->fc()->getAllocator()->make_pod_array<Unit>(
        std::max<size_t>(uncompressedLength_, 1));
    if (!sourceUnits) {
      return xdr_->fail(JS::TranscodeResult::Throw);
    }

    MOZ_TRY(xdr_->codeChars(sourceUnits.get(), uncompressedLength_));

    if (!scriptSource_->initializeUnretrievableUncompressedSource(
            xdr_->fc(), std::move(sourceUnits), uncompressedLength_)) {
      return xdr_->fail(JS::TranscodeResult::Throw);
    }

    return Ok();
  }
};

template <>
XDRResult StencilXDR::codeSourceUnretrievableUncompressed<XDR_DECODE>(
    XDRState<XDR_DECODE>* xdr, ScriptSource* ss, uint8_t sourceCharSize,
    uint32_t uncompressedLength) {
  MOZ_ASSERT(sourceCharSize == 1 || sourceCharSize == 2);

  if (sourceCharSize == 1) {
    UnretrievableSourceDecoder<Utf8Unit> decoder(xdr, ss, uncompressedLength);
    return decoder.decode();
  }

  UnretrievableSourceDecoder<char16_t> decoder(xdr, ss, uncompressedLength);
  return decoder.decode();
}

template <typename Unit>
struct UnretrievableSourceEncoder {
  XDRState<XDR_ENCODE>* const xdr_;
  ScriptSource* const source_;
  const uint32_t uncompressedLength_;

  UnretrievableSourceEncoder(XDRState<XDR_ENCODE>* xdr, ScriptSource* source,
                             uint32_t uncompressedLength)
      : xdr_(xdr), source_(source), uncompressedLength_(uncompressedLength) {}

  XDRResult encode() {
    Unit* sourceUnits =
        const_cast<Unit*>(source_->uncompressedData<Unit>()->units());

    return xdr_->codeChars(sourceUnits, uncompressedLength_);
  }
};

template <>
/* static */
XDRResult StencilXDR::codeSourceUnretrievableUncompressed<XDR_ENCODE>(
    XDRState<XDR_ENCODE>* xdr, ScriptSource* ss, uint8_t sourceCharSize,
    uint32_t uncompressedLength) {
  MOZ_ASSERT(sourceCharSize == 1 || sourceCharSize == 2);

  if (sourceCharSize == 1) {
    UnretrievableSourceEncoder<Utf8Unit> encoder(xdr, ss, uncompressedLength);
    return encoder.encode();
  }

  UnretrievableSourceEncoder<char16_t> encoder(xdr, ss, uncompressedLength);
  return encoder.encode();
}

template <typename Unit, XDRMode mode>
/* static */
XDRResult StencilXDR::codeSourceUncompressedData(XDRState<mode>* const xdr,
                                                 ScriptSource* const ss) {
  static_assert(
      std::is_same_v<Unit, Utf8Unit> || std::is_same_v<Unit, char16_t>,
      "should handle UTF-8 and UTF-16");

  if (mode == XDR_ENCODE) {
    MOZ_ASSERT(ss->isUncompressed<Unit>());
  } else {
    MOZ_ASSERT(ss->data.is<ScriptSource::Missing>());
  }

  uint32_t uncompressedLength;
  if (mode == XDR_ENCODE) {
    uncompressedLength = ss->uncompressedData<Unit>()->length();
  }
  MOZ_TRY(xdr->codeUint32(&uncompressedLength));

  return codeSourceUnretrievableUncompressed(xdr, ss, sizeof(Unit),
                                             uncompressedLength);
}

template <typename Unit, XDRMode mode>
/* static */
XDRResult StencilXDR::codeSourceCompressedData(XDRState<mode>* const xdr,
                                               ScriptSource* const ss) {
  static_assert(
      std::is_same_v<Unit, Utf8Unit> || std::is_same_v<Unit, char16_t>,
      "should handle UTF-8 and UTF-16");

  if (mode == XDR_ENCODE) {
    MOZ_ASSERT(ss->isCompressed<Unit>());
  } else {
    MOZ_ASSERT(ss->data.is<ScriptSource::Missing>());
  }

  uint32_t uncompressedLength;
  if (mode == XDR_ENCODE) {
    uncompressedLength =
        ss->data.as<ScriptSource::Compressed<Unit, SourceRetrievable::No>>()
            .uncompressedLength;
  }
  MOZ_TRY(xdr->codeUint32(&uncompressedLength));

  uint32_t compressedLength;
  if (mode == XDR_ENCODE) {
    compressedLength =
        ss->data.as<ScriptSource::Compressed<Unit, SourceRetrievable::No>>()
            .raw.length();
  }
  MOZ_TRY(xdr->codeUint32(&compressedLength));

  if (mode == XDR_DECODE) {
    // Compressed data is always single-byte chars.
    auto bytes = xdr->fc()->getAllocator()->template make_pod_array<char>(
        compressedLength);
    if (!bytes) {
      return xdr->fail(JS::TranscodeResult::Throw);
    }
    MOZ_TRY(xdr->codeBytes(bytes.get(), compressedLength));

    if (!ss->initializeWithUnretrievableCompressedSource<Unit>(
            xdr->fc(), std::move(bytes), compressedLength,
            uncompressedLength)) {
      return xdr->fail(JS::TranscodeResult::Throw);
    }
  } else {
    void* bytes = const_cast<char*>(ss->compressedData<Unit>()->raw.chars());
    MOZ_TRY(xdr->codeBytes(bytes, compressedLength));
  }

  return Ok();
}

template <typename Unit,
          template <typename U, SourceRetrievable CanRetrieve> class Data,
          XDRMode mode>
/* static */
void StencilXDR::codeSourceRetrievable(ScriptSource* const ss) {
  static_assert(
      std::is_same_v<Unit, Utf8Unit> || std::is_same_v<Unit, char16_t>,
      "should handle UTF-8 and UTF-16");

  if (mode == XDR_ENCODE) {
    MOZ_ASSERT((ss->data.is<Data<Unit, SourceRetrievable::Yes>>()));
  } else {
    MOZ_ASSERT(ss->data.is<ScriptSource::Missing>());
    ss->data = ScriptSource::SourceType(ScriptSource::Retrievable<Unit>());
  }
}

template <typename Unit, XDRMode mode>
/* static */
void StencilXDR::codeSourceRetrievableData(ScriptSource* ss) {
  // There's nothing to code for retrievable data.  Just be sure to set
  // retrievable data when decoding.
  if (mode == XDR_ENCODE) {
    MOZ_ASSERT(ss->data.is<ScriptSource::Retrievable<Unit>>());
  } else {
    MOZ_ASSERT(ss->data.is<ScriptSource::Missing>());
    ss->data = ScriptSource::SourceType(ScriptSource::Retrievable<Unit>());
  }
}

template <XDRMode mode>
/* static */
XDRResult StencilXDR::codeSourceData(XDRState<mode>* const xdr,
                                     ScriptSource* const ss) {
  // The order here corresponds to the type order in |ScriptSource::SourceType|
  // so number->internal Variant tag is a no-op.
  enum class DataType {
    CompressedUtf8Retrievable,
    UncompressedUtf8Retrievable,
    CompressedUtf8NotRetrievable,
    UncompressedUtf8NotRetrievable,
    CompressedUtf16Retrievable,
    UncompressedUtf16Retrievable,
    CompressedUtf16NotRetrievable,
    UncompressedUtf16NotRetrievable,
    RetrievableUtf8,
    RetrievableUtf16,
    Missing,
  };

  DataType tag;
  {
    // This is terrible, but we can't do better.  When |mode == XDR_DECODE| we
    // don't have a |ScriptSource::data| |Variant| to match -- the entire XDR
    // idiom for tagged unions depends on coding a tag-number, then the
    // corresponding tagged data.  So we must manually define a tag-enum, code
    // it, then switch on it (and ignore the |Variant::match| API).
    class XDRDataTag {
     public:
      DataType operator()(
          const ScriptSource::Compressed<Utf8Unit, SourceRetrievable::Yes>&) {
        return DataType::CompressedUtf8Retrievable;
      }
      DataType operator()(
          const ScriptSource::Uncompressed<Utf8Unit, SourceRetrievable::Yes>&) {
        return DataType::UncompressedUtf8Retrievable;
      }
      DataType operator()(
          const ScriptSource::Compressed<Utf8Unit, SourceRetrievable::No>&) {
        return DataType::CompressedUtf8NotRetrievable;
      }
      DataType operator()(
          const ScriptSource::Uncompressed<Utf8Unit, SourceRetrievable::No>&) {
        return DataType::UncompressedUtf8NotRetrievable;
      }
      DataType operator()(
          const ScriptSource::Compressed<char16_t, SourceRetrievable::Yes>&) {
        return DataType::CompressedUtf16Retrievable;
      }
      DataType operator()(
          const ScriptSource::Uncompressed<char16_t, SourceRetrievable::Yes>&) {
        return DataType::UncompressedUtf16Retrievable;
      }
      DataType operator()(
          const ScriptSource::Compressed<char16_t, SourceRetrievable::No>&) {
        return DataType::CompressedUtf16NotRetrievable;
      }
      DataType operator()(
          const ScriptSource::Uncompressed<char16_t, SourceRetrievable::No>&) {
        return DataType::UncompressedUtf16NotRetrievable;
      }
      DataType operator()(const ScriptSource::Retrievable<Utf8Unit>&) {
        return DataType::RetrievableUtf8;
      }
      DataType operator()(const ScriptSource::Retrievable<char16_t>&) {
        return DataType::RetrievableUtf16;
      }
      DataType operator()(const ScriptSource::Missing&) {
        return DataType::Missing;
      }
    };

    uint8_t type;
    if (mode == XDR_ENCODE) {
      type = static_cast<uint8_t>(ss->data.match(XDRDataTag()));
    }
    MOZ_TRY(xdr->codeUint8(&type));

    if (type > static_cast<uint8_t>(DataType::Missing)) {
      // Fail in debug, but only soft-fail in release, if the type is invalid.
      MOZ_ASSERT_UNREACHABLE("bad tag");
      return xdr->fail(JS::TranscodeResult::Failure_BadDecode);
    }

    tag = static_cast<DataType>(type);
  }

  switch (tag) {
    case DataType::CompressedUtf8Retrievable:
      codeSourceRetrievable<Utf8Unit, ScriptSource::Compressed, mode>(ss);
      return Ok();

    case DataType::CompressedUtf8NotRetrievable:
      return codeSourceCompressedData<Utf8Unit>(xdr, ss);

    case DataType::UncompressedUtf8Retrievable:
      codeSourceRetrievable<Utf8Unit, ScriptSource::Uncompressed, mode>(ss);
      return Ok();

    case DataType::UncompressedUtf8NotRetrievable:
      return codeSourceUncompressedData<Utf8Unit>(xdr, ss);

    case DataType::CompressedUtf16Retrievable:
      codeSourceRetrievable<char16_t, ScriptSource::Compressed, mode>(ss);
      return Ok();

    case DataType::CompressedUtf16NotRetrievable:
      return codeSourceCompressedData<char16_t>(xdr, ss);

    case DataType::UncompressedUtf16Retrievable:
      codeSourceRetrievable<char16_t, ScriptSource::Uncompressed, mode>(ss);
      return Ok();

    case DataType::UncompressedUtf16NotRetrievable:
      return codeSourceUncompressedData<char16_t>(xdr, ss);

    case DataType::Missing: {
      MOZ_ASSERT(ss->data.is<ScriptSource::Missing>(),
                 "ScriptSource::data is initialized as missing, so neither "
                 "encoding nor decoding has to change anything");

      // There's no data to XDR for missing source.
      break;
    }

    case DataType::RetrievableUtf8:
      codeSourceRetrievableData<Utf8Unit, mode>(ss);
      return Ok();

    case DataType::RetrievableUtf16:
      codeSourceRetrievableData<char16_t, mode>(ss);
      return Ok();
  }

  // The range-check on |type| far above ought ensure the above |switch| is
  // exhaustive and all cases will return, but not all compilers understand
  // this.  Make the Missing case break to here so control obviously never flows
  // off the end.
  MOZ_ASSERT(tag == DataType::Missing);
  return Ok();
}

template <XDRMode mode>
/* static */
XDRResult StencilXDR::codeSource(XDRState<mode>* xdr,
                                 const JS::ReadOnlyDecodeOptions* maybeOptions,
                                 RefPtr<ScriptSource>& source) {
  FrontendContext* fc = xdr->fc();

  if (mode == XDR_DECODE) {
    // Allocate a new ScriptSource and root it with the holder.
    source = do_AddRef(fc->getAllocator()->new_<ScriptSource>());
    if (!source) {
      return xdr->fail(JS::TranscodeResult::Throw);
    }
  }

  static constexpr uint8_t HasFilename = 1 << 0;
  static constexpr uint8_t HasDisplayURL = 1 << 1;
  static constexpr uint8_t HasSourceMapURL = 1 << 2;
  static constexpr uint8_t MutedErrors = 1 << 3;

  uint8_t flags = 0;
  if (mode == XDR_ENCODE) {
    if (source->filename_) {
      flags |= HasFilename;
    }
    if (source->hasDisplayURL()) {
      flags |= HasDisplayURL;
    }
    if (source->hasSourceMapURL()) {
      flags |= HasSourceMapURL;
    }
    if (source->mutedErrors()) {
      flags |= MutedErrors;
    }
  }

  MOZ_TRY(xdr->codeUint8(&flags));

  if (flags & HasFilename) {
    XDRTranscodeString<char> chars;

    if (mode == XDR_ENCODE) {
      chars.construct<const char*>(source->filename());
    }
    MOZ_TRY(xdr->codeCharsZ(chars));
    if (mode == XDR_DECODE) {
      if (!source->setFilename(fc, std::move(chars.ref<UniqueChars>()))) {
        return xdr->fail(JS::TranscodeResult::Throw);
      }
    }
  }

  if (flags & HasDisplayURL) {
    XDRTranscodeString<char16_t> chars;

    if (mode == XDR_ENCODE) {
      chars.construct<const char16_t*>(source->displayURL());
    }
    MOZ_TRY(xdr->codeCharsZ(chars));
    if (mode == XDR_DECODE) {
      if (!source->setDisplayURL(fc,
                                 std::move(chars.ref<UniqueTwoByteChars>()))) {
        return xdr->fail(JS::TranscodeResult::Throw);
      }
    }
  }

  if (flags & HasSourceMapURL) {
    XDRTranscodeString<char16_t> chars;

    if (mode == XDR_ENCODE) {
      chars.construct<const char16_t*>(source->sourceMapURL());
    }
    MOZ_TRY(xdr->codeCharsZ(chars));
    if (mode == XDR_DECODE) {
      if (!source->setSourceMapURL(
              fc, std::move(chars.ref<UniqueTwoByteChars>()))) {
        return xdr->fail(JS::TranscodeResult::Throw);
      }
    }
  }

  MOZ_ASSERT(source->parameterListEnd_ == 0);

  if (flags & MutedErrors) {
    if (mode == XDR_DECODE) {
      source->mutedErrors_ = true;
    }
  }

  MOZ_TRY(xdr->codeUint32(&source->startLine_));
  MOZ_TRY(xdr->codeUint32(source->startColumn_.addressOfValueForTranscode()));

  // The introduction info doesn't persist across encode/decode.
  if (mode == XDR_DECODE) {
    source->introductionType_ = maybeOptions->introductionType;
    source->setIntroductionOffset(maybeOptions->introductionOffset);
    if (maybeOptions->introducerFilename()) {
      if (!source->setIntroducerFilename(
              fc, maybeOptions->introducerFilename().c_str())) {
        return xdr->fail(JS::TranscodeResult::Throw);
      }
    }
  }

  MOZ_TRY(codeSourceData(xdr, source.get()));

  return Ok();
}

template /* static */
    XDRResult
    StencilXDR::codeSource(XDRState<XDR_ENCODE>* xdr,
                           const JS::ReadOnlyDecodeOptions* maybeOptions,
                           RefPtr<ScriptSource>& holder);
template /* static */
    XDRResult
    StencilXDR::codeSource(XDRState<XDR_DECODE>* xdr,
                           const JS::ReadOnlyDecodeOptions* maybeOptions,
                           RefPtr<ScriptSource>& holder);

JS_PUBLIC_API bool JS::GetScriptTranscodingBuildId(
    JS::BuildIdCharVector* buildId) {
  MOZ_ASSERT(buildId->empty());
  MOZ_ASSERT(GetBuildId);

  if (!GetBuildId(buildId)) {
    return false;
  }

  // Note: the buildId returned here is also used for the bytecode cache MIME
  // type so use plain ASCII characters.

  if (!buildId->reserve(buildId->length() + 4)) {
    return false;
  }

  buildId->infallibleAppend('-');

  // XDR depends on pointer size and endianness.
  static_assert(sizeof(uintptr_t) == 4 || sizeof(uintptr_t) == 8);
  buildId->infallibleAppend(sizeof(uintptr_t) == 4 ? '4' : '8');
  buildId->infallibleAppend(MOZ_LITTLE_ENDIAN() ? 'l' : 'b');

  return true;
}

template <XDRMode mode>
static XDRResult VersionCheck(XDRState<mode>* xdr) {
  JS::BuildIdCharVector buildId;
  if (!JS::GetScriptTranscodingBuildId(&buildId)) {
    ReportOutOfMemory(xdr->fc());
    return xdr->fail(JS::TranscodeResult::Throw);
  }
  MOZ_ASSERT(!buildId.empty());

  uint32_t buildIdLength;
  if (mode == XDR_ENCODE) {
    buildIdLength = buildId.length();
  }

  MOZ_TRY(xdr->codeUint32(&buildIdLength));

  if (mode == XDR_DECODE && buildIdLength != buildId.length()) {
    return xdr->fail(JS::TranscodeResult::Failure_BadBuildId);
  }

  if (mode == XDR_ENCODE) {
    MOZ_TRY(xdr->codeBytes(buildId.begin(), buildIdLength));
  } else {
    JS::BuildIdCharVector decodedBuildId;

    // buildIdLength is already checked against the length of current
    // buildId.
    if (!decodedBuildId.resize(buildIdLength)) {
      ReportOutOfMemory(xdr->fc());
      return xdr->fail(JS::TranscodeResult::Throw);
    }

    MOZ_TRY(xdr->codeBytes(decodedBuildId.begin(), buildIdLength));

    // We do not provide binary compatibility with older scripts.
    if (!mozilla::ArrayEqual(decodedBuildId.begin(), buildId.begin(),
                             buildIdLength)) {
      return xdr->fail(JS::TranscodeResult::Failure_BadBuildId);
    }
  }

  return Ok();
}

XDRResult XDRStencilEncoder::codeStencil(
    const RefPtr<ScriptSource>& source,
    const frontend::CompilationStencil& stencil) {
#ifdef DEBUG
  auto sanityCheck = mozilla::MakeScopeExit(
      [&] { MOZ_ASSERT(validateResultCode(fc(), resultCode())); });
#endif

  MOZ_TRY(frontend::StencilXDR::checkCompilationStencil(this, stencil));

  MOZ_TRY(VersionCheck(this));

  uint32_t dummy = 0;
  size_t lengthOffset = buf->cursor();
  MOZ_TRY(codeUint32(&dummy));
  size_t hashOffset = buf->cursor();
  MOZ_TRY(codeUint32(&dummy));

  size_t contentOffset = buf->cursor();
  MOZ_TRY(frontend::StencilXDR::codeSource(
      this, nullptr, const_cast<RefPtr<ScriptSource>&>(source)));
  MOZ_TRY(frontend::StencilXDR::codeCompilationStencil(
      this, const_cast<frontend::CompilationStencil&>(stencil)));
  size_t endOffset = buf->cursor();

  if (endOffset > UINT32_MAX) {
    ReportOutOfMemory(fc());
    return fail(JS::TranscodeResult::Throw);
  }

  uint32_t length = endOffset - contentOffset;
  codeUint32At(&length, lengthOffset);

  const uint8_t* contentBegin = buf->bufferAt(contentOffset);
  uint32_t hash = mozilla::HashBytes(contentBegin, length);
  codeUint32At(&hash, hashOffset);

  return Ok();
}

XDRResult XDRStencilEncoder::codeStencil(
    const frontend::CompilationStencil& stencil) {
  return codeStencil(stencil.source, stencil);
}

static JS::TranscodeResult EncodeStencilImpl(
    JS::FrontendContext* fc, const frontend::CompilationStencil* initial,
    JS::TranscodeBuffer& buffer) {
  XDRStencilEncoder encoder(fc, buffer);
  XDRResult res = encoder.codeStencil(*initial);
  if (res.isErr()) {
    return res.unwrapErr();
  }
  return JS::TranscodeResult::Ok;
}

JS::TranscodeResult JS::EncodeStencil(JSContext* cx, JS::Stencil* stencil,
                                      JS::TranscodeBuffer& buffer) {
  AutoReportFrontendContext fc(cx);

  const CompilationStencil* initial;
  UniquePtr<CompilationStencil> merged;
  if (stencil->canLazilyParse()) {
    merged.reset(stencil->getMerged(&fc));
    if (!merged) {
      return TranscodeResult::Throw;
    }
    initial = merged.get();
  } else {
    initial = stencil->getInitial();
  }

  return EncodeStencilImpl(&fc, initial, buffer);
}

JS::TranscodeResult js::EncodeStencil(JSContext* cx,
                                      frontend::CompilationStencil* stencil,
                                      JS::TranscodeBuffer& buffer) {
  AutoReportFrontendContext fc(cx);
  return EncodeStencilImpl(&fc, stencil, buffer);
}

XDRResult XDRStencilDecoder::codeStencil(
    const JS::ReadOnlyDecodeOptions& options,
    frontend::CompilationStencil& stencil) {
#ifdef DEBUG
  auto sanityCheck = mozilla::MakeScopeExit(
      [&] { MOZ_ASSERT(validateResultCode(fc(), resultCode())); });
#endif

  auto resetOptions = mozilla::MakeScopeExit([&] { options_ = nullptr; });
  options_ = &options;

  MOZ_TRY(VersionCheck(this));

  uint32_t length;
  MOZ_TRY(codeUint32(&length));

  uint32_t hash;
  MOZ_TRY(codeUint32(&hash));

  const uint8_t* contentBegin;
  MOZ_TRY(peekArray(length, &contentBegin));
  uint32_t actualHash = mozilla::HashBytes(contentBegin, length);

  if (MOZ_UNLIKELY(actualHash != hash)) {
    return fail(JS::TranscodeResult::Failure_BadDecode);
  }

  MOZ_TRY(frontend::StencilXDR::codeSource(this, &options, stencil.source));
  MOZ_TRY(frontend::StencilXDR::codeCompilationStencil(this, stencil));

  return Ok();
}

JS::TranscodeResult JS::DecodeStencil(JSContext* cx,
                                      const JS::ReadOnlyDecodeOptions& options,
                                      const JS::TranscodeRange& range,
                                      JS::Stencil** stencilOut) {
  AutoReportFrontendContext fc(cx);
  return JS::DecodeStencil(&fc, options, range, stencilOut);
}

JS::TranscodeResult JS::DecodeStencil(JS::FrontendContext* fc,
                                      const JS::ReadOnlyDecodeOptions& options,
                                      const JS::TranscodeRange& range,
                                      JS::Stencil** stencilOut) {
  RefPtr<CompilationStencil> stencil;
  JS::TranscodeResult result =
      js::DecodeStencil(fc, options, range, getter_AddRefs(stencil));
  if (result != TranscodeResult::Ok) {
    return result;
  }

  RefPtr stencils =
      fc->getAllocator()->new_<frontend::InitialStencilAndDelazifications>();
  if (!stencils) {
    return TranscodeResult::Throw;
  }
  if (!stencils->init(fc, stencil.get())) {
    return TranscodeResult::Throw;
  }
  stencils.forget(stencilOut);
  return TranscodeResult::Ok;
}

JS::TranscodeResult js::DecodeStencil(
    JS::FrontendContext* fc, const JS::ReadOnlyDecodeOptions& options,
    const JS::TranscodeRange& range,
    frontend::CompilationStencil** stencilOut) {
  RefPtr<ScriptSource> source = fc->getAllocator()->new_<ScriptSource>();
  if (!source) {
    return JS::TranscodeResult::Throw;
  }
  RefPtr<CompilationStencil> stencil =
      fc->getAllocator()->new_<CompilationStencil>(source);
  if (!stencil) {
    return JS::TranscodeResult::Throw;
  }
  XDRStencilDecoder decoder(fc, range);
  XDRResult res = decoder.codeStencil(options, *stencil);
  if (res.isErr()) {
    return res.unwrapErr();
  }
  stencil.forget(stencilOut);
  return JS::TranscodeResult::Ok;
}

template /* static */ XDRResult StencilXDR::codeCompilationStencil(
    XDRState<XDR_ENCODE>* xdr, CompilationStencil& stencil);

template /* static */ XDRResult StencilXDR::codeCompilationStencil(
    XDRState<XDR_DECODE>* xdr, CompilationStencil& stencil);

/* static */ XDRResult StencilXDR::checkCompilationStencil(
    XDRStencilEncoder* encoder, const CompilationStencil& stencil) {
  if (stencil.hasAsmJS()) {
    return encoder->fail(JS::TranscodeResult::Failure_AsmJSNotSupported);
  }

  return Ok();
}

/* static */ XDRResult StencilXDR::checkCompilationStencil(
    const ExtensibleCompilationStencil& stencil) {
  if (stencil.hasAsmJS()) {
    return mozilla::Err(JS::TranscodeResult::Failure_AsmJSNotSupported);
  }

  return Ok();
}
