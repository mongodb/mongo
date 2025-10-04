/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/RegExp.h"

#include "mozilla/Casting.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/TextUtils.h"

#include "jsapi.h"

#include "frontend/FrontendContext.h"  // AutoReportFrontendContext
#include "frontend/TokenStream.h"
#include "irregexp/RegExpAPI.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_NEWREGEXP_FLAGGED
#include "js/PropertySpec.h"
#include "js/RegExpFlags.h"  // JS::RegExpFlag, JS::RegExpFlags
#include "util/StringBuffer.h"
#include "vm/Interpreter.h"
#include "vm/JSContext.h"
#include "vm/RegExpObject.h"
#include "vm/RegExpStatics.h"
#include "vm/SelfHosting.h"

#include "vm/EnvironmentObject-inl.h"
#include "vm/GeckoProfiler-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/ObjectOperations-inl.h"
#include "vm/PlainObject-inl.h"

using namespace js;

using mozilla::AssertedCast;
using mozilla::CheckedInt;
using mozilla::IsAsciiDigit;

using JS::CompileOptions;
using JS::RegExpFlag;
using JS::RegExpFlags;

// Allocate an object for the |.groups| or |.indices.groups| property
// of a regexp match result.
static PlainObject* CreateGroupsObject(JSContext* cx,
                                       Handle<PlainObject*> groupsTemplate) {
  if (groupsTemplate->inDictionaryMode()) {
    return NewPlainObjectWithProto(cx, nullptr);
  }

  // The groups template object is stored in RegExpShared, which is shared
  // across compartments and realms. So watch out for the case when the template
  // object's realm is different from the current realm.
  if (cx->realm() != groupsTemplate->realm()) {
    return PlainObject::createWithTemplateFromDifferentRealm(cx,
                                                             groupsTemplate);
  }

  return PlainObject::createWithTemplate(cx, groupsTemplate);
}

static inline void getValueAndIndex(HandleRegExpShared re, uint32_t i,
                                    Handle<ArrayObject*> arr,
                                    MutableHandleValue val,
                                    uint32_t& valueIndex) {
  if (re->numNamedCaptures() == re->numDistinctNamedCaptures()) {
    valueIndex = re->getNamedCaptureIndex(i);
    val.set(arr->getDenseElement(valueIndex));
  } else {
    mozilla::Span<uint32_t> indicesSlice = re->getNamedCaptureIndices(i);
    MOZ_ASSERT(!indicesSlice.IsEmpty());
    valueIndex = indicesSlice[0];
    for (uint32_t index : indicesSlice) {
      val.set(arr->getDenseElement(index));
      if (!val.isUndefined()) {
        valueIndex = index;
        break;
      }
    }
  }
}

/*
 * Implements RegExpBuiltinExec: Steps 18-35
 * https://tc39.es/ecma262/#sec-regexpbuiltinexec
 */
bool js::CreateRegExpMatchResult(JSContext* cx, HandleRegExpShared re,
                                 HandleString input, const MatchPairs& matches,
                                 MutableHandleValue rval) {
  MOZ_ASSERT(re);
  MOZ_ASSERT(input);

  /*
   * Create the (slow) result array for a match.
   *
   * Array contents:
   *  0:              matched string
   *  1..pairCount-1: paren matches
   *  input:          input string
   *  index:          start index for the match
   *  groups:         named capture groups for the match
   *  indices:        capture indices for the match, if required
   */

  bool hasIndices = re->hasIndices();

  // Get the shape for the output object.
  RegExpRealm::ResultShapeKind kind =
      hasIndices ? RegExpRealm::ResultShapeKind::WithIndices
                 : RegExpRealm::ResultShapeKind::Normal;
  Rooted<SharedShape*> shape(
      cx, cx->global()->regExpRealm().getOrCreateMatchResultShape(cx, kind));
  if (!shape) {
    return false;
  }

  // Steps 18-19
  size_t numPairs = matches.length();
  MOZ_ASSERT(numPairs > 0);

  // Steps 20-21: Allocate the match result object.
  Rooted<ArrayObject*> arr(
      cx, NewDenseFullyAllocatedArrayWithShape(cx, numPairs, shape));
  if (!arr) {
    return false;
  }

  // Steps 28-29 and 33 a-d: Initialize the elements of the match result.
  // Store a Value for each match pair.
  for (size_t i = 0; i < numPairs; i++) {
    const MatchPair& pair = matches[i];

    if (pair.isUndefined()) {
      MOZ_ASSERT(i != 0);  // Since we had a match, first pair must be present.
      arr->setDenseInitializedLength(i + 1);
      arr->initDenseElement(i, UndefinedValue());
    } else {
      JSLinearString* str =
          NewDependentString(cx, input, pair.start, pair.length());
      if (!str) {
        return false;
      }
      arr->setDenseInitializedLength(i + 1);
      arr->initDenseElement(i, StringValue(str));
    }
  }

  // Step 34a (reordered): Allocate and initialize the indices object if needed.
  // This is an inlined implementation of MakeIndicesArray:
  // https://tc39.es/ecma262/#sec-makeindicesarray
  Rooted<ArrayObject*> indices(cx);
  Rooted<PlainObject*> indicesGroups(cx);
  if (hasIndices) {
    // MakeIndicesArray: step 8
    Rooted<SharedShape*> indicesShape(
        cx, cx->global()->regExpRealm().getOrCreateMatchResultShape(
                cx, RegExpRealm::ResultShapeKind::Indices));
    if (!indicesShape) {
      return false;
    }
    indices = NewDenseFullyAllocatedArrayWithShape(cx, numPairs, indicesShape);
    if (!indices) {
      return false;
    }

    // MakeIndicesArray: steps 10-12
    if (re->numNamedCaptures() > 0) {
      Rooted<PlainObject*> groupsTemplate(cx, re->getGroupsTemplate());
      indicesGroups = CreateGroupsObject(cx, groupsTemplate);
      if (!indicesGroups) {
        return false;
      }
      indices->initSlot(RegExpRealm::IndicesGroupsSlot,
                        ObjectValue(*indicesGroups));
    }

    // MakeIndicesArray: step 13 a-d. (Step 13.e is implemented below.)
    for (size_t i = 0; i < numPairs; i++) {
      const MatchPair& pair = matches[i];

      if (pair.isUndefined()) {
        // Since we had a match, first pair must be present.
        MOZ_ASSERT(i != 0);
        indices->setDenseInitializedLength(i + 1);
        indices->initDenseElement(i, UndefinedValue());
      } else {
        Rooted<ArrayObject*> indexPair(cx, NewDenseFullyAllocatedArray(cx, 2));
        if (!indexPair) {
          return false;
        }
        indexPair->setDenseInitializedLength(2);
        indexPair->initDenseElement(0, Int32Value(pair.start));
        indexPair->initDenseElement(1, Int32Value(pair.limit));

        indices->setDenseInitializedLength(i + 1);
        indices->initDenseElement(i, ObjectValue(*indexPair));
      }
    }
  }

  // Steps 30-31 (reordered): Allocate the groups object (if needed).
  Rooted<PlainObject*> groups(cx);
  bool groupsInDictionaryMode = false;
  if (re->numNamedCaptures() > 0) {
    Rooted<PlainObject*> groupsTemplate(cx, re->getGroupsTemplate());
    groupsInDictionaryMode = groupsTemplate->inDictionaryMode();
    groups = CreateGroupsObject(cx, groupsTemplate);
    if (!groups) {
      return false;
    }
  }

  // Step 33 e-f: Initialize the properties of |groups| and |indices.groups|.
  // The groups template object stores the names of the named captures
  // in the the order in which they are defined. The named capture
  // indices vector stores the corresponding capture indices. In
  // dictionary mode, we have to define the properties explicitly. If
  // we are not in dictionary mode, we simply fill in the slots with
  // the correct values.
  if (groupsInDictionaryMode) {
    RootedIdVector keys(cx);
    Rooted<PlainObject*> groupsTemplate(cx, re->getGroupsTemplate());
    if (!GetPropertyKeys(cx, groupsTemplate, 0, &keys)) {
      return false;
    }
    MOZ_ASSERT(keys.length() == re->numDistinctNamedCaptures());
    RootedId key(cx);
    RootedValue val(cx);
    uint32_t valueIndex;
    for (uint32_t i = 0; i < keys.length(); i++) {
      key = keys[i];
      getValueAndIndex(re, i, arr, &val, valueIndex);
      if (!NativeDefineDataProperty(cx, groups, key, val, JSPROP_ENUMERATE)) {
        return false;
      }

      // MakeIndicesArray: Step 13.e (reordered)
      if (hasIndices) {
        val = indices->getDenseElement(valueIndex);
        if (!NativeDefineDataProperty(cx, indicesGroups, key, val,
                                      JSPROP_ENUMERATE)) {
          return false;
        }
      }
    }
  } else {
    RootedValue val(cx);
    uint32_t valueIndex;

    for (uint32_t i = 0; i < re->numDistinctNamedCaptures(); i++) {
      getValueAndIndex(re, i, arr, &val, valueIndex);
      groups->initSlot(i, val);

      // MakeIndicesArray: Step 13.e (reordered)
      if (hasIndices) {
        indicesGroups->initSlot(i, indices->getDenseElement(valueIndex));
      }
    }
  }

  // Step 22 (reordered).
  // Set the |index| property.
  arr->initSlot(RegExpRealm::MatchResultObjectIndexSlot,
                Int32Value(matches[0].start));

  // Step 23 (reordered).
  // Set the |input| property.
  arr->initSlot(RegExpRealm::MatchResultObjectInputSlot, StringValue(input));

  // Step 32 (reordered)
  // Set the |groups| property.
  if (groups) {
    arr->initSlot(RegExpRealm::MatchResultObjectGroupsSlot,
                  ObjectValue(*groups));
  }

  // Step 34b
  // Set the |indices| property.
  if (re->hasIndices()) {
    arr->initSlot(RegExpRealm::MatchResultObjectIndicesSlot,
                  ObjectValue(*indices));
  }

#ifdef DEBUG
  RootedValue test(cx);
  RootedId id(cx, NameToId(cx->names().index));
  if (!NativeGetProperty(cx, arr, id, &test)) {
    return false;
  }
  MOZ_ASSERT(test == arr->getSlot(RegExpRealm::MatchResultObjectIndexSlot));
  id = NameToId(cx->names().input);
  if (!NativeGetProperty(cx, arr, id, &test)) {
    return false;
  }
  MOZ_ASSERT(test == arr->getSlot(RegExpRealm::MatchResultObjectInputSlot));
#endif

  // Step 35.
  rval.setObject(*arr);
  return true;
}

