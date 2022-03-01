/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef JS_CACHEIR_SPEW

#  include "jit/CacheIRSpewer.h"

#  include "mozilla/Sprintf.h"

#  include <algorithm>
#  include <stdarg.h>

#  include "jsmath.h"

#  include "js/ScalarType.h"  // js::Scalar::Type
#  include "util/GetPidProvider.h"
#  include "util/Text.h"
#  include "vm/JSFunction.h"
#  include "vm/JSObject.h"
#  include "vm/JSScript.h"

#  include "vm/JSObject-inl.h"
#  include "vm/Realm-inl.h"

using namespace js;
using namespace js::jit;

// Text spewer for CacheIR ops that can be used with JitSpew.
// Output looks like this:
//
//   GuardToInt32                   inputId 0, resultId 2
//   GuardToInt32                   inputId 1, resultId 3
//   CompareInt32Result             op JSOp::Lt, lhsId 2, rhsId 3
//   ReturnFromIC
class MOZ_RAII CacheIROpsJitSpewer {
  GenericPrinter& out_;

  // String prepended to each line. Can be used for indentation.
  const char* prefix_;

  CACHE_IR_SPEWER_GENERATED

  void spewOp(CacheOp op) {
    const char* opName = CacheIROpNames[size_t(op)];
    out_.printf("%s%-30s", prefix_, opName);
  }
  void spewOpEnd() { out_.printf("\n"); }

  void spewArgSeparator() { out_.printf(", "); }

  void spewOperandId(const char* name, OperandId id) {
    spewRawOperandId(name, id.id());
  }
  void spewRawOperandId(const char* name, uint32_t id) {
    out_.printf("%s %u", name, id);
  }
  void spewField(const char* name, uint32_t offset) {
    out_.printf("%s %u", name, offset);
  }
  void spewBoolImm(const char* name, bool b) {
    out_.printf("%s %s", name, b ? "true" : "false");
  }
  void spewByteImm(const char* name, uint8_t val) {
    out_.printf("%s %u", name, val);
  }
  void spewJSOpImm(const char* name, JSOp op) {
    out_.printf("%s JSOp::%s", name, CodeName(op));
  }
  void spewStaticStringImm(const char* name, const char* str) {
    out_.printf("%s \"%s\"", name, str);
  }
  void spewInt32Imm(const char* name, int32_t val) {
    out_.printf("%s %d", name, val);
  }
  void spewUInt32Imm(const char* name, uint32_t val) {
    out_.printf("%s %u", name, val);
  }
  void spewCallFlagsImm(const char* name, CallFlags flags) {
    out_.printf(
        "%s (format %u%s%s%s)", name, flags.getArgFormat(),
        flags.isConstructing() ? ", isConstructing" : "",
        flags.isSameRealm() ? ", isSameRealm" : "",
        flags.needsUninitializedThis() ? ", needsUninitializedThis" : "");
  }
  void spewJSWhyMagicImm(const char* name, JSWhyMagic magic) {
    out_.printf("%s JSWhyMagic(%u)", name, unsigned(magic));
  }
  void spewScalarTypeImm(const char* name, Scalar::Type type) {
    out_.printf("%s Scalar::Type(%u)", name, unsigned(type));
  }
  void spewUnaryMathFunctionImm(const char* name, UnaryMathFunction fun) {
    const char* funName = GetUnaryMathFunctionName(fun);
    out_.printf("%s UnaryMathFunction::%s", name, funName);
  }
  void spewValueTypeImm(const char* name, ValueType type) {
    out_.printf("%s ValueType(%u)", name, unsigned(type));
  }
  void spewJSNativeImm(const char* name, JSNative native) {
    out_.printf("%s %p", name, native);
  }
  void spewGuardClassKindImm(const char* name, GuardClassKind kind) {
    out_.printf("%s GuardClassKind(%u)", name, unsigned(kind));
  }
  void spewWasmValTypeImm(const char* name, wasm::ValType::Kind kind) {
    out_.printf("%s WasmValTypeKind(%u)", name, unsigned(kind));
  }
  void spewAllocKindImm(const char* name, gc::AllocKind kind) {
    out_.printf("%s AllocKind(%u)", name, unsigned(kind));
  }

 public:
  CacheIROpsJitSpewer(GenericPrinter& out, const char* prefix)
      : out_(out), prefix_(prefix) {}

  void spew(CacheIRReader& reader) {
    do {
      switch (reader.readOp()) {
#  define SPEW_OP(op, ...) \
    case CacheOp::op:      \
      spew##op(reader);    \
      break;
        CACHE_IR_OPS(SPEW_OP)
#  undef SPEW_OP

        default:
          MOZ_CRASH("Invalid op");
      }
    } while (reader.more());
  }
};

void js::jit::SpewCacheIROps(GenericPrinter& out, const char* prefix,
                             const CacheIRStubInfo* info) {
  CacheIRReader reader(info);
  CacheIROpsJitSpewer spewer(out, prefix);
  spewer.spew(reader);
}

