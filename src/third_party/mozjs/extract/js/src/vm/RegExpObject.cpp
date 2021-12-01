/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/RegExpObject.h"

#include "mozilla/MemoryReporting.h"
#include "mozilla/PodOperations.h"

#include "builtin/String.h"
#ifdef DEBUG
#include "jsutil.h"
#endif

#include "builtin/RegExp.h"
#include "frontend/TokenStream.h"
#include "gc/HashUtil.h"
#ifdef DEBUG
#include "irregexp/RegExpBytecode.h"
#endif
#include "irregexp/RegExpParser.h"
#include "util/StringBuffer.h"
#include "vm/MatchPairs.h"
#include "vm/RegExpStatics.h"
#include "vm/StringType.h"
#include "vm/TraceLogging.h"
#ifdef DEBUG
#include "util/Unicode.h"
#endif
#include "vm/Xdr.h"

#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/Shape-inl.h"

using namespace js;

using mozilla::ArrayLength;
using mozilla::DebugOnly;
using mozilla::PodCopy;
using js::frontend::TokenStream;

using JS::AutoCheckCannotGC;

JS_STATIC_ASSERT(IgnoreCaseFlag == JSREG_FOLD);
JS_STATIC_ASSERT(GlobalFlag == JSREG_GLOB);
JS_STATIC_ASSERT(MultilineFlag == JSREG_MULTILINE);
JS_STATIC_ASSERT(StickyFlag == JSREG_STICKY);
JS_STATIC_ASSERT(UnicodeFlag == JSREG_UNICODE);

RegExpObject*
js::RegExpAlloc(JSContext* cx, NewObjectKind newKind, HandleObject proto /* = nullptr */)
{
    Rooted<RegExpObject*> regexp(cx, NewObjectWithClassProto<RegExpObject>(cx, proto, newKind));
    if (!regexp)
        return nullptr;

    regexp->initPrivate(nullptr);

    if (!EmptyShape::ensureInitialCustomShape<RegExpObject>(cx, regexp))
        return nullptr;

    MOZ_ASSERT(regexp->lookupPure(cx->names().lastIndex)->slot() ==
               RegExpObject::lastIndexSlot());

    return regexp;
}

/* MatchPairs */

bool
VectorMatchPairs::initArrayFrom(VectorMatchPairs& copyFrom)
{
    MOZ_ASSERT(copyFrom.pairCount() > 0);

    if (!allocOrExpandArray(copyFrom.pairCount()))
        return false;

    PodCopy(pairs_, copyFrom.pairs_, pairCount_);

    return true;
}

bool
VectorMatchPairs::allocOrExpandArray(size_t pairCount)
{
    if (!vec_.resizeUninitialized(pairCount))
        return false;

    pairs_ = &vec_[0];
    pairCount_ = pairCount;
    return true;
}

/* RegExpObject */

/* static */ RegExpShared*
RegExpObject::getShared(JSContext* cx, Handle<RegExpObject*> regexp)
{
    if (regexp->hasShared())
        return regexp->sharedRef();

    return createShared(cx, regexp);
}

/* static */ bool
RegExpObject::isOriginalFlagGetter(JSNative native, RegExpFlag* mask)
{
  if (native == regexp_global) {
      *mask = GlobalFlag;
      return true;
  }
  if (native == regexp_ignoreCase) {
      *mask = IgnoreCaseFlag;
      return true;
  }
  if (native == regexp_multiline) {
      *mask = MultilineFlag;
      return true;
  }
  if (native == regexp_sticky) {
      *mask = StickyFlag;
      return true;
  }
  if (native == regexp_unicode) {
      *mask = UnicodeFlag;
      return true;
  }

  return false;
}

/* static */ void
RegExpObject::trace(JSTracer* trc, JSObject* obj)
{
    obj->as<RegExpObject>().trace(trc);
}

static inline bool
IsMarkingTrace(JSTracer* trc)
{
    // Determine whether tracing is happening during normal marking.  We need to
    // test all the following conditions, since:
    //
    //   1. During TraceRuntime, CurrentThreadIsHeapBusy() is true, but the
    //      tracer might not be a marking tracer.
    //   2. When a write barrier executes, IsMarkingTracer is true, but
    //      CurrentThreadIsHeapBusy() will be false.

    return JS::CurrentThreadIsHeapCollecting() && trc->isMarkingTracer();
}

void
RegExpObject::trace(JSTracer* trc)
{
    TraceNullableEdge(trc, &sharedRef(), "RegExpObject shared");
}

static JSObject*
CreateRegExpPrototype(JSContext* cx, JSProtoKey key)
{
    return GlobalObject::createBlankPrototype(cx, cx->global(), &RegExpObject::protoClass_);
}

static const ClassOps RegExpObjectClassOps = {
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* enumerate */
    nullptr, /* newEnumerate */
    nullptr, /* resolve */
    nullptr, /* mayResolve */
    nullptr, /* finalize */
    nullptr, /* call */
    nullptr, /* hasInstance */
    nullptr, /* construct */
    RegExpObject::trace,
};

static const ClassSpec RegExpObjectClassSpec = {
    GenericCreateConstructor<js::regexp_construct, 2, gc::AllocKind::FUNCTION>,
    CreateRegExpPrototype,
    nullptr,
    js::regexp_static_props,
    js::regexp_methods,
    js::regexp_properties
};

const Class RegExpObject::class_ = {
    js_RegExp_str,
    JSCLASS_HAS_PRIVATE |
    JSCLASS_HAS_RESERVED_SLOTS(RegExpObject::RESERVED_SLOTS) |
    JSCLASS_HAS_CACHED_PROTO(JSProto_RegExp),
    &RegExpObjectClassOps,
    &RegExpObjectClassSpec
};

const Class RegExpObject::protoClass_ = {
    js_Object_str,
    JSCLASS_HAS_CACHED_PROTO(JSProto_RegExp),
    JS_NULL_CLASS_OPS,
    &RegExpObjectClassSpec
};

template<typename CharT>
RegExpObject*
RegExpObject::create(JSContext* cx, const CharT* chars, size_t length, RegExpFlag flags,
                     frontend::TokenStreamAnyChars& tokenStream, LifoAlloc& alloc,
                     NewObjectKind newKind)
{
    static_assert(mozilla::IsSame<CharT, char16_t>::value,
                  "this code may need updating if/when CharT encodes UTF-8");

    RootedAtom source(cx, AtomizeChars(cx, chars, length));
    if (!source)
        return nullptr;

    return create(cx, source, flags, tokenStream, alloc, newKind);
}