static int32_t CreateRegExpSearchResult(JSContext* cx,
                                        const MatchPairs& matches) {
  MOZ_ASSERT(matches[0].start >= 0);
  MOZ_ASSERT(matches[0].limit >= 0);

  MOZ_ASSERT(cx->regExpSearcherLastLimit == RegExpSearcherLastLimitSentinel);

#ifdef DEBUG
  static_assert(JSString::MAX_LENGTH < RegExpSearcherLastLimitSentinel);
  MOZ_ASSERT(uint32_t(matches[0].limit) < RegExpSearcherLastLimitSentinel);
#endif

  cx->regExpSearcherLastLimit = matches[0].limit;
  return matches[0].start;
}

/*
 * ES 2017 draft rev 6a13789aa9e7c6de4e96b7d3e24d9e6eba6584ad 21.2.5.2.2
 * steps 3, 9-14, except 12.a.i, 12.c.i.1.
 */
static RegExpRunStatus ExecuteRegExpImpl(JSContext* cx, RegExpStatics* res,
                                         MutableHandleRegExpShared re,
                                         Handle<JSLinearString*> input,
                                         size_t searchIndex,
                                         VectorMatchPairs* matches) {
  RegExpRunStatus status =
      RegExpShared::execute(cx, re, input, searchIndex, matches);

  /* Out of spec: Update RegExpStatics. */
  if (status == RegExpRunStatus::Success && res) {
    if (!res->updateFromMatchPairs(cx, input, *matches)) {
      return RegExpRunStatus::Error;
    }
  }
  return status;
}

/* Legacy ExecuteRegExp behavior is baked into the JSAPI. */
bool js::ExecuteRegExpLegacy(JSContext* cx, RegExpStatics* res,
                             Handle<RegExpObject*> reobj,
                             Handle<JSLinearString*> input, size_t* lastIndex,
                             bool test, MutableHandleValue rval) {
  cx->check(reobj, input);

  RootedRegExpShared shared(cx, RegExpObject::getShared(cx, reobj));
  if (!shared) {
    return false;
  }

  VectorMatchPairs matches;

  RegExpRunStatus status =
      ExecuteRegExpImpl(cx, res, &shared, input, *lastIndex, &matches);
  if (status == RegExpRunStatus::Error) {
    return false;
  }

  if (status == RegExpRunStatus::Success_NotFound) {
    /* ExecuteRegExp() previously returned an array or null. */
    rval.setNull();
    return true;
  }

  *lastIndex = matches[0].limit;

  if (test) {
    /* Forbid an array, as an optimization. */
    rval.setBoolean(true);
    return true;
  }

  return CreateRegExpMatchResult(cx, shared, input, matches, rval);
}

static bool CheckPatternSyntaxSlow(JSContext* cx, Handle<JSAtom*> pattern,
                                   RegExpFlags flags) {
  LifoAllocScope allocScope(&cx->tempLifoAlloc());
  AutoReportFrontendContext fc(cx);
  CompileOptions options(cx);
  frontend::DummyTokenStream dummyTokenStream(&fc, options);
  return irregexp::CheckPatternSyntax(cx, cx->stackLimitForCurrentPrincipal(),
                                      dummyTokenStream, pattern, flags);
}

static RegExpShared* CheckPatternSyntax(JSContext* cx, Handle<JSAtom*> pattern,
                                        RegExpFlags flags) {
  // If we already have a RegExpShared for this pattern/flags, we can
  // avoid the much slower CheckPatternSyntaxSlow call.

  RootedRegExpShared shared(cx, cx->zone()->regExps().maybeGet(pattern, flags));
  if (shared) {
#ifdef DEBUG
    // Assert the pattern is valid.
    if (!CheckPatternSyntaxSlow(cx, pattern, flags)) {
      MOZ_ASSERT(cx->isThrowingOutOfMemory() || cx->isThrowingOverRecursed());
      return nullptr;
    }
#endif
    return shared;
  }

  if (!CheckPatternSyntaxSlow(cx, pattern, flags)) {
    return nullptr;
  }

  // Allocate and return a new RegExpShared so we will hit the fast path
  // next time.
  return cx->zone()->regExps().get(cx, pattern, flags);
}

/*
 * ES 2016 draft Mar 25, 2016 21.2.3.2.2.
 *
 * Steps 14-15 set |obj|'s "lastIndex" property to zero.  Some of
 * RegExpInitialize's callers have a fresh RegExp not yet exposed to script:
 * in these cases zeroing "lastIndex" is infallible.  But others have a RegExp
 * whose "lastIndex" property might have been made non-writable: here, zeroing
 * "lastIndex" can fail.  We efficiently solve this problem by completely
 * removing "lastIndex" zeroing from the provided function.
 *
 * CALLERS MUST HANDLE "lastIndex" ZEROING THEMSELVES!
 *
 * Because this function only ever returns a user-provided |obj| in the spec,
 * we omit it and just return the usual success/failure.
 */
static bool RegExpInitializeIgnoringLastIndex(JSContext* cx,
                                              Handle<RegExpObject*> obj,
                                              HandleValue patternValue,
                                              HandleValue flagsValue) {
  Rooted<JSAtom*> pattern(cx);
  if (patternValue.isUndefined()) {
    /* Step 1. */
    pattern = cx->names().empty_;
  } else {
    /* Step 2. */
    pattern = ToAtom<CanGC>(cx, patternValue);
    if (!pattern) {
      return false;
    }
  }

  /* Step 3. */
  RegExpFlags flags = RegExpFlag::NoFlags;
  if (!flagsValue.isUndefined()) {
    /* Step 4. */
    RootedString flagStr(cx, ToString<CanGC>(cx, flagsValue));
    if (!flagStr) {
      return false;
    }

    /* Step 5. */
    if (!ParseRegExpFlags(cx, flagStr, &flags)) {
      return false;
    }
  }

  /* Steps 7-8. */
  RegExpShared* shared = CheckPatternSyntax(cx, pattern, flags);
  if (!shared) {
    return false;
  }

  /* Steps 9-12. */
  obj->initIgnoringLastIndex(pattern, flags);

  obj->setShared(shared);

  return true;
}

/* ES 2016 draft Mar 25, 2016 21.2.3.2.3. */
bool js::RegExpCreate(JSContext* cx, HandleValue patternValue,
                      HandleValue flagsValue, MutableHandleValue rval) {
  /* Step 1. */
  Rooted<RegExpObject*> regexp(cx, RegExpAlloc(cx, GenericObject));
  if (!regexp) {
    return false;
  }

  /* Step 2. */
  if (!RegExpInitializeIgnoringLastIndex(cx, regexp, patternValue,
                                         flagsValue)) {
    return false;
  }
  regexp->zeroLastIndex(cx);

  rval.setObject(*regexp);
  return true;
}

MOZ_ALWAYS_INLINE bool IsRegExpObject(HandleValue v) {
  return v.isObject() && v.toObject().is<RegExpObject>();
}

/* ES6 draft rc3 7.2.8. */
bool js::IsRegExp(JSContext* cx, HandleValue value, bool* result) {
  /* Step 1. */
  if (!value.isObject()) {
    *result = false;
    return true;
  }
  RootedObject obj(cx, &value.toObject());

  /* Steps 2-3. */
  RootedValue isRegExp(cx);
  RootedId matchId(cx, PropertyKey::Symbol(cx->wellKnownSymbols().match));
  if (!GetProperty(cx, obj, obj, matchId, &isRegExp)) {
    return false;
  }

  /* Step 4. */
  if (!isRegExp.isUndefined()) {
    *result = ToBoolean(isRegExp);
    return true;
  }

  /* Steps 5-6. */
  ESClass cls;
  if (!GetClassOfValue(cx, value, &cls)) {
    return false;
  }

  *result = cls == ESClass::RegExp;
  return true;
}

// The "lastIndex" property is non-configurable, but it can be made
// non-writable. If CalledFromJit is true, we have emitted guards to ensure it's
// writable.
template <bool CalledFromJit = false>
static bool SetLastIndex(JSContext* cx, Handle<RegExpObject*> regexp,
                         int32_t lastIndex) {
  MOZ_ASSERT(lastIndex >= 0);

  if (CalledFromJit || MOZ_LIKELY(RegExpObject::isInitialShape(regexp)) ||
      regexp->lookupPure(cx->names().lastIndex)->writable()) {
    regexp->setLastIndex(cx, lastIndex);
    return true;
  }

  Rooted<Value> val(cx, Int32Value(lastIndex));
  return SetProperty(cx, regexp, cx->names().lastIndex, val);
}

/* ES6 B.2.5.1. */
MOZ_ALWAYS_INLINE bool regexp_compile_impl(JSContext* cx,
                                           const CallArgs& args) {
  MOZ_ASSERT(IsRegExpObject(args.thisv()));

  Rooted<RegExpObject*> regexp(cx, &args.thisv().toObject().as<RegExpObject>());

  // Step 3.
  RootedValue patternValue(cx, args.get(0));
  ESClass cls;
  if (!GetClassOfValue(cx, patternValue, &cls)) {
    return false;
  }
  if (cls == ESClass::RegExp) {
    // Step 3a.
    if (args.hasDefined(1)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_NEWREGEXP_FLAGGED);
      return false;
    }

    // Beware!  |patternObj| might be a proxy into another compartment, so
    // don't assume |patternObj.is<RegExpObject>()|.  For the same reason,
    // don't reuse the RegExpShared below.
    RootedObject patternObj(cx, &patternValue.toObject());

    Rooted<JSAtom*> sourceAtom(cx);
    RegExpFlags flags = RegExpFlag::NoFlags;
    {
      // Step 3b.
      RegExpShared* shared = RegExpToShared(cx, patternObj);
      if (!shared) {
        return false;
      }

      sourceAtom = shared->getSource();
      flags = shared->getFlags();
    }

    // Step 5, minus lastIndex zeroing.
    regexp->initIgnoringLastIndex(sourceAtom, flags);
  } else {
    // Step 4.
    RootedValue P(cx, patternValue);
    RootedValue F(cx, args.get(1));

    // Step 5, minus lastIndex zeroing.
    if (!RegExpInitializeIgnoringLastIndex(cx, regexp, P, F)) {
      return false;
    }
  }

  // The final niggling bit of step 5.
  //
  // |regexp| is user-exposed, so its "lastIndex" property might be
  // non-writable.
  if (!SetLastIndex(cx, regexp, 0)) {
    return false;
  }

  args.rval().setObject(*regexp);
  return true;
}