// JSON spewer for CacheIR ops. Output looks like this:
//
// ...
//    {
//      "op":"GuardToInt32",
//      "args":[
//        {
//          "name":"inputId",
//          "type":"Id",
//          "value":0
//        },
//        {
//          "name":"resultId",
//          "type":"Id",
//          "value":1
//        }
//      ]
//    },
//    {
//      "op":"Int32IncResult",
//      "args":[
//        {
//          "name":"inputId",
//          "type":"Id",
//          "value":1
//        }
//      ]
//    }
// ...
class MOZ_RAII CacheIROpsJSONSpewer {
  JSONPrinter& j_;

  CACHE_IR_SPEWER_GENERATED

  void spewOp(CacheOp op) {
    const char* opName = CacheIROpNames[size_t(op)];
    j_.beginObject();
    j_.property("op", opName);
    j_.beginListProperty("args");
  }
  void spewOpEnd() {
    j_.endList();
    j_.endObject();
  }

  void spewArgSeparator() {}

  template <typename T>
  void spewArgImpl(const char* name, const char* type, T value) {
    j_.beginObject();
    j_.property("name", name);
    j_.property("type", type);
    j_.property("value", value);
    j_.endObject();
  }

  void spewOperandId(const char* name, OperandId id) {
    spewRawOperandId(name, id.id());
  }
  void spewRawOperandId(const char* name, uint32_t id) {
    spewArgImpl(name, "Id", id);
  }
  void spewField(const char* name, uint32_t offset) {
    spewArgImpl(name, "Field", offset);
  }
  void spewBoolImm(const char* name, bool b) { spewArgImpl(name, "Imm", b); }
  void spewByteImm(const char* name, uint8_t val) {
    spewArgImpl(name, "Imm", val);
  }
  void spewJSOpImm(const char* name, JSOp op) {
    spewArgImpl(name, "JSOp", CodeName(op));
  }
  void spewStaticStringImm(const char* name, const char* str) {
    spewArgImpl(name, "String", str);
  }
  void spewInt32Imm(const char* name, int32_t val) {
    spewArgImpl(name, "Imm", val);
  }
  void spewUInt32Imm(const char* name, uint32_t val) {
    spewArgImpl(name, "Imm", val);
  }
  void spewCallFlagsImm(const char* name, CallFlags flags) {
    spewArgImpl(name, "Imm", flags.toByte());
  }
  void spewJSWhyMagicImm(const char* name, JSWhyMagic magic) {
    spewArgImpl(name, "Imm", unsigned(magic));
  }
  void spewScalarTypeImm(const char* name, Scalar::Type type) {
    spewArgImpl(name, "Imm", unsigned(type));
  }
  void spewUnaryMathFunctionImm(const char* name, UnaryMathFunction fun) {
    const char* funName = GetUnaryMathFunctionName(fun);
    spewArgImpl(name, "MathFunction", funName);
  }
  void spewValueTypeImm(const char* name, ValueType type) {
    spewArgImpl(name, "Imm", unsigned(type));
  }
  void spewJSNativeImm(const char* name, JSNative native) {
    spewArgImpl(name, "Word", uintptr_t(native));
  }
  void spewGuardClassKindImm(const char* name, GuardClassKind kind) {
    spewArgImpl(name, "Imm", unsigned(kind));
  }
  void spewWasmValTypeImm(const char* name, wasm::ValType::Kind kind) {
    spewArgImpl(name, "Imm", unsigned(kind));
  }
  void spewAllocKindImm(const char* name, gc::AllocKind kind) {
    spewArgImpl(name, "Imm", unsigned(kind));
  }

 public:
  explicit CacheIROpsJSONSpewer(JSONPrinter& j) : j_(j) {}

  void spew(CacheIRReader& reader) {
    do {
      switch (reader.readOp()) {
#  define SPEW_OP(op, ...) \
    case CacheOp::op:      \
      spew##op(reader);    \
      break;
        CACHE_IR_OPS(SPEW_OP)
#  undef SPEW_OP

        default:
          MOZ_CRASH("Invalid op");
      }
    } while (reader.more());
  }
};

CacheIRSpewer CacheIRSpewer::cacheIRspewer;

CacheIRSpewer::CacheIRSpewer()
    : outputLock_(mutexid::CacheIRSpewer), guardCount_(0) {
  spewInterval_ =
      getenv("CACHEIR_LOG_FLUSH") ? atoi(getenv("CACHEIR_LOG_FLUSH")) : 10000;

  if (spewInterval_ < 1) {
    spewInterval_ = 1;
  }
}

CacheIRSpewer::~CacheIRSpewer() {
  if (!enabled()) {
    return;
  }

  json_.ref().endList();
  output_.flush();
  output_.finish();
}

#  ifndef JIT_SPEW_DIR
#    if defined(_WIN32)
#      define JIT_SPEW_DIR "."
#    elif defined(__ANDROID__)
#      define JIT_SPEW_DIR "/data/local/tmp"
#    else
#      define JIT_SPEW_DIR "/tmp"
#    endif
#  endif