template RegExpObject*
RegExpObject::create(JSContext* cx, const char16_t* chars, size_t length, RegExpFlag flags,
                     frontend::TokenStreamAnyChars& tokenStream, LifoAlloc& alloc,
                     NewObjectKind newKind);

template<typename CharT>
RegExpObject*
RegExpObject::create(JSContext* cx, const CharT* chars, size_t length, RegExpFlag flags,
                     LifoAlloc& alloc, NewObjectKind newKind)
{
    static_assert(mozilla::IsSame<CharT, char16_t>::value,
                  "this code may need updating if/when CharT encodes UTF-8");

    RootedAtom source(cx, AtomizeChars(cx, chars, length));
    if (!source)
        return nullptr;

    return create(cx, source, flags, alloc, newKind);
}

template RegExpObject*
RegExpObject::create(JSContext* cx, const char16_t* chars, size_t length, RegExpFlag flags,
                     LifoAlloc& alloc, NewObjectKind newKind);

RegExpObject*
RegExpObject::create(JSContext* cx, HandleAtom source, RegExpFlag flags,
                     frontend::TokenStreamAnyChars& tokenStream,
                     LifoAlloc& alloc, NewObjectKind newKind)
{
    if (!irregexp::ParsePatternSyntax(tokenStream, alloc, source, flags & UnicodeFlag))
        return nullptr;

    Rooted<RegExpObject*> regexp(cx, RegExpAlloc(cx, newKind));
    if (!regexp)
        return nullptr;

    regexp->initAndZeroLastIndex(source, flags, cx);

    return regexp;
}

RegExpObject*
RegExpObject::create(JSContext* cx, HandleAtom source, RegExpFlag flags, LifoAlloc& alloc,
                     NewObjectKind newKind)
{
    CompileOptions dummyOptions(cx);
    TokenStream dummyTokenStream(cx, dummyOptions, (const char16_t*) nullptr, 0, nullptr);

    if (!irregexp::ParsePatternSyntax(dummyTokenStream, alloc, source, flags & UnicodeFlag))
        return nullptr;

    Rooted<RegExpObject*> regexp(cx, RegExpAlloc(cx, newKind));
    if (!regexp)
        return nullptr;

    regexp->initAndZeroLastIndex(source, flags, cx);

    return regexp;
}

/* static */ RegExpShared*
RegExpObject::createShared(JSContext* cx, Handle<RegExpObject*> regexp)
{
    MOZ_ASSERT(!regexp->hasShared());
    RootedAtom source(cx, regexp->getSource());
    RegExpShared* shared = cx->zone()->regExps.get(cx, source, regexp->getFlags());
    if (!shared)
        return nullptr;

    regexp->setShared(*shared);
    return shared;
}

Shape*
RegExpObject::assignInitialShape(JSContext* cx, Handle<RegExpObject*> self)
{
    MOZ_ASSERT(self->empty());

    JS_STATIC_ASSERT(LAST_INDEX_SLOT == 0);

    /* The lastIndex property alone is writable but non-configurable. */
    return NativeObject::addDataProperty(cx, self, cx->names().lastIndex, LAST_INDEX_SLOT,
                                         JSPROP_PERMANENT);
}

void
RegExpObject::initIgnoringLastIndex(JSAtom* source, RegExpFlag flags)
{
    // If this is a re-initialization with an existing RegExpShared, 'flags'
    // may not match getShared()->flags, so forget the RegExpShared.
    sharedRef() = nullptr;

    setSource(source);
    setFlags(flags);
}

void
RegExpObject::initAndZeroLastIndex(JSAtom* source, RegExpFlag flags, JSContext* cx)
{
    initIgnoringLastIndex(source, flags);
    zeroLastIndex(cx);
}

static MOZ_ALWAYS_INLINE bool
IsLineTerminator(const JS::Latin1Char c)
{
    return c == '\n' || c == '\r';
}

static MOZ_ALWAYS_INLINE bool
IsLineTerminator(const char16_t c)
{
    return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029;
}

static MOZ_ALWAYS_INLINE bool
AppendEscapedLineTerminator(StringBuffer& sb, const JS::Latin1Char c)
{
    switch (c) {
      case '\n':
        if (!sb.append('n'))
            return false;
        break;
      case '\r':
        if (!sb.append('r'))
            return false;
        break;
      default:
        MOZ_CRASH("Bad LineTerminator");
    }
    return true;
}

static MOZ_ALWAYS_INLINE bool
AppendEscapedLineTerminator(StringBuffer& sb, const char16_t c)
{
    switch (c) {
      case '\n':
        if (!sb.append('n'))
            return false;
        break;
      case '\r':
        if (!sb.append('r'))
            return false;
        break;
      case 0x2028:
        if (!sb.append("u2028"))
            return false;
        break;
      case 0x2029:
        if (!sb.append("u2029"))
            return false;
        break;
      default:
        MOZ_CRASH("Bad LineTerminator");
    }
    return true;
}

template <typename CharT>
static MOZ_ALWAYS_INLINE bool
SetupBuffer(StringBuffer& sb, const CharT* oldChars, size_t oldLen, const CharT* it)
{
    if (mozilla::IsSame<CharT, char16_t>::value && !sb.ensureTwoByteChars())
        return false;

    if (!sb.reserve(oldLen + 1))
        return false;

    sb.infallibleAppend(oldChars, size_t(it - oldChars));
    return true;
}

// Note: leaves the string buffer empty if no escaping need be performed.
template <typename CharT>
static bool
EscapeRegExpPattern(StringBuffer& sb, const CharT* oldChars, size_t oldLen)
{
    bool inBrackets = false;
    bool previousCharacterWasBackslash = false;

    for (const CharT* it = oldChars; it < oldChars + oldLen; ++it) {
        CharT ch = *it;
        if (!previousCharacterWasBackslash) {
            if (inBrackets) {
                if (ch == ']')
                    inBrackets = false;
            } else if (ch == '/') {
                // There's a forward slash that needs escaping.
                if (sb.empty()) {
                    // This is the first char we've seen that needs escaping,
                    // copy everything up to this point.
                    if (!SetupBuffer(sb, oldChars, oldLen, it))
                        return false;
                }
                if (!sb.append('\\'))
                    return false;
            } else if (ch == '[') {
                inBrackets = true;
            }
        }

        if (IsLineTerminator(ch)) {
            // There's LineTerminator that needs escaping.
            if (sb.empty()) {
                // This is the first char we've seen that needs escaping,
                // copy everything up to this point.
                if (!SetupBuffer(sb, oldChars, oldLen, it))
                    return false;
            }
            if (!previousCharacterWasBackslash) {
                if (!sb.append('\\'))
                    return false;
            }
            if (!AppendEscapedLineTerminator(sb, ch))
                return false;
        } else if (!sb.empty()) {
            if (!sb.append(ch))
                return false;
        }

        if (previousCharacterWasBackslash)
            previousCharacterWasBackslash = false;
        else if (ch == '\\')
            previousCharacterWasBackslash = true;
    }

    return true;
}