static bool regexp_compile(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  /* Steps 1-2. */
  return CallNonGenericMethod<IsRegExpObject, regexp_compile_impl>(cx, args);
}

/*
 * ES 2017 draft rev 6a13789aa9e7c6de4e96b7d3e24d9e6eba6584ad 21.2.3.1.
 */
bool js::regexp_construct(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSConstructorProfilerEntry pseudoFrame(cx, "RegExp");
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1.
  bool patternIsRegExp;
  if (!IsRegExp(cx, args.get(0), &patternIsRegExp)) {
    return false;
  }

  // We can delay step 3 and step 4a until later, during
  // GetPrototypeFromBuiltinConstructor calls. Accessing the new.target
  // and the callee from the stack is unobservable.
  if (!args.isConstructing()) {
    // Step 3.b.
    if (patternIsRegExp && !args.hasDefined(1)) {
      RootedObject patternObj(cx, &args[0].toObject());

      // Step 3.b.i.
      RootedValue patternConstructor(cx);
      if (!GetProperty(cx, patternObj, patternObj, cx->names().constructor,
                       &patternConstructor)) {
        return false;
      }

      // Step 3.b.ii.
      if (patternConstructor.isObject() &&
          patternConstructor.toObject() == args.callee()) {
        args.rval().set(args[0]);
        return true;
      }
    }
  }

  RootedValue patternValue(cx, args.get(0));

  // Step 4.
  ESClass cls;
  if (!GetClassOfValue(cx, patternValue, &cls)) {
    return false;
  }
  if (cls == ESClass::RegExp) {
    // Beware!  |patternObj| might be a proxy into another compartment, so
    // don't assume |patternObj.is<RegExpObject>()|.
    RootedObject patternObj(cx, &patternValue.toObject());

    Rooted<JSAtom*> sourceAtom(cx);
    RegExpFlags flags;
    RootedRegExpShared shared(cx);
    {
      // Step 4.a.
      shared = RegExpToShared(cx, patternObj);
      if (!shared) {
        return false;
      }
      sourceAtom = shared->getSource();

      // Step 4.b.
      // Get original flags in all cases, to compare with passed flags.
      flags = shared->getFlags();

      // If the RegExpShared is in another Zone, don't reuse it.
      if (cx->zone() != shared->zone()) {
        shared = nullptr;
      }
    }

    // Step 7.
    RootedObject proto(cx);
    if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_RegExp, &proto)) {
      return false;
    }

    Rooted<RegExpObject*> regexp(cx, RegExpAlloc(cx, GenericObject, proto));
    if (!regexp) {
      return false;
    }

    // Step 8.
    if (args.hasDefined(1)) {
      // Step 4.c / 21.2.3.2.2 RegExpInitialize step 4.
      RegExpFlags flagsArg = RegExpFlag::NoFlags;
      RootedString flagStr(cx, ToString<CanGC>(cx, args[1]));
      if (!flagStr) {
        return false;
      }
      if (!ParseRegExpFlags(cx, flagStr, &flagsArg)) {
        return false;
      }

      // Don't reuse the RegExpShared if we have different flags.
      if (flags != flagsArg) {
        shared = nullptr;
      }

      if (!flags.unicode() && flagsArg.unicode()) {
        // Have to check syntax again when adding 'u' flag.

        // ES 2017 draft rev 9b49a888e9dfe2667008a01b2754c3662059ae56
        // 21.2.3.2.2 step 7.
        shared = CheckPatternSyntax(cx, sourceAtom, flagsArg);
        if (!shared) {
          return false;
        }
      }
      flags = flagsArg;
    }

    regexp->initAndZeroLastIndex(sourceAtom, flags, cx);

    if (shared) {
      regexp->setShared(shared);
    }

    args.rval().setObject(*regexp);
    return true;
  }

  RootedValue P(cx);
  RootedValue F(cx);

  // Step 5.
  if (patternIsRegExp) {
    RootedObject patternObj(cx, &patternValue.toObject());

    // Step 5.a.
    if (!GetProperty(cx, patternObj, patternObj, cx->names().source, &P)) {
      return false;
    }

    // Step 5.b.
    F = args.get(1);
    if (F.isUndefined()) {
      if (!GetProperty(cx, patternObj, patternObj, cx->names().flags, &F)) {
        return false;
      }
    }
  } else {
    // Steps 6.a-b.
    P = patternValue;
    F = args.get(1);
  }

  // Step 7.
  RootedObject proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_RegExp, &proto)) {
    return false;
  }

  Rooted<RegExpObject*> regexp(cx, RegExpAlloc(cx, GenericObject, proto));
  if (!regexp) {
    return false;
  }

  // Step 8.
  if (!RegExpInitializeIgnoringLastIndex(cx, regexp, P, F)) {
    return false;
  }
  regexp->zeroLastIndex(cx);

  args.rval().setObject(*regexp);
  return true;
}

/*
 * ES 2017 draft rev 6a13789aa9e7c6de4e96b7d3e24d9e6eba6584ad 21.2.3.1
 * steps 4, 7-8.
 */
bool js::regexp_construct_raw_flags(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);
  MOZ_ASSERT(!args.isConstructing());

  // Step 4.a.
  Rooted<JSAtom*> sourceAtom(cx, AtomizeString(cx, args[0].toString()));
  if (!sourceAtom) {
    return false;
  }

  // Step 4.c.
  RegExpFlags flags = AssertedCast<uint8_t>(int32_t(args[1].toNumber()));

  // Step 7.
  RegExpObject* regexp = RegExpAlloc(cx, GenericObject);
  if (!regexp) {
    return false;
  }

  // Step 8.
  regexp->initAndZeroLastIndex(sourceAtom, flags, cx);
  args.rval().setObject(*regexp);
  return true;
}

// This is a specialized implementation of "UnwrapAndTypeCheckThis" for RegExp
// getters that need to return a special value for same-realm
// %RegExp.prototype%.
template <typename Fn>
static bool RegExpGetter(JSContext* cx, CallArgs& args, const char* methodName,
                         Fn&& fn,
                         HandleValue fallbackValue = UndefinedHandleValue) {
  JSObject* obj = nullptr;
  if (args.thisv().isObject()) {
    obj = &args.thisv().toObject();
    if (IsWrapper(obj)) {
      obj = CheckedUnwrapStatic(obj);
      if (!obj) {
        ReportAccessDenied(cx);
        return false;
      }
    }
  }

  if (obj) {
    // Step 4ff
    if (obj->is<RegExpObject>()) {
      return fn(&obj->as<RegExpObject>());
    }

    // Step 3.a. "If SameValue(R, %RegExp.prototype%) is true, return
    // undefined."
    // Or `return "(?:)"` for get RegExp.prototype.source.
    if (obj == cx->global()->maybeGetRegExpPrototype()) {
      args.rval().set(fallbackValue);
      return true;
    }

    // fall-through
  }

  // Step 2. and Step 3.b.
  JS_ReportErrorNumberLatin1(cx, GetErrorMessage, nullptr,
                             JSMSG_INCOMPATIBLE_REGEXP_GETTER, methodName,
                             InformalValueTypeName(args.thisv()));
  return false;
}

bool js::regexp_hasIndices(JSContext* cx, unsigned argc, JS::Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return RegExpGetter(cx, args, "hasIndices", [args](RegExpObject* unwrapped) {
    args.rval().setBoolean(unwrapped->hasIndices());
    return true;
  });
}

// ES2021 draft rev 0b3a808af87a9123890767152a26599cc8fde161
// 21.2.5.5 get RegExp.prototype.global
bool js::regexp_global(JSContext* cx, unsigned argc, JS::Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return RegExpGetter(cx, args, "global", [args](RegExpObject* unwrapped) {
    args.rval().setBoolean(unwrapped->global());
    return true;
  });
}

// ES2021 draft rev 0b3a808af87a9123890767152a26599cc8fde161
// 21.2.5.6 get RegExp.prototype.ignoreCase
bool js::regexp_ignoreCase(JSContext* cx, unsigned argc, JS::Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return RegExpGetter(cx, args, "ignoreCase", [args](RegExpObject* unwrapped) {
    args.rval().setBoolean(unwrapped->ignoreCase());
    return true;
  });
}

// ES2021 draft rev 0b3a808af87a9123890767152a26599cc8fde161
// 21.2.5.9 get RegExp.prototype.multiline
bool js::regexp_multiline(JSContext* cx, unsigned argc, JS::Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return RegExpGetter(cx, args, "multiline", [args](RegExpObject* unwrapped) {
    args.rval().setBoolean(unwrapped->multiline());
    return true;
  });
}

// ES2021 draft rev 0b3a808af87a9123890767152a26599cc8fde161
// 21.2.5.12 get RegExp.prototype.source
static bool regexp_source(JSContext* cx, unsigned argc, JS::Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  // Step 3.a. Return "(?:)" for %RegExp.prototype%.
  RootedValue fallback(cx, StringValue(cx->names().emptyRegExp_));
  return RegExpGetter(
      cx, args, "source",
      [cx, args](RegExpObject* unwrapped) {
        Rooted<JSAtom*> src(cx, unwrapped->getSource());
        MOZ_ASSERT(src);
        // Mark potentially cross-zone JSAtom.
        if (cx->zone() != unwrapped->zone()) {
          cx->markAtom(src);
        }

        // Step 7.
        JSString* escaped = EscapeRegExpPattern(cx, src);
        if (!escaped) {
          return false;
        }

        args.rval().setString(escaped);
        return true;
      },
      fallback);
}

// ES2021 draft rev 0b3a808af87a9123890767152a26599cc8fde161
// 21.2.5.3 get RegExp.prototype.dotAll
bool js::regexp_dotAll(JSContext* cx, unsigned argc, JS::Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return RegExpGetter(cx, args, "dotAll", [args](RegExpObject* unwrapped) {
    args.rval().setBoolean(unwrapped->dotAll());
    return true;
  });
}

// ES2021 draft rev 0b3a808af87a9123890767152a26599cc8fde161
// 21.2.5.14 get RegExp.prototype.sticky
bool js::regexp_sticky(JSContext* cx, unsigned argc, JS::Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return RegExpGetter(cx, args, "sticky", [args](RegExpObject* unwrapped) {
    args.rval().setBoolean(unwrapped->sticky());
    return true;
  });
}