bool CacheIRSpewer::init(const char* filename) {
  if (enabled()) {
    return true;
  }

  char name[256];
  uint32_t pid = getpid();
  // Default to JIT_SPEW_DIR/cacheir${pid}.json
  if (filename[0] == '1') {
    SprintfLiteral(name, JIT_SPEW_DIR "/cacheir%" PRIu32 ".json", pid);
  } else {
    SprintfLiteral(name, "%s%" PRIu32 ".json", filename, pid);
  }

  if (!output_.init(name)) {
    return false;
  }

  json_.emplace(output_);
  json_->beginList();
  return true;
}

void CacheIRSpewer::beginCache(const IRGenerator& gen) {
  MOZ_ASSERT(enabled());
  JSONPrinter& j = json_.ref();
  const char* filename = gen.script_->filename();
  j.beginObject();
  j.property("name", CacheKindNames[uint8_t(gen.cacheKind_)]);
  j.property("file", filename ? filename : "null");
  j.property("mode", int(gen.mode_));
  if (jsbytecode* pc = gen.pc_) {
    unsigned column;
    j.property("line", PCToLineNumber(gen.script_, pc, &column));
    j.property("column", column);
    j.formatProperty("pc", "%p", pc);
  }
}

template <typename CharT>
static void QuoteString(GenericPrinter& out, const CharT* s, size_t length) {
  const CharT* end = s + length;
  for (const CharT* t = s; t < end; s = ++t) {
    // This quote implementation is probably correct,
    // but uses \u even when not strictly necessary.
    char16_t c = *t;
    if (c == '"' || c == '\\') {
      out.printf("\\");
      out.printf("%c", char(c));
    } else if (!IsAsciiPrintable(c)) {
      out.printf("\\u%04x", c);
    } else {
      out.printf("%c", char(c));
    }
  }
}

static void QuoteString(GenericPrinter& out, JSLinearString* str) {
  JS::AutoCheckCannotGC nogc;

  // Limit the string length to reduce the JSON file size.
  size_t length = std::min(str->length(), size_t(128));
  if (str->hasLatin1Chars()) {
    QuoteString(out, str->latin1Chars(nogc), length);
  } else {
    QuoteString(out, str->twoByteChars(nogc), length);
  }
}

void CacheIRSpewer::valueProperty(const char* name, const Value& v) {
  MOZ_ASSERT(enabled());
  JSONPrinter& j = json_.ref();

  j.beginObjectProperty(name);

  const char* type = InformalValueTypeName(v);
  if (v.isInt32()) {
    type = "int32";
  }
  j.property("type", type);

  if (v.isInt32()) {
    j.property("value", v.toInt32());
  } else if (v.isDouble()) {
    j.floatProperty("value", v.toDouble(), 3);
  } else if (v.isString() || v.isSymbol()) {
    JSString* str = v.isString() ? v.toString() : v.toSymbol()->description();
    if (str && str->isLinear()) {
      j.beginStringProperty("value");
      QuoteString(output_, &str->asLinear());
      j.endStringProperty();
    }
  } else if (v.isObject()) {
    JSObject& object = v.toObject();
    j.formatProperty("value", "%p (shape: %p)", &object, object.shape());

    if (object.is<JSFunction>()) {
      if (JSAtom* name = object.as<JSFunction>().displayAtom()) {
        j.beginStringProperty("funName");
        QuoteString(output_, name);
        j.endStringProperty();
      }
    }

    if (NativeObject* nobj =
            object.is<NativeObject>() ? &object.as<NativeObject>() : nullptr) {
      j.beginListProperty("flags");
      {
        if (nobj->isIndexed()) {
          j.value("indexed");
        }
        if (nobj->inDictionaryMode()) {
          j.value("dictionaryMode");
        }
      }
      j.endList();
      if (nobj->isIndexed()) {
        j.beginObjectProperty("indexed");
        {
          j.property("denseInitializedLength",
                     nobj->getDenseInitializedLength());
          j.property("denseCapacity", nobj->getDenseCapacity());
          j.property("denseElementsAreSealed", nobj->denseElementsAreSealed());
          j.property("denseElementsAreFrozen", nobj->denseElementsAreFrozen());
        }
        j.endObject();
      }
    }
  }

  j.endObject();
}

void CacheIRSpewer::opcodeProperty(const char* name, const JSOp op) {
  MOZ_ASSERT(enabled());
  JSONPrinter& j = json_.ref();

  j.beginStringProperty(name);
  output_.put(CodeName(op));
  j.endStringProperty();
}

void CacheIRSpewer::cacheIRSequence(CacheIRReader& reader) {
  MOZ_ASSERT(enabled());
  JSONPrinter& j = json_.ref();

  j.beginListProperty("cacheIR");

  CacheIROpsJSONSpewer spewer(j);
  spewer.spew(reader);

  j.endList();
}

void CacheIRSpewer::attached(const char* name) {
  MOZ_ASSERT(enabled());
  json_.ref().property("attached", name);
}

void CacheIRSpewer::endCache() {
  MOZ_ASSERT(enabled());
  json_.ref().endObject();
}

#endif /* JS_CACHEIR_SPEW */