// ES6 draft rev32 21.2.3.2.4.
JSAtom*
js::EscapeRegExpPattern(JSContext* cx, HandleAtom src)
{
    // Step 2.
    if (src->length() == 0)
        return cx->names().emptyRegExp;

    // We may never need to use |sb|. Start using it lazily.
    StringBuffer sb(cx);

    if (src->hasLatin1Chars()) {
        JS::AutoCheckCannotGC nogc;
        if (!::EscapeRegExpPattern(sb, src->latin1Chars(nogc), src->length()))
            return nullptr;
    } else {
        JS::AutoCheckCannotGC nogc;
        if (!::EscapeRegExpPattern(sb, src->twoByteChars(nogc), src->length()))
            return nullptr;
    }

    // Step 3.
    return sb.empty() ? src : sb.finishAtom();
}

// ES6 draft rev32 21.2.5.14. Optimized for RegExpObject.
JSFlatString*
RegExpObject::toString(JSContext* cx) const
{
    // Steps 3-4.
    RootedAtom src(cx, getSource());
    if (!src)
        return nullptr;
    RootedAtom escapedSrc(cx, EscapeRegExpPattern(cx, src));

    // Step 7.
    StringBuffer sb(cx);
    size_t len = escapedSrc->length();
    if (!sb.reserve(len + 2))
        return nullptr;
    sb.infallibleAppend('/');
    if (!sb.append(escapedSrc))
        return nullptr;
    sb.infallibleAppend('/');

    // Steps 5-7.
    if (global() && !sb.append('g'))
        return nullptr;
    if (ignoreCase() && !sb.append('i'))
        return nullptr;
    if (multiline() && !sb.append('m'))
        return nullptr;
    if (unicode() && !sb.append('u'))
        return nullptr;
    if (sticky() && !sb.append('y'))
        return nullptr;

    return sb.finishString();
}