// ES2021 draft rev 0b3a808af87a9123890767152a26599cc8fde161
// 21.2.5.17 get RegExp.prototype.unicode
bool js::regexp_unicode(JSContext* cx, unsigned argc, JS::Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return RegExpGetter(cx, args, "unicode", [args](RegExpObject* unwrapped) {
    args.rval().setBoolean(unwrapped->unicode());
    return true;
  });
}

// https://arai-a.github.io/ecma262-compare/?pr=2418&id=sec-get-regexp.prototype.unicodesets
// 21.2.6.19 get RegExp.prototype.unicodeSets
bool js::regexp_unicodeSets(JSContext* cx, unsigned argc, JS::Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return RegExpGetter(cx, args, "unicodeSets", [args](RegExpObject* unwrapped) {
    args.rval().setBoolean(unwrapped->unicodeSets());
    return true;
  });
}

const JSPropertySpec js::regexp_properties[] = {
    JS_SELF_HOSTED_GET("flags", "$RegExpFlagsGetter", 0),
    JS_PSG("hasIndices", regexp_hasIndices, 0),
    JS_PSG("global", regexp_global, 0),
    JS_PSG("ignoreCase", regexp_ignoreCase, 0),
    JS_PSG("multiline", regexp_multiline, 0),
    JS_PSG("dotAll", regexp_dotAll, 0),
    JS_PSG("source", regexp_source, 0),
    JS_PSG("sticky", regexp_sticky, 0),
    JS_PSG("unicode", regexp_unicode, 0),
    JS_PSG("unicodeSets", regexp_unicodeSets, 0),
    JS_PS_END};

const JSFunctionSpec js::regexp_methods[] = {
    JS_SELF_HOSTED_FN("toSource", "$RegExpToString", 0, 0),
    JS_SELF_HOSTED_FN("toString", "$RegExpToString", 0, 0),
    JS_FN("compile", regexp_compile, 2, 0),
    JS_SELF_HOSTED_FN("exec", "RegExp_prototype_Exec", 1, 0),
    JS_SELF_HOSTED_FN("test", "RegExpTest", 1, 0),
    JS_SELF_HOSTED_SYM_FN(match, "RegExpMatch", 1, 0),
    JS_SELF_HOSTED_SYM_FN(matchAll, "RegExpMatchAll", 1, 0),
    JS_SELF_HOSTED_SYM_FN(replace, "RegExpReplace", 2, 0),
    JS_SELF_HOSTED_SYM_FN(search, "RegExpSearch", 1, 0),
    JS_SELF_HOSTED_SYM_FN(split, "RegExpSplit", 2, 0),
    JS_FS_END};

#define STATIC_PAREN_GETTER_CODE(parenNum)                        \
  if (!res->createParen(cx, parenNum, args.rval())) return false; \
  if (args.rval().isUndefined())                                  \
    args.rval().setString(cx->runtime()->emptyString);            \
  return true

/*
 * RegExp static properties.
 *
 * RegExp class static properties and their Perl counterparts:
 *
 *  RegExp.input                $_
 *  RegExp.lastMatch            $&
 *  RegExp.lastParen            $+
 *  RegExp.leftContext          $`
 *  RegExp.rightContext         $'
 */

#define DEFINE_STATIC_GETTER(name, code)                                   \
  static bool name(JSContext* cx, unsigned argc, Value* vp) {              \
    CallArgs args = CallArgsFromVp(argc, vp);                              \
    RegExpStatics* res = GlobalObject::getRegExpStatics(cx, cx->global()); \
    if (!res) return false;                                                \
    code;                                                                  \
  }

DEFINE_STATIC_GETTER(static_input_getter,
                     return res->createPendingInput(cx, args.rval()))
DEFINE_STATIC_GETTER(static_lastMatch_getter,
                     return res->createLastMatch(cx, args.rval()))
DEFINE_STATIC_GETTER(static_lastParen_getter,
                     return res->createLastParen(cx, args.rval()))
DEFINE_STATIC_GETTER(static_leftContext_getter,
                     return res->createLeftContext(cx, args.rval()))
DEFINE_STATIC_GETTER(static_rightContext_getter,
                     return res->createRightContext(cx, args.rval()))

DEFINE_STATIC_GETTER(static_paren1_getter, STATIC_PAREN_GETTER_CODE(1))
DEFINE_STATIC_GETTER(static_paren2_getter, STATIC_PAREN_GETTER_CODE(2))
DEFINE_STATIC_GETTER(static_paren3_getter, STATIC_PAREN_GETTER_CODE(3))
DEFINE_STATIC_GETTER(static_paren4_getter, STATIC_PAREN_GETTER_CODE(4))
DEFINE_STATIC_GETTER(static_paren5_getter, STATIC_PAREN_GETTER_CODE(5))
DEFINE_STATIC_GETTER(static_paren6_getter, STATIC_PAREN_GETTER_CODE(6))
DEFINE_STATIC_GETTER(static_paren7_getter, STATIC_PAREN_GETTER_CODE(7))
DEFINE_STATIC_GETTER(static_paren8_getter, STATIC_PAREN_GETTER_CODE(8))
DEFINE_STATIC_GETTER(static_paren9_getter, STATIC_PAREN_GETTER_CODE(9))

#define DEFINE_STATIC_SETTER(name, code)                                   \
  static bool name(JSContext* cx, unsigned argc, Value* vp) {              \
    RegExpStatics* res = GlobalObject::getRegExpStatics(cx, cx->global()); \
    if (!res) return false;                                                \
    code;                                                                  \
    return true;                                                           \
  }

static bool static_input_setter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  RegExpStatics* res = GlobalObject::getRegExpStatics(cx, cx->global());
  if (!res) {
    return false;
  }

  RootedString str(cx, ToString<CanGC>(cx, args.get(0)));
  if (!str) {
    return false;
  }

  res->setPendingInput(str);
  args.rval().setString(str);
  return true;
}

const JSPropertySpec js::regexp_static_props[] = {
    JS_PSGS("input", static_input_getter, static_input_setter,
            JSPROP_PERMANENT | JSPROP_ENUMERATE),
    JS_PSG("lastMatch", static_lastMatch_getter,
           JSPROP_PERMANENT | JSPROP_ENUMERATE),
    JS_PSG("lastParen", static_lastParen_getter,
           JSPROP_PERMANENT | JSPROP_ENUMERATE),
    JS_PSG("leftContext", static_leftContext_getter,
           JSPROP_PERMANENT | JSPROP_ENUMERATE),
    JS_PSG("rightContext", static_rightContext_getter,
           JSPROP_PERMANENT | JSPROP_ENUMERATE),
    JS_PSG("$1", static_paren1_getter, JSPROP_PERMANENT | JSPROP_ENUMERATE),
    JS_PSG("$2", static_paren2_getter, JSPROP_PERMANENT | JSPROP_ENUMERATE),
    JS_PSG("$3", static_paren3_getter, JSPROP_PERMANENT | JSPROP_ENUMERATE),
    JS_PSG("$4", static_paren4_getter, JSPROP_PERMANENT | JSPROP_ENUMERATE),
    JS_PSG("$5", static_paren5_getter, JSPROP_PERMANENT | JSPROP_ENUMERATE),
    JS_PSG("$6", static_paren6_getter, JSPROP_PERMANENT | JSPROP_ENUMERATE),
    JS_PSG("$7", static_paren7_getter, JSPROP_PERMANENT | JSPROP_ENUMERATE),
    JS_PSG("$8", static_paren8_getter, JSPROP_PERMANENT | JSPROP_ENUMERATE),
    JS_PSG("$9", static_paren9_getter, JSPROP_PERMANENT | JSPROP_ENUMERATE),
    JS_PSGS("$_", static_input_getter, static_input_setter, JSPROP_PERMANENT),
    JS_PSG("$&", static_lastMatch_getter, JSPROP_PERMANENT),
    JS_PSG("$+", static_lastParen_getter, JSPROP_PERMANENT),
    JS_PSG("$`", static_leftContext_getter, JSPROP_PERMANENT),
    JS_PSG("$'", static_rightContext_getter, JSPROP_PERMANENT),
    JS_SELF_HOSTED_SYM_GET(species, "$RegExpSpecies", 0),
    JS_PS_END};

/*
 * ES 2017 draft rev 6a13789aa9e7c6de4e96b7d3e24d9e6eba6584ad 21.2.5.2.2
 * steps 3, 9-14, except 12.a.i, 12.c.i.1.
 */
static RegExpRunStatus ExecuteRegExp(JSContext* cx, HandleObject regexp,
                                     HandleString string, int32_t lastIndex,
                                     VectorMatchPairs* matches) {
  /*
   * WARNING: Despite the presence of spec step comment numbers, this
   *          algorithm isn't consistent with any ES6 version, draft or
   *          otherwise.  YOU HAVE BEEN WARNED.
   */

  /* Steps 1-2 performed by the caller. */
  Handle<RegExpObject*> reobj = regexp.as<RegExpObject>();

  RootedRegExpShared re(cx, RegExpObject::getShared(cx, reobj));
  if (!re) {
    return RegExpRunStatus::Error;
  }

  RegExpStatics* res = GlobalObject::getRegExpStatics(cx, cx->global());
  if (!res) {
    return RegExpRunStatus::Error;
  }

  Rooted<JSLinearString*> input(cx, string->ensureLinear(cx));
  if (!input) {
    return RegExpRunStatus::Error;
  }

  /* Handled by caller */
  MOZ_ASSERT(lastIndex >= 0 && size_t(lastIndex) <= input->length());

  /* Steps 4-8 performed by the caller. */

  /* Steps 3, 10-14, except 12.a.i, 12.c.i.1. */
  RegExpRunStatus status =
      ExecuteRegExpImpl(cx, res, &re, input, lastIndex, matches);
  if (status == RegExpRunStatus::Error) {
    return RegExpRunStatus::Error;
  }

  /* Steps 12.a.i, 12.c.i.i, 15 are done by Self-hosted function. */

  return status;
}

/*
 * ES 2017 draft rev 6a13789aa9e7c6de4e96b7d3e24d9e6eba6584ad 21.2.5.2.2
 * steps 3, 9-25, except 12.a.i, 12.c.i.1, 15.
 */
static bool RegExpMatcherImpl(JSContext* cx, HandleObject regexp,
                              HandleString string, int32_t lastIndex,
                              MutableHandleValue rval) {
  /* Execute regular expression and gather matches. */
  VectorMatchPairs matches;

  /* Steps 3, 9-14, except 12.a.i, 12.c.i.1. */
  RegExpRunStatus status =
      ExecuteRegExp(cx, regexp, string, lastIndex, &matches);
  if (status == RegExpRunStatus::Error) {
    return false;
  }

  /* Steps 12.a, 12.c. */
  if (status == RegExpRunStatus::Success_NotFound) {
    rval.setNull();
    return true;
  }

  /* Steps 16-25 */
  RootedRegExpShared shared(cx, regexp->as<RegExpObject>().getShared());
  return CreateRegExpMatchResult(cx, shared, string, matches, rval);
}

/*
 * ES 2017 draft rev 6a13789aa9e7c6de4e96b7d3e24d9e6eba6584ad 21.2.5.2.2
 * steps 3, 9-25, except 12.a.i, 12.c.i.1, 15.
 */
bool js::RegExpMatcher(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 3);
  MOZ_ASSERT(IsRegExpObject(args[0]));
  MOZ_ASSERT(args[1].isString());
  MOZ_ASSERT(args[2].isNumber());

  RootedObject regexp(cx, &args[0].toObject());
  RootedString string(cx, args[1].toString());

  int32_t lastIndex;
  MOZ_ALWAYS_TRUE(ToInt32(cx, args[2], &lastIndex));

  /* Steps 3, 9-25, except 12.a.i, 12.c.i.1, 15. */
  return RegExpMatcherImpl(cx, regexp, string, lastIndex, args.rval());
}

/*
 * Separate interface for use by the JITs.
 * This code cannot re-enter JIT code.
 */
bool js::RegExpMatcherRaw(JSContext* cx, HandleObject regexp,
                          HandleString input, int32_t lastIndex,
                          MatchPairs* maybeMatches, MutableHandleValue output) {
  MOZ_ASSERT(lastIndex >= 0 && size_t(lastIndex) <= input->length());

  // RegExp execution was successful only if the pairs have actually been
  // filled in. Note that IC code always passes a nullptr maybeMatches.
  if (maybeMatches && maybeMatches->pairsRaw()[0] > MatchPair::NoMatch) {
    RootedRegExpShared shared(cx, regexp->as<RegExpObject>().getShared());
    return CreateRegExpMatchResult(cx, shared, input, *maybeMatches, output);
  }
  return RegExpMatcherImpl(cx, regexp, input, lastIndex, output);
}

/*
 * ES 2017 draft rev 6a13789aa9e7c6de4e96b7d3e24d9e6eba6584ad 21.2.5.2.2
 * steps 3, 9-25, except 12.a.i, 12.c.i.1, 15.
 * This code is inlined in CodeGenerator.cpp generateRegExpSearcherStub,
 * changes to this code need to get reflected in there too.
 */
static bool RegExpSearcherImpl(JSContext* cx, HandleObject regexp,
                               HandleString string, int32_t lastIndex,
                               int32_t* result) {
  /* Execute regular expression and gather matches. */
  VectorMatchPairs matches;

#ifdef DEBUG
  // Ensure we assert if RegExpSearcherLastLimit is called when there's no
  // match.
  cx->regExpSearcherLastLimit = RegExpSearcherLastLimitSentinel;
#endif

  /* Steps 3, 9-14, except 12.a.i, 12.c.i.1. */
  RegExpRunStatus status =
      ExecuteRegExp(cx, regexp, string, lastIndex, &matches);
  if (status == RegExpRunStatus::Error) {
    return false;
  }

  /* Steps 12.a, 12.c. */
  if (status == RegExpRunStatus::Success_NotFound) {
    *result = -1;
    return true;
  }

  /* Steps 16-25 */
  *result = CreateRegExpSearchResult(cx, matches);
  return true;
}

/*
 * ES 2017 draft rev 6a13789aa9e7c6de4e96b7d3e24d9e6eba6584ad 21.2.5.2.2
 * steps 3, 9-25, except 12.a.i, 12.c.i.1, 15.
 */
bool js::RegExpSearcher(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 3);
  MOZ_ASSERT(IsRegExpObject(args[0]));
  MOZ_ASSERT(args[1].isString());
  MOZ_ASSERT(args[2].isNumber());

  RootedObject regexp(cx, &args[0].toObject());
  RootedString string(cx, args[1].toString());

  int32_t lastIndex;
  MOZ_ALWAYS_TRUE(ToInt32(cx, args[2], &lastIndex));

  /* Steps 3, 9-25, except 12.a.i, 12.c.i.1, 15. */
  int32_t result = 0;
  if (!RegExpSearcherImpl(cx, regexp, string, lastIndex, &result)) {
    return false;
  }

  args.rval().setInt32(result);
  return true;
}

/*
 * Separate interface for use by the JITs.
 * This code cannot re-enter JIT code.
 */
bool js::RegExpSearcherRaw(JSContext* cx, HandleObject regexp,
                           HandleString input, int32_t lastIndex,
                           MatchPairs* maybeMatches, int32_t* result) {
  MOZ_ASSERT(lastIndex >= 0);

  // RegExp execution was successful only if the pairs have actually been
  // filled in. Note that IC code always passes a nullptr maybeMatches.
  if (maybeMatches && maybeMatches->pairsRaw()[0] > MatchPair::NoMatch) {
    *result = CreateRegExpSearchResult(cx, *maybeMatches);
    return true;
  }
  return RegExpSearcherImpl(cx, regexp, input, lastIndex, result);
}

bool js::RegExpSearcherLastLimit(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].isString());

  // Assert the limit is not the sentinel value and is valid for this string.
  MOZ_ASSERT(cx->regExpSearcherLastLimit != RegExpSearcherLastLimitSentinel);
  MOZ_ASSERT(cx->regExpSearcherLastLimit <= args[0].toString()->length());

  args.rval().setInt32(cx->regExpSearcherLastLimit);

#ifdef DEBUG
  // Ensure we assert if this function is called again without a new call to
  // RegExpSearcher.
  cx->regExpSearcherLastLimit = RegExpSearcherLastLimitSentinel;
#endif
  return true;
}

template <bool CalledFromJit>
static bool RegExpBuiltinExecMatchRaw(JSContext* cx,
                                      Handle<RegExpObject*> regexp,
                                      HandleString input, int32_t lastIndex,
                                      MatchPairs* maybeMatches,
                                      MutableHandleValue output) {
  MOZ_ASSERT(lastIndex >= 0);
  MOZ_ASSERT(size_t(lastIndex) <= input->length());
  MOZ_ASSERT_IF(!CalledFromJit, !maybeMatches);

  // RegExp execution was successful only if the pairs have actually been
  // filled in. Note that IC code always passes a nullptr maybeMatches.
  int32_t lastIndexNew = 0;
  if (CalledFromJit && maybeMatches &&
      maybeMatches->pairsRaw()[0] > MatchPair::NoMatch) {
    RootedRegExpShared shared(cx, regexp->as<RegExpObject>().getShared());
    if (!CreateRegExpMatchResult(cx, shared, input, *maybeMatches, output)) {
      return false;
    }
    lastIndexNew = (*maybeMatches)[0].limit;
  } else {
    VectorMatchPairs matches;
    RegExpRunStatus status =
        ExecuteRegExp(cx, regexp, input, lastIndex, &matches);
    if (status == RegExpRunStatus::Error) {
      return false;
    }
    if (status == RegExpRunStatus::Success_NotFound) {
      output.setNull();
      lastIndexNew = 0;
    } else {
      RootedRegExpShared shared(cx, regexp->as<RegExpObject>().getShared());
      if (!CreateRegExpMatchResult(cx, shared, input, matches, output)) {
        return false;
      }
      lastIndexNew = matches[0].limit;
    }
  }

  RegExpFlags flags = regexp->getFlags();
  if (!flags.global() && !flags.sticky()) {
    return true;
  }

  return SetLastIndex<CalledFromJit>(cx, regexp, lastIndexNew);
}

bool js::RegExpBuiltinExecMatchFromJit(JSContext* cx,
                                       Handle<RegExpObject*> regexp,
                                       HandleString input,
                                       MatchPairs* maybeMatches,
                                       MutableHandleValue output) {
  int32_t lastIndex = 0;
  if (regexp->isGlobalOrSticky()) {
    lastIndex = regexp->getLastIndex().toInt32();
    MOZ_ASSERT(lastIndex >= 0);
    if (size_t(lastIndex) > input->length()) {
      output.setNull();
      return SetLastIndex<true>(cx, regexp, 0);
    }
  }
  return RegExpBuiltinExecMatchRaw<true>(cx, regexp, input, lastIndex,
                                         maybeMatches, output);
}

template <bool CalledFromJit>
static bool RegExpBuiltinExecTestRaw(JSContext* cx,
                                     Handle<RegExpObject*> regexp,
                                     HandleString input, int32_t lastIndex,
                                     bool* result) {
  MOZ_ASSERT(lastIndex >= 0);
  MOZ_ASSERT(size_t(lastIndex) <= input->length());

  VectorMatchPairs matches;
  RegExpRunStatus status =
      ExecuteRegExp(cx, regexp, input, lastIndex, &matches);
  if (status == RegExpRunStatus::Error) {
    return false;
  }

  *result = (status == RegExpRunStatus::Success);

  RegExpFlags flags = regexp->getFlags();
  if (!flags.global() && !flags.sticky()) {
    return true;
  }

  int32_t lastIndexNew = *result ? matches[0].limit : 0;
  return SetLastIndex<CalledFromJit>(cx, regexp, lastIndexNew);
}

bool js::RegExpBuiltinExecTestFromJit(JSContext* cx,
                                      Handle<RegExpObject*> regexp,
                                      HandleString input, bool* result) {
  int32_t lastIndex = 0;
  if (regexp->isGlobalOrSticky()) {
    lastIndex = regexp->getLastIndex().toInt32();
    MOZ_ASSERT(lastIndex >= 0);
    if (size_t(lastIndex) > input->length()) {
      *result = false;
      return SetLastIndex<true>(cx, regexp, 0);
    }
  }
  return RegExpBuiltinExecTestRaw<true>(cx, regexp, input, lastIndex, result);
}

using CapturesVector = GCVector<Value, 4>;

struct JSSubString {
  JSLinearString* base = nullptr;
  size_t offset = 0;
  size_t length = 0;

  JSSubString() = default;