#ifdef DEBUG
/* static */ bool
RegExpShared::dumpBytecode(JSContext* cx, MutableHandleRegExpShared re, bool match_only,
                           HandleLinearString input)
{
    CompilationMode mode = match_only ? MatchOnly : Normal;
    if (!RegExpShared::compileIfNecessary(cx, re, input, mode, ForceByteCode))
        return false;

    const uint8_t* byteCode = re->compilation(mode, input->hasLatin1Chars()).byteCode;
    const uint8_t* pc = byteCode;

    auto Load32Aligned = [](const uint8_t* pc) -> int32_t {
        MOZ_ASSERT((reinterpret_cast<uintptr_t>(pc) & 3) == 0);
        return *reinterpret_cast<const int32_t*>(pc);
    };

    auto Load16Aligned = [](const uint8_t* pc) -> int32_t {
        MOZ_ASSERT((reinterpret_cast<uintptr_t>(pc) & 1) == 0);
        return *reinterpret_cast<const uint16_t*>(pc);
    };

    int32_t numRegisters = Load32Aligned(pc);
    fprintf(stderr, "numRegisters: %d\n", numRegisters);
    pc += 4;

    fprintf(stderr, "loc     op\n");
    fprintf(stderr, "-----   --\n");

    auto DumpLower = [](const char* text) {
        while (*text) {
            fprintf(stderr, "%c", unicode::ToLowerCase(*text));
            text++;
        }
    };

#define BYTECODE(NAME) \
    case irregexp::BC_##NAME: \
      DumpLower(#NAME);
#define ADVANCE(NAME) \
    fprintf(stderr, "\n"); \
    pc += irregexp::BC_##NAME##_LENGTH; \
    maxPc = js::Max(maxPc, pc); \
    break;
#define STOP(NAME) \
    fprintf(stderr, "\n"); \
    pc += irregexp::BC_##NAME##_LENGTH; \
    break;
#define JUMP(NAME, OFFSET) \
    fprintf(stderr, "\n"); \
    maxPc = js::Max(maxPc, byteCode + OFFSET); \
    pc += irregexp::BC_##NAME##_LENGTH; \
    break;
#define BRANCH(NAME, OFFSET) \
    fprintf(stderr, "\n"); \
    pc += irregexp::BC_##NAME##_LENGTH; \
    maxPc = js::Max(maxPc, js::Max(pc, byteCode + OFFSET)); \
    break;

    // Bytecode has no end marker, we need to calculate the bytecode length by
    // tracing jumps and branches.
    const uint8_t* maxPc = pc;
    while (pc <= maxPc) {
        fprintf(stderr, "%05d:  ", int32_t(pc - byteCode));
        int32_t insn = Load32Aligned(pc);
        switch (insn & irregexp::BYTECODE_MASK) {
          BYTECODE(BREAK) {
            STOP(BREAK);
          }
          BYTECODE(PUSH_CP) {
            ADVANCE(PUSH_CP);
          }
          BYTECODE(PUSH_BT) {
            int32_t offset = Load32Aligned(pc + 4);
            fprintf(stderr, " %d",
                    offset);
            // Pushed value is used by POP_BT for jumping.
            // Resolve maxPc here.
            BRANCH(PUSH_BT, offset);
          }
          BYTECODE(PUSH_REGISTER) {
            fprintf(stderr, " reg[%d]",
                    insn >> irregexp::BYTECODE_SHIFT);
            ADVANCE(PUSH_REGISTER);
          }
          BYTECODE(SET_REGISTER) {
            fprintf(stderr, " reg[%d], %d",
                    insn >> irregexp::BYTECODE_SHIFT,
                    Load32Aligned(pc + 4));
            ADVANCE(SET_REGISTER);
          }
          BYTECODE(ADVANCE_REGISTER) {
            fprintf(stderr, " reg[%d], %d",
                    insn >> irregexp::BYTECODE_SHIFT,
                    Load32Aligned(pc + 4));
            ADVANCE(ADVANCE_REGISTER);
          }
          BYTECODE(SET_REGISTER_TO_CP) {
            fprintf(stderr, " reg[%d], %d",
                    insn >> irregexp::BYTECODE_SHIFT,
                    Load32Aligned(pc + 4));
            ADVANCE(SET_REGISTER_TO_CP);
          }
          BYTECODE(SET_CP_TO_REGISTER) {
            fprintf(stderr, " reg[%d]",
                    insn >> irregexp::BYTECODE_SHIFT);
            ADVANCE(SET_CP_TO_REGISTER);
          }
          BYTECODE(SET_REGISTER_TO_SP) {
            fprintf(stderr, " reg[%d]",
                    insn >> irregexp::BYTECODE_SHIFT);
            ADVANCE(SET_REGISTER_TO_SP);
          }
          BYTECODE(SET_SP_TO_REGISTER) {
            fprintf(stderr, " reg[%d]",
                    insn >> irregexp::BYTECODE_SHIFT);
            ADVANCE(SET_SP_TO_REGISTER);
          }
          BYTECODE(POP_CP) {
            ADVANCE(POP_CP);
          }
          BYTECODE(POP_BT) {
            // Jump is already resolved in PUSH_BT.
            STOP(POP_BT);
          }
          BYTECODE(POP_REGISTER) {
            fprintf(stderr, " reg[%d]",
                    insn >> irregexp::BYTECODE_SHIFT);
            ADVANCE(POP_REGISTER);
          }
          BYTECODE(FAIL) {
            ADVANCE(FAIL);
          }
          BYTECODE(SUCCEED) {
            ADVANCE(SUCCEED);
          }
          BYTECODE(ADVANCE_CP) {
            fprintf(stderr, " %d",
                    insn >> irregexp::BYTECODE_SHIFT);
            ADVANCE(ADVANCE_CP);
          }
          BYTECODE(GOTO) {
            int32_t offset = Load32Aligned(pc + 4);
            fprintf(stderr, " %d",
                    offset);
            JUMP(GOTO, offset);
          }
          BYTECODE(ADVANCE_CP_AND_GOTO) {
            int32_t offset = Load32Aligned(pc + 4);
            fprintf(stderr, " %d, %d",
                    insn >> irregexp::BYTECODE_SHIFT,
                    offset);
            JUMP(ADVANCE_CP_AND_GOTO, offset);
          }
          BYTECODE(CHECK_GREEDY) {
            int32_t offset = Load32Aligned(pc + 4);
            fprintf(stderr, " %d",
                    offset);
            BRANCH(CHECK_GREEDY, offset);
          }
          BYTECODE(LOAD_CURRENT_CHAR) {
            int32_t offset = Load32Aligned(pc + 4);
            fprintf(stderr, " %d, %d",
                    insn >> irregexp::BYTECODE_SHIFT,
                    offset);
            BRANCH(LOAD_CURRENT_CHAR, offset);
          }
          BYTECODE(LOAD_CURRENT_CHAR_UNCHECKED) {
            fprintf(stderr, " %d",
                    insn >> irregexp::BYTECODE_SHIFT);
            ADVANCE(LOAD_CURRENT_CHAR_UNCHECKED);
          }
          BYTECODE(LOAD_2_CURRENT_CHARS) {
            int32_t offset = Load32Aligned(pc + 4);
            fprintf(stderr, " %d, %d",
                    insn >> irregexp::BYTECODE_SHIFT,
                    offset);
            BRANCH(LOAD_2_CURRENT_CHARS, offset);
          }
          BYTECODE(LOAD_2_CURRENT_CHARS_UNCHECKED) {
            fprintf(stderr, " %d",
                    insn >> irregexp::BYTECODE_SHIFT);
            ADVANCE(LOAD_2_CURRENT_CHARS_UNCHECKED);
          }
          BYTECODE(LOAD_4_CURRENT_CHARS) {
            ADVANCE(LOAD_4_CURRENT_CHARS);
          }
          BYTECODE(LOAD_4_CURRENT_CHARS_UNCHECKED) {
            ADVANCE(LOAD_4_CURRENT_CHARS_UNCHECKED);
          }
          BYTECODE(CHECK_4_CHARS) {
            int32_t offset = Load32Aligned(pc + 8);
            fprintf(stderr, " %d, %d",
                    Load32Aligned(pc + 4),
                    offset);
            BRANCH(CHECK_4_CHARS, offset);
          }
          BYTECODE(CHECK_CHAR) {
            int32_t offset = Load32Aligned(pc + 4);
            fprintf(stderr, " %d, %d",
                    insn >> irregexp::BYTECODE_SHIFT,
                    offset);
            BRANCH(CHECK_CHAR, offset);
          }
          BYTECODE(CHECK_NOT_4_CHARS) {
            int32_t offset = Load32Aligned(pc + 8);
            fprintf(stderr, " %d, %d",
                    Load32Aligned(pc + 4),
                    offset);
            BRANCH(CHECK_NOT_4_CHARS, offset);
          }
          BYTECODE(CHECK_NOT_CHAR) {
            int32_t offset = Load32Aligned(pc + 4);
            fprintf(stderr, " %d, %d",
                    insn >> irregexp::BYTECODE_SHIFT,
                    offset);
            BRANCH(CHECK_NOT_CHAR, offset);
          }
          BYTECODE(AND_CHECK_4_CHARS) {
            int32_t offset = Load32Aligned(pc + 12);
            fprintf(stderr, " %d, %d, %d",
                    Load32Aligned(pc + 4),
                    Load32Aligned(pc + 8),
                    offset);
            BRANCH(AND_CHECK_4_CHARS, offset);
          }
          BYTECODE(AND_CHECK_CHAR) {
            int32_t offset = Load32Aligned(pc + 8);
            fprintf(stderr, " %d, %d, %d",
                    insn >> irregexp::BYTECODE_SHIFT,
                    Load32Aligned(pc + 4),
                    offset);
            BRANCH(AND_CHECK_CHAR, offset);
          }
          BYTECODE(AND_CHECK_NOT_4_CHARS) {
            int32_t offset = Load32Aligned(pc + 12);
            fprintf(stderr, " %d, %d, %d",
                    Load32Aligned(pc + 4),
                    Load32Aligned(pc + 8),
                    offset);
            BRANCH(AND_CHECK_NOT_4_CHARS, offset);
          }
          BYTECODE(AND_CHECK_NOT_CHAR) {
            int32_t offset = Load32Aligned(pc + 8);
            fprintf(stderr, " %d, %d, %d",
                    insn >> irregexp::BYTECODE_SHIFT,
                    Load32Aligned(pc + 4),
                    offset);
            BRANCH(AND_CHECK_NOT_CHAR, offset);
          }
          BYTECODE(MINUS_AND_CHECK_NOT_CHAR) {
            int32_t offset = Load32Aligned(pc + 8);
            fprintf(stderr, " %d, %d, %d, %d",
                    insn >> irregexp::BYTECODE_SHIFT,
                    Load16Aligned(pc + 4),
                    Load16Aligned(pc + 6),
                    offset);
            BRANCH(MINUS_AND_CHECK_NOT_CHAR, offset);
          }
          BYTECODE(CHECK_CHAR_IN_RANGE) {
            int32_t offset = Load32Aligned(pc + 8);
            fprintf(stderr, " %d, %d, %d",
                    Load16Aligned(pc + 4),
                    Load16Aligned(pc + 6),
                    offset);
            BRANCH(CHECK_CHAR_IN_RANGE, offset);
          }
          BYTECODE(CHECK_CHAR_NOT_IN_RANGE) {
            int32_t offset = Load32Aligned(pc + 8);
            fprintf(stderr, " %d, %d, %d",
                    Load16Aligned(pc + 4),
                    Load16Aligned(pc + 6),
                    offset);
            BRANCH(CHECK_CHAR_NOT_IN_RANGE, offset);
          }
          BYTECODE(CHECK_BIT_IN_TABLE) {
            int32_t offset = Load32Aligned(pc + 4);
            fprintf(stderr, " %d, "
                    "%02x %02x %02x %02x %02x %02x %02x %02x "
                    "%02x %02x %02x %02x %02x %02x %02x %02x",
                    offset,
                    pc[8], pc[9], pc[10], pc[11],
                    pc[12], pc[13], pc[14], pc[15],
                    pc[16], pc[17], pc[18], pc[19],
                    pc[20], pc[21], pc[22], pc[23]);
            BRANCH(CHECK_BIT_IN_TABLE, offset);
          }
          BYTECODE(CHECK_LT) {
            int32_t offset = Load32Aligned(pc + 4);
            fprintf(stderr, " %d, %d",
                    insn >> irregexp::BYTECODE_SHIFT,
                    offset);
            BRANCH(CHECK_LT, offset);
          }
          BYTECODE(CHECK_GT) {
            int32_t offset = Load32Aligned(pc + 4);
            fprintf(stderr, " %d, %d",
                    insn >> irregexp::BYTECODE_SHIFT,
                    offset);
            BRANCH(CHECK_GT, offset);
          }
          BYTECODE(CHECK_REGISTER_LT) {
            int32_t offset = Load32Aligned(pc + 8);
            fprintf(stderr, " reg[%d], %d, %d",
                    insn >> irregexp::BYTECODE_SHIFT,
                    Load32Aligned(pc + 4),
                    offset);
            BRANCH(CHECK_REGISTER_LT, offset);
          }
          BYTECODE(CHECK_REGISTER_GE) {
            int32_t offset = Load32Aligned(pc + 8);
            fprintf(stderr, " reg[%d], %d, %d",
                    insn >> irregexp::BYTECODE_SHIFT,
                    Load32Aligned(pc + 4),
                    offset);
            BRANCH(CHECK_REGISTER_GE, offset);
          }
          BYTECODE(CHECK_REGISTER_EQ_POS) {
            int32_t offset = Load32Aligned(pc + 4);
            fprintf(stderr, " reg[%d], %d",
                    insn >> irregexp::BYTECODE_SHIFT,
                    offset);
            BRANCH(CHECK_REGISTER_EQ_POS, offset);
          }
          BYTECODE(CHECK_NOT_REGS_EQUAL) {
            int32_t offset = Load32Aligned(pc + 8);
            fprintf(stderr, " reg[%d], %d, %d",
                    insn >> irregexp::BYTECODE_SHIFT,
                    Load32Aligned(pc + 4),
                    offset);
            BRANCH(CHECK_NOT_REGS_EQUAL, offset);
          }
          BYTECODE(CHECK_NOT_BACK_REF) {
            int32_t offset = Load32Aligned(pc + 4);
            fprintf(stderr, " reg[%d], %d",
                    insn >> irregexp::BYTECODE_SHIFT,
                    offset);
            BRANCH(CHECK_NOT_BACK_REF, offset);
          }
          BYTECODE(CHECK_NOT_BACK_REF_NO_CASE) {
            int32_t offset = Load32Aligned(pc + 4);
            fprintf(stderr, " reg[%d], %d",
                    insn >> irregexp::BYTECODE_SHIFT,
                    offset);
            BRANCH(CHECK_NOT_BACK_REF_NO_CASE, offset);
          }
          BYTECODE(CHECK_NOT_BACK_REF_NO_CASE_UNICODE) {
            int32_t offset = Load32Aligned(pc + 4);
            fprintf(stderr, " reg[%d], %d",
                    insn >> irregexp::BYTECODE_SHIFT,
                    offset);
            BRANCH(CHECK_NOT_BACK_REF_NO_CASE_UNICODE, offset);
          }
          BYTECODE(CHECK_AT_START) {
            int32_t offset = Load32Aligned(pc + 4);
            fprintf(stderr, " %d",
                    offset);
            BRANCH(CHECK_AT_START, offset);
          }
          BYTECODE(CHECK_NOT_AT_START) {
            int32_t offset = Load32Aligned(pc + 4);
            fprintf(stderr, " %d",
                    offset);
            BRANCH(CHECK_NOT_AT_START, offset);
          }
          BYTECODE(SET_CURRENT_POSITION_FROM_END) {
            fprintf(stderr, " %u",
                    static_cast<uint32_t>(insn) >> irregexp::BYTECODE_SHIFT);
            ADVANCE(SET_CURRENT_POSITION_FROM_END);
          }
          default:
            MOZ_CRASH("Bad bytecode");
        }
    }

#undef BYTECODE
#undef ADVANCE
#undef STOP
#undef JUMP
#undef BRANCH

    return true;
}

/* static */ bool
RegExpObject::dumpBytecode(JSContext* cx, Handle<RegExpObject*> regexp,
                           bool match_only, HandleLinearString input)
{
    RootedRegExpShared shared(cx, getShared(cx, regexp));
    if (!shared)
        return false;

    return RegExpShared::dumpBytecode(cx, &shared, match_only, input);
}
#endif

template <typename CharT>
static MOZ_ALWAYS_INLINE bool
IsRegExpMetaChar(CharT ch)
{
    switch (ch) {
      /* ES 2016 draft Mar 25, 2016 21.2.1 SyntaxCharacter. */
      case '^': case '$': case '\\': case '.': case '*': case '+':
      case '?': case '(': case ')': case '[': case ']': case '{':
      case '}': case '|':
        return true;
      default:
        return false;
    }
}

template <typename CharT>
bool
js::HasRegExpMetaChars(const CharT* chars, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        if (IsRegExpMetaChar<CharT>(chars[i]))
            return true;
    }
    return false;
}

template bool
js::HasRegExpMetaChars<Latin1Char>(const Latin1Char* chars, size_t length);

template bool
js::HasRegExpMetaChars<char16_t>(const char16_t* chars, size_t length);

bool
js::StringHasRegExpMetaChars(JSLinearString* str)
{
    AutoCheckCannotGC nogc;
    if (str->hasLatin1Chars())
        return HasRegExpMetaChars(str->latin1Chars(nogc), str->length());

    return HasRegExpMetaChars(str->twoByteChars(nogc), str->length());
}

/* RegExpShared */

RegExpShared::RegExpShared(JSAtom* source, RegExpFlag flags)
  : source(source), flags(flags), canStringMatch(false), parenCount(0)
{}

void
RegExpShared::traceChildren(JSTracer* trc)
{
    // Discard code to avoid holding onto ExecutablePools.
    if (IsMarkingTrace(trc) && trc->runtime()->gc.isShrinkingGC())
        discardJitCode();

    TraceNullableEdge(trc, &source, "RegExpShared source");
    for (auto& comp : compilationArray)
        TraceNullableEdge(trc, &comp.jitCode, "RegExpShared code");
}

void
RegExpShared::discardJitCode()
{
    for (auto& comp : compilationArray)
        comp.jitCode = nullptr;

    // We can also purge the tables used by JIT code.
    tables.clearAndFree();
}