  void initEmpty(JSLinearString* base) {
    this->base = base;
    offset = length = 0;
  }
  void init(JSLinearString* base, size_t offset, size_t length) {
    this->base = base;
    this->offset = offset;
    this->length = length;
  }
};

static void GetParen(JSLinearString* matched, const JS::Value& capture,
                     JSSubString* out) {
  if (capture.isUndefined()) {
    out->initEmpty(matched);
    return;
  }
  JSLinearString& captureLinear = capture.toString()->asLinear();
  out->init(&captureLinear, 0, captureLinear.length());
}

template <typename CharT>
static bool InterpretDollar(JSLinearString* matched, JSLinearString* string,
                            size_t position, size_t tailPos,
                            Handle<CapturesVector> captures,
                            Handle<CapturesVector> namedCaptures,
                            JSLinearString* replacement,
                            const CharT* replacementBegin,
                            const CharT* currentDollar,
                            const CharT* replacementEnd, JSSubString* out,
                            size_t* skip, uint32_t* currentNamedCapture) {
  MOZ_ASSERT(*currentDollar == '$');

  /* If there is only a dollar, bail now. */
  if (currentDollar + 1 >= replacementEnd) {
    return false;
  }

  // ES 2021 Table 57: Replacement Text Symbol Substitutions
  // https://tc39.es/ecma262/#table-replacement-text-symbol-substitutions
  char16_t c = currentDollar[1];
  if (IsAsciiDigit(c)) {
    /* $n, $nn */
    unsigned num = AsciiDigitToNumber(c);
    if (num > captures.length()) {
      // The result is implementation-defined. Do not substitute.
      return false;
    }

    const CharT* currentChar = currentDollar + 2;
    if (currentChar < replacementEnd) {
      c = *currentChar;
      if (IsAsciiDigit(c)) {
        unsigned tmpNum = 10 * num + AsciiDigitToNumber(c);
        // If num > captures.length(), the result is implementation-defined.
        // Consume next character only if num <= captures.length().
        if (tmpNum <= captures.length()) {
          currentChar++;
          num = tmpNum;
        }
      }
    }

    if (num == 0) {
      // The result is implementation-defined. Do not substitute.
      return false;
    }

    *skip = currentChar - currentDollar;

    MOZ_ASSERT(num <= captures.length());

    GetParen(matched, captures[num - 1], out);
    return true;
  }

  // '$<': Named Captures
  if (c == '<') {
    // Step 1.
    if (namedCaptures.length() == 0) {
      return false;
    }

    // Step 2.b
    const CharT* nameStart = currentDollar + 2;
    const CharT* nameEnd = js_strchr_limit(nameStart, '>', replacementEnd);

    // Step 2.c
    if (!nameEnd) {
      return false;
    }

    // Step 2.d
    // We precompute named capture replacements in InitNamedCaptures.
    // They are stored in the order in which we will need them, so here
    // we can just take the next one in the list.
    size_t nameLength = nameEnd - nameStart;
    *skip = nameLength + 3;  // $<...>

    // Steps 2.d.iii-iv
    GetParen(matched, namedCaptures[*currentNamedCapture], out);
    *currentNamedCapture += 1;
    return true;
  }

  switch (c) {
    default:
      return false;
    case '$':
      out->init(replacement, currentDollar - replacementBegin, 1);
      break;
    case '&':
      out->init(matched, 0, matched->length());
      break;
    case '`':
      out->init(string, 0, position);
      break;
    case '\'':
      if (tailPos >= string->length()) {
        out->initEmpty(matched);
      } else {
        out->init(string, tailPos, string->length() - tailPos);
      }
      break;
  }

  *skip = 2;
  return true;
}

template <typename CharT>
static bool FindReplaceLengthString(JSContext* cx,
                                    Handle<JSLinearString*> matched,
                                    Handle<JSLinearString*> string,
                                    size_t position, size_t tailPos,
                                    Handle<CapturesVector> captures,
                                    Handle<CapturesVector> namedCaptures,
                                    Handle<JSLinearString*> replacement,
                                    size_t firstDollarIndex, size_t* sizep) {
  CheckedInt<uint32_t> replen = replacement->length();

  JS::AutoCheckCannotGC nogc;
  MOZ_ASSERT(firstDollarIndex < replacement->length());
  const CharT* replacementBegin = replacement->chars<CharT>(nogc);
  const CharT* currentDollar = replacementBegin + firstDollarIndex;
  const CharT* replacementEnd = replacementBegin + replacement->length();
  uint32_t currentNamedCapture = 0;
  do {
    JSSubString sub;
    size_t skip;
    if (InterpretDollar(matched, string, position, tailPos, captures,
                        namedCaptures, replacement, replacementBegin,
                        currentDollar, replacementEnd, &sub, &skip,
                        &currentNamedCapture)) {
      if (sub.length > skip) {
        replen += sub.length - skip;
      } else {
        replen -= skip - sub.length;
      }
      currentDollar += skip;
    } else {
      currentDollar++;
    }

    currentDollar = js_strchr_limit(currentDollar, '$', replacementEnd);
  } while (currentDollar);

  if (!replen.isValid()) {
    ReportAllocationOverflow(cx);
    return false;
  }

  *sizep = replen.value();
  return true;
}

static bool FindReplaceLength(JSContext* cx, Handle<JSLinearString*> matched,
                              Handle<JSLinearString*> string, size_t position,
                              size_t tailPos, Handle<CapturesVector> captures,
                              Handle<CapturesVector> namedCaptures,
                              Handle<JSLinearString*> replacement,
                              size_t firstDollarIndex, size_t* sizep) {
  return replacement->hasLatin1Chars()
             ? FindReplaceLengthString<Latin1Char>(
                   cx, matched, string, position, tailPos, captures,
                   namedCaptures, replacement, firstDollarIndex, sizep)
             : FindReplaceLengthString<char16_t>(
                   cx, matched, string, position, tailPos, captures,
                   namedCaptures, replacement, firstDollarIndex, sizep);
}

/*
 * Precondition: |sb| already has necessary growth space reserved (as
 * derived from FindReplaceLength), and has been inflated to TwoByte if
 * necessary.
 */
template <typename CharT>
static void DoReplace(Handle<JSLinearString*> matched,
                      Handle<JSLinearString*> string, size_t position,
                      size_t tailPos, Handle<CapturesVector> captures,
                      Handle<CapturesVector> namedCaptures,
                      Handle<JSLinearString*> replacement,
                      size_t firstDollarIndex, StringBuffer& sb) {
  JS::AutoCheckCannotGC nogc;
  const CharT* replacementBegin = replacement->chars<CharT>(nogc);
  const CharT* currentChar = replacementBegin;

  MOZ_ASSERT(firstDollarIndex < replacement->length());
  const CharT* currentDollar = replacementBegin + firstDollarIndex;
  const CharT* replacementEnd = replacementBegin + replacement->length();
  uint32_t currentNamedCapture = 0;
  do {
    /* Move one of the constant portions of the replacement value. */
    size_t len = currentDollar - currentChar;
    sb.infallibleAppend(currentChar, len);
    currentChar = currentDollar;

    JSSubString sub;
    size_t skip;
    if (InterpretDollar(matched, string, position, tailPos, captures,
                        namedCaptures, replacement, replacementBegin,
                        currentDollar, replacementEnd, &sub, &skip,
                        &currentNamedCapture)) {
      sb.infallibleAppendSubstring(sub.base, sub.offset, sub.length);
      currentChar += skip;
      currentDollar += skip;
    } else {
      currentDollar++;
    }

    currentDollar = js_strchr_limit(currentDollar, '$', replacementEnd);
  } while (currentDollar);
  sb.infallibleAppend(currentChar,
                      replacement->length() - (currentChar - replacementBegin));
}

/*
 * This function finds the list of named captures of the form
 * "$<name>" in a replacement string and converts them into jsids, for
 * use in InitNamedReplacements.
 */
template <typename CharT>
static bool CollectNames(JSContext* cx, Handle<JSLinearString*> replacement,
                         size_t firstDollarIndex,
                         MutableHandle<GCVector<jsid>> names) {
  JS::AutoCheckCannotGC nogc;
  MOZ_ASSERT(firstDollarIndex < replacement->length());

  const CharT* replacementBegin = replacement->chars<CharT>(nogc);
  const CharT* currentDollar = replacementBegin + firstDollarIndex;
  const CharT* replacementEnd = replacementBegin + replacement->length();

  // https://tc39.es/ecma262/#table-45, "$<" section
  while (currentDollar && currentDollar + 1 < replacementEnd) {
    if (currentDollar[1] == '<') {
      // Step 2.b
      const CharT* nameStart = currentDollar + 2;
      const CharT* nameEnd = js_strchr_limit(nameStart, '>', replacementEnd);

      // Step 2.c
      if (!nameEnd) {
        return true;
      }

      // Step 2.d.i
      size_t nameLength = nameEnd - nameStart;
      JSAtom* atom = AtomizeChars(cx, nameStart, nameLength);
      if (!atom || !names.append(AtomToId(atom))) {
        return false;
      }
      currentDollar = nameEnd + 1;
    } else {
      currentDollar += 2;
    }
    currentDollar = js_strchr_limit(currentDollar, '$', replacementEnd);
  }
  return true;
}

/*
 * When replacing named captures, the spec requires us to perform
 * `Get(match.groups, name)` for each "$<name>". These `Get`s can be
 * script-visible; for example, RegExp can be extended with an `exec`
 * method that wraps `groups` in a proxy. To make sure that we do the
 * right thing, if a regexp has named captures, we find the named
 * capture replacements before beginning the actual replacement.
 * This guarantees that we will call GetProperty once and only once for
 * each "$<name>" in the replacement string, in the correct order.
 *
 * This function precomputes the results of step 2 of the '$<' case
 * here: https://tc39.es/proposal-regexp-named-groups/#table-45, so
 * that when we need to access the nth named capture in InterpretDollar,
 * we can just use the nth value stored in namedCaptures.
 */