void
RegExpShared::finalize(FreeOp* fop)
{
    for (auto& comp : compilationArray)
        js_free(comp.byteCode);
    tables.~JitCodeTables();
}

/* static */ bool
RegExpShared::compile(JSContext* cx, MutableHandleRegExpShared re, HandleLinearString input,
                      CompilationMode mode, ForceByteCodeEnum force)
{
    TraceLoggerThread* logger = TraceLoggerForCurrentThread(cx);
    AutoTraceLog logCompile(logger, TraceLogger_IrregexpCompile);

    RootedAtom pattern(cx, re->source);
    return compile(cx, re, pattern, input, mode, force);
}

/* static */ bool
RegExpShared::compile(JSContext* cx, MutableHandleRegExpShared re, HandleAtom pattern,
                      HandleLinearString input, CompilationMode mode, ForceByteCodeEnum force)
{
    if (!re->ignoreCase() && !StringHasRegExpMetaChars(pattern))
        re->canStringMatch = true;

    CompileOptions options(cx);
    frontend::TokenStream dummyTokenStream(cx, options, nullptr, 0, nullptr);

    LifoAllocScope scope(&cx->tempLifoAlloc());

    /* Parse the pattern. */
    irregexp::RegExpCompileData data;
    if (!irregexp::ParsePattern(dummyTokenStream, cx->tempLifoAlloc(), pattern,
                                re->multiline(), mode == MatchOnly, re->unicode(),
                                re->ignoreCase(), re->global(), re->sticky(), &data))
    {
        return false;
    }

    re->parenCount = data.capture_count;

    JitCodeTables tables;
    irregexp::RegExpCode code = irregexp::CompilePattern(cx, re, &data, input,
                                                         false /* global() */,
                                                         re->ignoreCase(),
                                                         input->hasLatin1Chars(),
                                                         mode == MatchOnly,
                                                         force == ForceByteCode,
                                                         re->sticky(),
                                                         re->unicode(),
                                                         tables);
    if (code.empty())
        return false;

    MOZ_ASSERT(!code.jitCode || !code.byteCode);
    MOZ_ASSERT_IF(force == ForceByteCode, code.byteCode);

    RegExpCompilation& compilation = re->compilation(mode, input->hasLatin1Chars());
    if (code.jitCode) {
        // First copy the tables. GC can purge the tables if the RegExpShared
        // has no JIT code, so it's important to do this right before setting
        // compilation.jitCode (to ensure no purging happens between adding the
        // tables and setting the JIT code).
        for (size_t i = 0; i < tables.length(); i++) {
            if (!re->addTable(Move(tables[i])))
                return false;
        }
        compilation.jitCode = code.jitCode;
    } else if (code.byteCode) {
        MOZ_ASSERT(tables.empty(), "RegExpInterpreter does not use data tables");
        compilation.byteCode = code.byteCode;
    }

    return true;
}

/* static */ bool
RegExpShared::compileIfNecessary(JSContext* cx, MutableHandleRegExpShared re,
                                 HandleLinearString input, CompilationMode mode,
                                 ForceByteCodeEnum force)
{
    if (re->isCompiled(mode, input->hasLatin1Chars(), force))
        return true;
    return compile(cx, re, input, mode, force);
}

/* static */ RegExpRunStatus
RegExpShared::execute(JSContext* cx, MutableHandleRegExpShared re, HandleLinearString input,
                      size_t start, VectorMatchPairs* matches, size_t* endIndex)
{
    MOZ_ASSERT_IF(matches, !endIndex);
    MOZ_ASSERT_IF(!matches, endIndex);
    TraceLoggerThread* logger = TraceLoggerForCurrentThread(cx);

    CompilationMode mode = matches ? Normal : MatchOnly;

    /* Compile the code at point-of-use. */
    if (!compileIfNecessary(cx, re, input, mode, DontForceByteCode))
        return RegExpRunStatus_Error;

    /*
     * Ensure sufficient memory for output vector.
     * No need to initialize it. The RegExp engine fills them in on a match.
     */
    if (matches && !matches->allocOrExpandArray(re->pairCount())) {
        ReportOutOfMemory(cx);
        return RegExpRunStatus_Error;
    }

    size_t length = input->length();

    // Reset the Irregexp backtrack stack if it grows during execution.
    irregexp::RegExpStackScope stackScope(cx);

    if (re->canStringMatch) {
        MOZ_ASSERT(re->pairCount() == 1);
        size_t sourceLength = re->source->length();
        if (re->sticky()) {
            // First part checks size_t overflow.
            if (sourceLength + start < sourceLength || sourceLength + start > length)
                return RegExpRunStatus_Success_NotFound;
            if (!HasSubstringAt(input, re->source, start))
                return RegExpRunStatus_Success_NotFound;

            if (matches) {
                (*matches)[0].start = start;
                (*matches)[0].limit = start + sourceLength;

                matches->checkAgainst(length);
            } else if (endIndex) {
                *endIndex = start + sourceLength;
            }
            return RegExpRunStatus_Success;
        }

        int res = StringFindPattern(input, re->source, start);
        if (res == -1)
            return RegExpRunStatus_Success_NotFound;

        if (matches) {
            (*matches)[0].start = res;
            (*matches)[0].limit = res + sourceLength;

            matches->checkAgainst(length);
        } else if (endIndex) {
            *endIndex = res + sourceLength;
        }
        return RegExpRunStatus_Success;
    }

    do {
        jit::JitCode* code = re->compilation(mode, input->hasLatin1Chars()).jitCode;
        if (!code)
            break;

        RegExpRunStatus result;
        {
            AutoTraceLog logJIT(logger, TraceLogger_IrregexpExecute);
            AutoCheckCannotGC nogc;
            if (input->hasLatin1Chars()) {
                const Latin1Char* chars = input->latin1Chars(nogc);
                result = irregexp::ExecuteCode(cx, code, chars, start, length, matches, endIndex);
            } else {
                const char16_t* chars = input->twoByteChars(nogc);
                result = irregexp::ExecuteCode(cx, code, chars, start, length, matches, endIndex);
            }
        }

        if (result == RegExpRunStatus_Error) {
            // An 'Error' result is returned if a stack overflow guard or
            // interrupt guard failed. If CheckOverRecursed doesn't throw, break
            // out and retry the regexp in the bytecode interpreter, which can
            // execute while tolerating future interrupts. Otherwise, if we keep
            // getting interrupted we will never finish executing the regexp.
            if (!jit::CheckOverRecursed(cx))
                return RegExpRunStatus_Error;
            break;
        }

        if (result == RegExpRunStatus_Success_NotFound)
            return RegExpRunStatus_Success_NotFound;

        MOZ_ASSERT(result == RegExpRunStatus_Success);

        if (matches)
            matches->checkAgainst(length);
        return RegExpRunStatus_Success;
    } while (false);

    // Compile bytecode for the RegExp if necessary.
    if (!compileIfNecessary(cx, re, input, mode, ForceByteCode))
        return RegExpRunStatus_Error;

    uint8_t* byteCode = re->compilation(mode, input->hasLatin1Chars()).byteCode;
    AutoTraceLog logInterpreter(logger, TraceLogger_IrregexpExecute);

    AutoStableStringChars inputChars(cx);
    if (!inputChars.init(cx, input))
        return RegExpRunStatus_Error;

    RegExpRunStatus result;
    if (inputChars.isLatin1()) {
        const Latin1Char* chars = inputChars.latin1Range().begin().get();
        result = irregexp::InterpretCode(cx, byteCode, chars, start, length, matches, endIndex);
    } else {
        const char16_t* chars = inputChars.twoByteRange().begin().get();
        result = irregexp::InterpretCode(cx, byteCode, chars, start, length, matches, endIndex);
    }

    if (result == RegExpRunStatus_Success && matches)
        matches->checkAgainst(length);
    return result;
}

size_t
RegExpShared::sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf)
{
    size_t n = 0;

    for (size_t i = 0; i < ArrayLength(compilationArray); i++) {
        const RegExpCompilation& compilation = compilationArray[i];
        if (compilation.byteCode)
            n += mallocSizeOf(compilation.byteCode);
    }

    n += tables.sizeOfExcludingThis(mallocSizeOf);
    for (size_t i = 0; i < tables.length(); i++)
        n += mallocSizeOf(tables[i].get());

    return n;
}

/* RegExpCompartment */

RegExpCompartment::RegExpCompartment()
  : matchResultTemplateObject_(nullptr),
    optimizableRegExpPrototypeShape_(nullptr),
    optimizableRegExpInstanceShape_(nullptr)
{}

ArrayObject*
RegExpCompartment::createMatchResultTemplateObject(JSContext* cx)
{
    MOZ_ASSERT(!matchResultTemplateObject_);

    /* Create template array object */
    RootedArrayObject templateObject(cx, NewDenseUnallocatedArray(cx, RegExpObject::MaxPairCount,
                                                                  nullptr, TenuredObject));
    if (!templateObject)
        return matchResultTemplateObject_; // = nullptr

    // Create a new group for the template.
    Rooted<TaggedProto> proto(cx, templateObject->taggedProto());
    ObjectGroup* group = ObjectGroupCompartment::makeGroup(cx, templateObject->getClass(), proto);
    if (!group)
        return matchResultTemplateObject_; // = nullptr
    templateObject->setGroup(group);

    /* Set dummy index property */
    RootedValue index(cx, Int32Value(0));
    if (!NativeDefineDataProperty(cx, templateObject, cx->names().index, index, JSPROP_ENUMERATE))
        return matchResultTemplateObject_; // = nullptr

    /* Set dummy input property */
    RootedValue inputVal(cx, StringValue(cx->runtime()->emptyString));
    if (!NativeDefineDataProperty(cx, templateObject, cx->names().input, inputVal,
                                  JSPROP_ENUMERATE))
    {
        return matchResultTemplateObject_; // = nullptr
    }

    // Make sure that the properties are in the right slots.
    DebugOnly<Shape*> shape = templateObject->lastProperty();
    MOZ_ASSERT(shape->previous()->slot() == 0 &&
               shape->previous()->propidRef() == NameToId(cx->names().index));
    MOZ_ASSERT(shape->slot() == 1 &&
               shape->propidRef() == NameToId(cx->names().input));

    // Make sure type information reflects the indexed properties which might
    // be added.
    AddTypePropertyId(cx, templateObject, JSID_VOID, TypeSet::StringType());
    AddTypePropertyId(cx, templateObject, JSID_VOID, TypeSet::UndefinedType());

    matchResultTemplateObject_.set(templateObject);

    return matchResultTemplateObject_;
}

bool
RegExpZone::init()
{
    if (!set_.init(0))
        return false;

    return true;
}