static bool InitNamedCaptures(JSContext* cx,
                              Handle<JSLinearString*> replacement,
                              HandleObject groups, size_t firstDollarIndex,
                              MutableHandle<CapturesVector> namedCaptures) {
  Rooted<GCVector<jsid>> names(cx, cx);
  if (replacement->hasLatin1Chars()) {
    if (!CollectNames<Latin1Char>(cx, replacement, firstDollarIndex, &names)) {
      return false;
    }
  } else {
    if (!CollectNames<char16_t>(cx, replacement, firstDollarIndex, &names)) {
      return false;
    }
  }

  // https://tc39.es/ecma262/#table-45, "$<" section
  RootedId id(cx);
  RootedValue capture(cx);
  for (uint32_t i = 0; i < names.length(); i++) {
    // Step 2.d.i
    id = names[i];

    // Step 2.d.ii
    if (!GetProperty(cx, groups, groups, id, &capture)) {
      return false;
    }

    // Step 2.d.iii
    if (capture.isUndefined()) {
      if (!namedCaptures.append(capture)) {
        return false;
      }
    } else {
      // Step 2.d.iv
      JSString* str = ToString<CanGC>(cx, capture);
      if (!str) {
        return false;
      }
      JSLinearString* linear = str->ensureLinear(cx);
      if (!linear) {
        return false;
      }
      if (!namedCaptures.append(StringValue(linear))) {
        return false;
      }
    }
  }

  return true;
}

static bool NeedTwoBytes(Handle<JSLinearString*> string,
                         Handle<JSLinearString*> replacement,
                         Handle<JSLinearString*> matched,
                         Handle<CapturesVector> captures,
                         Handle<CapturesVector> namedCaptures) {
  if (string->hasTwoByteChars()) {
    return true;
  }
  if (replacement->hasTwoByteChars()) {
    return true;
  }
  if (matched->hasTwoByteChars()) {
    return true;
  }

  for (const Value& capture : captures) {
    if (capture.isUndefined()) {
      continue;
    }
    if (capture.toString()->hasTwoByteChars()) {
      return true;
    }
  }

  for (const Value& capture : namedCaptures) {
    if (capture.isUndefined()) {
      continue;
    }
    if (capture.toString()->hasTwoByteChars()) {
      return true;
    }
  }

  return false;
}

// ES2024 draft rev d4927f9bc3706484c75dfef4bbcf5ba826d2632e
//
// 22.2.7.2 RegExpBuiltinExec ( R, S )
// https://tc39.es/ecma262/#sec-regexpbuiltinexec
//
// If `forTest` is true, this is called from `RegExp.prototype.test` and we can
// avoid allocating a result object.
bool js::RegExpBuiltinExec(JSContext* cx, Handle<RegExpObject*> regexp,
                           Handle<JSString*> string, bool forTest,
                           MutableHandle<Value> rval) {
  // Step 2.
  uint64_t lastIndex;
  if (MOZ_LIKELY(regexp->getLastIndex().isInt32())) {
    lastIndex = std::max(regexp->getLastIndex().toInt32(), 0);
  } else {
    Rooted<Value> lastIndexVal(cx, regexp->getLastIndex());
    if (!ToLength(cx, lastIndexVal, &lastIndex)) {
      return false;
    }
  }

  // Steps 3-5.
  bool globalOrSticky = regexp->isGlobalOrSticky();

  // Step 7.
  if (!globalOrSticky) {
    lastIndex = 0;
  } else {
    // Steps 1, 13.a.
    if (lastIndex > string->length()) {
      if (!SetLastIndex(cx, regexp, 0)) {
        return false;
      }
      rval.set(forTest ? BooleanValue(false) : NullValue());
      return true;
    }
  }

  MOZ_ASSERT(lastIndex <= string->length());
  static_assert(JSString::MAX_LENGTH <= INT32_MAX, "lastIndex fits in int32_t");

  // Steps 6, 8-35.

  if (forTest) {
    bool result;
    if (!RegExpBuiltinExecTestRaw<false>(cx, regexp, string, int32_t(lastIndex),
                                         &result)) {
      return false;
    }
    rval.setBoolean(result);
    return true;
  }

  return RegExpBuiltinExecMatchRaw<false>(cx, regexp, string,
                                          int32_t(lastIndex), nullptr, rval);
}

// ES2024 draft rev d4927f9bc3706484c75dfef4bbcf5ba826d2632e
//
// 22.2.7.1 RegExpExec ( R, S )
// https://tc39.es/ecma262/#sec-regexpexec
//
// If `forTest` is true, this is called from `RegExp.prototype.test` and we can
// avoid allocating a result object.
bool js::RegExpExec(JSContext* cx, Handle<JSObject*> regexp,
                    Handle<JSString*> string, bool forTest,
                    MutableHandle<Value> rval) {
  // Step 1.
  Rooted<Value> exec(cx);
  Rooted<PropertyKey> execKey(cx, PropertyKey::NonIntAtom(cx->names().exec));
  if (!GetProperty(cx, regexp, regexp, execKey, &exec)) {
    return false;
  }

  // Step 2.
  // If exec is the original RegExp.prototype.exec, use the same, faster,
  // path as for the case where exec isn't callable.
  PropertyName* execName = cx->names().RegExp_prototype_Exec;
  if (MOZ_LIKELY(IsSelfHostedFunctionWithName(exec, execName)) ||
      !IsCallable(exec)) {
    // Steps 3-4.
    if (MOZ_LIKELY(regexp->is<RegExpObject>())) {
      return RegExpBuiltinExec(cx, regexp.as<RegExpObject>(), string, forTest,
                               rval);
    }

    // Throw an exception if it's not a wrapped RegExpObject that we can safely
    // unwrap.
    if (!regexp->canUnwrapAs<RegExpObject>()) {
      Rooted<Value> thisv(cx, ObjectValue(*regexp));
      return ReportIncompatibleSelfHostedMethod(cx, thisv);
    }

    // Call RegExpBuiltinExec in the regular expression's realm.
    Rooted<RegExpObject*> unwrapped(cx, &regexp->unwrapAs<RegExpObject>());
    {
      AutoRealm ar(cx, unwrapped);
      Rooted<JSString*> wrappedString(cx, string);
      if (!cx->compartment()->wrap(cx, &wrappedString)) {
        return false;
      }
      if (!RegExpBuiltinExec(cx, unwrapped, wrappedString, forTest, rval)) {
        return false;
      }
    }
    return cx->compartment()->wrap(cx, rval);
  }

  ReportUsageCounter(cx, nullptr, SUBCLASSING_REGEXP, SUBCLASSING_TYPE_IV);
  // Step 2.a.
  Rooted<Value> thisv(cx, ObjectValue(*regexp));
  FixedInvokeArgs<1> args(cx);
  args[0].setString(string);
  if (!js::Call(cx, exec, thisv, args, rval, CallReason::CallContent)) {
    return false;
  }

  // Step 2.b.
  if (!rval.isObjectOrNull()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_EXEC_NOT_OBJORNULL);
    return false;
  }

  // Step 2.c.
  if (forTest) {
    rval.setBoolean(rval.isObject());
  }
  return true;
}

bool js::RegExpHasCaptureGroups(JSContext* cx, Handle<RegExpObject*> obj,
                                Handle<JSString*> input, bool* result) {
  // pairCount is only available for compiled regular expressions.
  if (!obj->hasShared() ||
      obj->getShared()->kind() == RegExpShared::Kind::Unparsed) {
    Rooted<RegExpShared*> shared(cx, RegExpObject::getShared(cx, obj));
    if (!shared) {
      return false;
    }
    Rooted<JSLinearString*> inputLinear(cx, input->ensureLinear(cx));
    if (!inputLinear) {
      return false;
    }
    if (!RegExpShared::compileIfNecessary(cx, &shared, inputLinear,
                                          RegExpShared::CodeKind::Any)) {
      return false;
    }
  }

  MOZ_ASSERT(obj->getShared()->pairCount() >= 1);

  *result = obj->getShared()->pairCount() > 1;
  return true;
}

/* ES 2021 21.1.3.17.1 */
// https://tc39.es/ecma262/#sec-getsubstitution
bool js::RegExpGetSubstitution(JSContext* cx, Handle<ArrayObject*> matchResult,
                               Handle<JSLinearString*> string, size_t position,
                               Handle<JSLinearString*> replacement,
                               size_t firstDollarIndex, HandleValue groups,
                               MutableHandleValue rval) {
  MOZ_ASSERT(firstDollarIndex < replacement->length());

  // Step 1 (skipped).

  // Step 10 (reordered).
  uint32_t matchResultLength = matchResult->length();
  MOZ_ASSERT(matchResultLength > 0);
  MOZ_ASSERT(matchResultLength == matchResult->getDenseInitializedLength());

  const Value& matchedValue = matchResult->getDenseElement(0);
  Rooted<JSLinearString*> matched(cx,
                                  matchedValue.toString()->ensureLinear(cx));
  if (!matched) {
    return false;
  }

  // Step 2.
  size_t matchLength = matched->length();

  // Steps 3-5 (skipped).

  // Step 6.
  MOZ_ASSERT(position <= string->length());

  uint32_t nCaptures = matchResultLength - 1;
  Rooted<CapturesVector> captures(cx, CapturesVector(cx));
  if (!captures.reserve(nCaptures)) {
    return false;
  }

  // Step 7.
  for (uint32_t i = 1; i <= nCaptures; i++) {
    const Value& capture = matchResult->getDenseElement(i);

    if (capture.isUndefined()) {
      captures.infallibleAppend(capture);
      continue;
    }

    JSLinearString* captureLinear = capture.toString()->ensureLinear(cx);
    if (!captureLinear) {
      return false;
    }
    captures.infallibleAppend(StringValue(captureLinear));
  }

  Rooted<CapturesVector> namedCaptures(cx, cx);
  if (groups.isObject()) {
    RootedObject groupsObj(cx, &groups.toObject());
    if (!InitNamedCaptures(cx, replacement, groupsObj, firstDollarIndex,
                           &namedCaptures)) {
      return false;
    }
  } else {
    MOZ_ASSERT(groups.isUndefined());
  }

  // Step 8 (skipped).

  // Step 9.
  CheckedInt<uint32_t> checkedTailPos(0);
  checkedTailPos += position;
  checkedTailPos += matchLength;
  if (!checkedTailPos.isValid()) {
    ReportAllocationOverflow(cx);
    return false;
  }
  uint32_t tailPos = checkedTailPos.value();

  // Step 11.
  size_t reserveLength;
  if (!FindReplaceLength(cx, matched, string, position, tailPos, captures,
                         namedCaptures, replacement, firstDollarIndex,
                         &reserveLength)) {
    return false;
  }

  JSStringBuilder result(cx);
  if (NeedTwoBytes(string, replacement, matched, captures, namedCaptures)) {
    if (!result.ensureTwoByteChars()) {
      return false;
    }
  }

  if (!result.reserve(reserveLength)) {
    return false;
  }

  if (replacement->hasLatin1Chars()) {
    DoReplace<Latin1Char>(matched, string, position, tailPos, captures,
                          namedCaptures, replacement, firstDollarIndex, result);
  } else {
    DoReplace<char16_t>(matched, string, position, tailPos, captures,
                        namedCaptures, replacement, firstDollarIndex, result);
  }

  // Step 12.
  JSString* resultString = result.finishString();
  if (!resultString) {
    return false;
  }

  rval.setString(resultString);
  return true;
}