void
RegExpCompartment::sweep()
{
    if (matchResultTemplateObject_ &&
        IsAboutToBeFinalized(&matchResultTemplateObject_))
    {
        matchResultTemplateObject_.set(nullptr);
    }

    if (optimizableRegExpPrototypeShape_ &&
        IsAboutToBeFinalized(&optimizableRegExpPrototypeShape_))
    {
        optimizableRegExpPrototypeShape_.set(nullptr);
    }

    if (optimizableRegExpInstanceShape_ &&
        IsAboutToBeFinalized(&optimizableRegExpInstanceShape_))
    {
        optimizableRegExpInstanceShape_.set(nullptr);
    }
}

RegExpShared*
RegExpZone::get(JSContext* cx, HandleAtom source, RegExpFlag flags)
{
    DependentAddPtr<Set> p(cx, set_, Key(source, flags));
    if (p)
        return *p;

    auto shared = Allocate<RegExpShared>(cx);
    if (!shared)
        return nullptr;

    new (shared) RegExpShared(source, flags);

    if (!p.add(cx, set_, Key(source, flags), shared)) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    return shared;
}

RegExpShared*
RegExpZone::get(JSContext* cx, HandleAtom atom, JSString* opt)
{
    RegExpFlag flags = RegExpFlag(0);
    if (opt && !ParseRegExpFlags(cx, opt, &flags))
        return nullptr;

    return get(cx, atom, flags);
}

size_t
RegExpZone::sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf)
{
    return set_.sizeOfExcludingThis(mallocSizeOf);
}

RegExpZone::RegExpZone(Zone* zone)
  : set_(zone, zone)
{}

/* Functions */

JSObject*
js::CloneRegExpObject(JSContext* cx, Handle<RegExpObject*> regex)
{
    // Unlike RegExpAlloc, all clones must use |regex|'s group.
    RootedObjectGroup group(cx, regex->group());
    Rooted<RegExpObject*> clone(cx, NewObjectWithGroup<RegExpObject>(cx, group, GenericObject));
    if (!clone)
        return nullptr;
    clone->initPrivate(nullptr);

    if (!EmptyShape::ensureInitialCustomShape<RegExpObject>(cx, clone))
        return nullptr;

    RegExpShared* shared = RegExpObject::getShared(cx, regex);
    if (!shared)
        return nullptr;

    clone->initAndZeroLastIndex(shared->getSource(), shared->getFlags(), cx);
    clone->setShared(*shared);

    return clone;
}

template <typename CharT>
static bool
ParseRegExpFlags(const CharT* chars, size_t length, RegExpFlag* flagsOut, char16_t* invalidFlag)
{
    *flagsOut = RegExpFlag(0);

    for (size_t i = 0; i < length; i++) {
        RegExpFlag flag;
        switch (chars[i]) {
          case 'i':
            flag = IgnoreCaseFlag;
            break;
          case 'g':
            flag = GlobalFlag;
            break;
          case 'm':
            flag = MultilineFlag;
            break;
          case 'y':
            flag = StickyFlag;
            break;
          case 'u':
            flag = UnicodeFlag;
            break;
          default:
            *invalidFlag = chars[i];
            return false;
        }
        if (*flagsOut & flag) {
            *invalidFlag = chars[i];
            return false;
        }
        *flagsOut = RegExpFlag(*flagsOut | flag);
    }

    return true;
}

bool
js::ParseRegExpFlags(JSContext* cx, JSString* flagStr, RegExpFlag* flagsOut)
{
    JSLinearString* linear = flagStr->ensureLinear(cx);
    if (!linear)
        return false;

    size_t len = linear->length();

    bool ok;
    char16_t invalidFlag;
    if (linear->hasLatin1Chars()) {
        AutoCheckCannotGC nogc;
        ok = ::ParseRegExpFlags(linear->latin1Chars(nogc), len, flagsOut, &invalidFlag);
    } else {
        AutoCheckCannotGC nogc;
        ok = ::ParseRegExpFlags(linear->twoByteChars(nogc), len, flagsOut, &invalidFlag);
    }

    if (!ok) {
        TwoByteChars range(&invalidFlag, 1);
        UniqueChars utf8(JS::CharsToNewUTF8CharsZ(nullptr, range).c_str());
        if (!utf8)
            return false;
        JS_ReportErrorFlagsAndNumberUTF8(cx, JSREPORT_ERROR, GetErrorMessage, nullptr,
                                         JSMSG_BAD_REGEXP_FLAG, utf8.get());
        return false;
    }

    return true;
}

template<XDRMode mode>
bool
js::XDRScriptRegExpObject(XDRState<mode>* xdr, MutableHandle<RegExpObject*> objp)
{
    /* NB: Keep this in sync with CloneScriptRegExpObject. */

    RootedAtom source(xdr->cx());
    uint32_t flagsword = 0;

    if (mode == XDR_ENCODE) {
        MOZ_ASSERT(objp);
        RegExpObject& reobj = *objp;
        source = reobj.getSource();
        flagsword = reobj.getFlags();
    }
    if (!XDRAtom(xdr, &source) || !xdr->codeUint32(&flagsword))
        return false;
    if (mode == XDR_DECODE) {
        RegExpObject* reobj = RegExpObject::create(xdr->cx(), source, RegExpFlag(flagsword),
                                                   xdr->lifoAlloc(), TenuredObject);
        if (!reobj)
            return false;

        objp.set(reobj);
    }
    return true;
}

template bool
js::XDRScriptRegExpObject(XDRState<XDR_ENCODE>* xdr, MutableHandle<RegExpObject*> objp);

template bool
js::XDRScriptRegExpObject(XDRState<XDR_DECODE>* xdr, MutableHandle<RegExpObject*> objp);

JSObject*
js::CloneScriptRegExpObject(JSContext* cx, RegExpObject& reobj)
{
    /* NB: Keep this in sync with XDRScriptRegExpObject. */

    RootedAtom source(cx, reobj.getSource());
    cx->markAtom(source);

    return RegExpObject::create(cx, source, reobj.getFlags(), cx->tempLifoAlloc(), TenuredObject);
}

JS_FRIEND_API(RegExpShared*)
js::RegExpToSharedNonInline(JSContext* cx, HandleObject obj)
{
    return RegExpToShared(cx, obj);
}

JS::ubi::Node::Size
JS::ubi::Concrete<RegExpShared>::size(mozilla::MallocSizeOf mallocSizeOf) const
{
    return js::gc::Arena::thingSize(gc::AllocKind::REGEXP_SHARED) +
        get().sizeOfExcludingThis(mallocSizeOf);
}