bool js::GetFirstDollarIndex(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  JSString* str = args[0].toString();

  // Should be handled in different path.
  MOZ_ASSERT(str->length() != 0);

  int32_t index = -1;
  if (!GetFirstDollarIndexRaw(cx, str, &index)) {
    return false;
  }

  args.rval().setInt32(index);
  return true;
}

template <typename TextChar>
static MOZ_ALWAYS_INLINE int GetFirstDollarIndexImpl(const TextChar* text,
                                                     uint32_t textLen) {
  const TextChar* end = text + textLen;
  for (const TextChar* c = text; c != end; ++c) {
    if (*c == '$') {
      return c - text;
    }
  }
  return -1;
}

int32_t js::GetFirstDollarIndexRawFlat(JSLinearString* text) {
  uint32_t len = text->length();

  JS::AutoCheckCannotGC nogc;
  if (text->hasLatin1Chars()) {
    return GetFirstDollarIndexImpl(text->latin1Chars(nogc), len);
  }

  return GetFirstDollarIndexImpl(text->twoByteChars(nogc), len);
}

bool js::GetFirstDollarIndexRaw(JSContext* cx, JSString* str, int32_t* index) {
  JSLinearString* text = str->ensureLinear(cx);
  if (!text) {
    return false;
  }

  *index = GetFirstDollarIndexRawFlat(text);
  return true;
}

bool js::RegExpPrototypeOptimizable(JSContext* cx, unsigned argc, Value* vp) {
  // This can only be called from self-hosted code.
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);

  args.rval().setBoolean(
      RegExpPrototypeOptimizableRaw(cx, &args[0].toObject()));
  return true;
}

bool js::RegExpPrototypeOptimizableRaw(JSContext* cx, JSObject* proto) {
  AutoUnsafeCallWithABI unsafe;
  AutoAssertNoPendingException aanpe(cx);
  if (!proto->is<NativeObject>()) {
    return false;
  }

  NativeObject* nproto = static_cast<NativeObject*>(proto);

  RegExpRealm& realm = cx->global()->regExpRealm();
  Shape* shape = realm.getOptimizableRegExpPrototypeShape();
  if (shape == nproto->shape()) {
    return true;
  }

  JSFunction* flagsGetter;
  if (!GetOwnGetterPure(cx, proto, NameToId(cx->names().flags), &flagsGetter)) {
    return false;
  }

  if (!flagsGetter) {
    return false;
  }

  if (!IsSelfHostedFunctionWithName(flagsGetter,
                                    cx->names().dollar_RegExpFlagsGetter_)) {
    return false;
  }

  JSNative globalGetter;
  if (!GetOwnNativeGetterPure(cx, proto, NameToId(cx->names().global),
                              &globalGetter)) {
    return false;
  }

  if (globalGetter != regexp_global) {
    return false;
  }

  JSNative hasIndicesGetter;
  if (!GetOwnNativeGetterPure(cx, proto, NameToId(cx->names().hasIndices),
                              &hasIndicesGetter)) {
    return false;
  }

  if (hasIndicesGetter != regexp_hasIndices) {
    return false;
  }

  JSNative ignoreCaseGetter;
  if (!GetOwnNativeGetterPure(cx, proto, NameToId(cx->names().ignoreCase),
                              &ignoreCaseGetter)) {
    return false;
  }

  if (ignoreCaseGetter != regexp_ignoreCase) {
    return false;
  }

  JSNative multilineGetter;
  if (!GetOwnNativeGetterPure(cx, proto, NameToId(cx->names().multiline),
                              &multilineGetter)) {
    return false;
  }

  if (multilineGetter != regexp_multiline) {
    return false;
  }

  JSNative stickyGetter;
  if (!GetOwnNativeGetterPure(cx, proto, NameToId(cx->names().sticky),
                              &stickyGetter)) {
    return false;
  }

  if (stickyGetter != regexp_sticky) {
    return false;
  }

  JSNative unicodeGetter;
  if (!GetOwnNativeGetterPure(cx, proto, NameToId(cx->names().unicode),
                              &unicodeGetter)) {
    return false;
  }

  if (unicodeGetter != regexp_unicode) {
    return false;
  }

  JSNative unicodeSetsGetter;
  if (!GetOwnNativeGetterPure(cx, proto, NameToId(cx->names().unicodeSets),
                              &unicodeSetsGetter)) {
    return false;
  }

  if (unicodeSetsGetter != regexp_unicodeSets) {
    return false;
  }

  JSNative dotAllGetter;
  if (!GetOwnNativeGetterPure(cx, proto, NameToId(cx->names().dotAll),
                              &dotAllGetter)) {
    return false;
  }

  if (dotAllGetter != regexp_dotAll) {
    return false;
  }

  // Check if @@match, @@search, and exec are own data properties,
  // those values should be tested in selfhosted JS.
  bool has = false;
  if (!HasOwnDataPropertyPure(
          cx, proto, PropertyKey::Symbol(cx->wellKnownSymbols().match), &has)) {
    return false;
  }
  if (!has) {
    return false;
  }

  if (!HasOwnDataPropertyPure(
          cx, proto, PropertyKey::Symbol(cx->wellKnownSymbols().search),
          &has)) {
    return false;
  }
  if (!has) {
    return false;
  }

  if (!HasOwnDataPropertyPure(cx, proto, NameToId(cx->names().exec), &has)) {
    return false;
  }
  if (!has) {
    return false;
  }

  realm.setOptimizableRegExpPrototypeShape(nproto->shape());
  return true;
}

bool js::RegExpInstanceOptimizable(JSContext* cx, unsigned argc, Value* vp) {
  // This can only be called from self-hosted code.
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);

  args.rval().setBoolean(RegExpInstanceOptimizableRaw(cx, &args[0].toObject(),
                                                      &args[1].toObject()));
  return true;
}

bool js::RegExpInstanceOptimizableRaw(JSContext* cx, JSObject* obj,
                                      JSObject* proto) {
  AutoUnsafeCallWithABI unsafe;
  AutoAssertNoPendingException aanpe(cx);

  RegExpObject* rx = &obj->as<RegExpObject>();

  RegExpRealm& realm = cx->global()->regExpRealm();
  Shape* shape = realm.getOptimizableRegExpInstanceShape();
  if (shape == rx->shape()) {
    return true;
  }

  if (!rx->hasStaticPrototype()) {
    return false;
  }

  if (rx->staticPrototype() != proto) {
    return false;
  }

  if (!RegExpObject::isInitialShape(rx)) {
    return false;
  }

  realm.setOptimizableRegExpInstanceShape(rx->shape());
  return true;
}

/*
 * Pattern match the script to check if it is is indexing into a particular
 * object, e.g. 'function(a) { return b[a]; }'. Avoid calling the script in
 * such cases, which are used by javascript packers (particularly the popular
 * Dean Edwards packer) to efficiently encode large scripts. We only handle the
 * code patterns generated by such packers here.
 */
bool js::intrinsic_GetElemBaseForLambda(JSContext* cx, unsigned argc,
                                        Value* vp) {
  // This can only be called from self-hosted code.
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);

  JSObject& lambda = args[0].toObject();
  args.rval().setUndefined();

  if (!lambda.is<JSFunction>()) {
    return true;
  }

  RootedFunction fun(cx, &lambda.as<JSFunction>());
  if (!fun->isInterpreted() || fun->isClassConstructor()) {
    return true;
  }

  JSScript* script = JSFunction::getOrCreateScript(cx, fun);
  if (!script) {
    return false;
  }

  jsbytecode* pc = script->code();

  /*
   * JSOp::GetAliasedVar tells us exactly where to find the base object 'b'.
   * Rule out the (unlikely) possibility of a function with environment
   * objects since it would make our environment walk off.
   */
  if (JSOp(*pc) != JSOp::GetAliasedVar || fun->needsSomeEnvironmentObject()) {
    return true;
  }
  EnvironmentCoordinate ec(pc);
  EnvironmentObject* env = &fun->environment()->as<EnvironmentObject>();
  for (unsigned i = 0; i < ec.hops(); ++i) {
    env = &env->enclosingEnvironment().as<EnvironmentObject>();
  }
  Value b = env->aliasedBinding(ec);
  pc += JSOpLength_GetAliasedVar;

  /* Look for 'a' to be the lambda's first argument. */
  if (JSOp(*pc) != JSOp::GetArg || GET_ARGNO(pc) != 0) {
    return true;
  }
  pc += JSOpLength_GetArg;

  /* 'b[a]' */
  if (JSOp(*pc) != JSOp::GetElem) {
    return true;
  }
  pc += JSOpLength_GetElem;

  /* 'return b[a]' */
  if (JSOp(*pc) != JSOp::Return) {
    return true;
  }

  /* 'b' must behave like a normal object. */
  if (!b.isObject()) {
    return true;
  }

  JSObject& bobj = b.toObject();
  const JSClass* clasp = bobj.getClass();
  if (!clasp->isNativeObject() || clasp->getOpsLookupProperty() ||
      clasp->getOpsGetProperty()) {
    return true;
  }

  args.rval().setObject(bobj);
  return true;
}

/*
 * Emulates `b[a]` property access, that is detected in GetElemBaseForLambda.
 * It returns the property value only if the property is data property and the
 * property value is a string.  Otherwise it returns undefined.
 */
bool js::intrinsic_GetStringDataProperty(JSContext* cx, unsigned argc,
                                         Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);

  RootedObject obj(cx, &args[0].toObject());
  if (!obj->is<NativeObject>()) {
    // The object is already checked to be native in GetElemBaseForLambda,
    // but it can be swapped to another class that is non-native.
    // Return undefined to mark failure to get the property.
    args.rval().setUndefined();
    return true;
  }

  JSAtom* atom = AtomizeString(cx, args[1].toString());
  if (!atom) {
    return false;
  }

  Value v;
  if (GetPropertyPure(cx, obj, AtomToId(atom), &v) && v.isString()) {
    args.rval().set(v);
  } else {
    args.rval().setUndefined();
  }

  return true;
}
