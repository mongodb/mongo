/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/ObjectEmitter.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include "frontend/BytecodeEmitter.h"  // BytecodeEmitter
#include "frontend/IfEmitter.h"        // IfEmitter
#include "frontend/ParseNode.h"        // AccessorType
#include "frontend/SharedContext.h"    // SharedContext
#include "vm/FunctionPrefixKind.h"     // FunctionPrefixKind
#include "vm/Opcodes.h"                // JSOp

using namespace js;
using namespace js::frontend;

PropertyEmitter::PropertyEmitter(BytecodeEmitter* bce) : bce_(bce) {}

bool PropertyEmitter::prepareForProtoValue(uint32_t keyPos) {
  MOZ_ASSERT(propertyState_ == PropertyState::Start ||
             propertyState_ == PropertyState::Init);

  //                [stack] CTOR? OBJ CTOR?

  if (!bce_->updateSourceCoordNotes(keyPos)) {
    return false;
  }

#ifdef DEBUG
  propertyState_ = PropertyState::ProtoValue;
#endif
  return true;
}

bool PropertyEmitter::emitMutateProto() {
  MOZ_ASSERT(propertyState_ == PropertyState::ProtoValue);

  //                [stack] OBJ PROTO

  if (!bce_->emit1(JSOp::MutateProto)) {
    //              [stack] OBJ
    return false;
  }

#ifdef DEBUG
  propertyState_ = PropertyState::Init;
#endif
  return true;
}

bool PropertyEmitter::prepareForSpreadOperand(uint32_t spreadPos) {
  MOZ_ASSERT(propertyState_ == PropertyState::Start ||
             propertyState_ == PropertyState::Init);

  //                [stack] OBJ

  if (!bce_->updateSourceCoordNotes(spreadPos)) {
    return false;
  }
  if (!bce_->emit1(JSOp::Dup)) {
    //              [stack] OBJ OBJ
    return false;
  }

#ifdef DEBUG
  propertyState_ = PropertyState::SpreadOperand;
#endif
  return true;
}

bool PropertyEmitter::emitSpread() {
  MOZ_ASSERT(propertyState_ == PropertyState::SpreadOperand);

  //                [stack] OBJ OBJ VAL

  if (!bce_->emitCopyDataProperties(BytecodeEmitter::CopyOption::Unfiltered)) {
    //              [stack] OBJ
    return false;
  }

#ifdef DEBUG
  propertyState_ = PropertyState::Init;
#endif
  return true;
}

MOZ_ALWAYS_INLINE bool PropertyEmitter::prepareForProp(uint32_t keyPos,
                                                       bool isStatic,
                                                       bool isIndexOrComputed) {
  isStatic_ = isStatic;
  isIndexOrComputed_ = isIndexOrComputed;

  //                [stack] CTOR? OBJ

  if (!bce_->updateSourceCoordNotes(keyPos)) {
    return false;
  }

  if (isStatic_) {
    if (!bce_->emit1(JSOp::Dup2)) {
      //            [stack] CTOR HOMEOBJ CTOR HOMEOBJ
      return false;
    }
    if (!bce_->emit1(JSOp::Pop)) {
      //            [stack] CTOR HOMEOBJ CTOR
      return false;
    }
  }

  return true;
}

bool PropertyEmitter::prepareForPrivateMethod() {
  MOZ_ASSERT(propertyState_ == PropertyState::Start ||
             propertyState_ == PropertyState::Init);
  MOZ_ASSERT(isClass_);

  isStatic_ = false;
  isIndexOrComputed_ = false;

#ifdef DEBUG
  propertyState_ = PropertyState::PrivateMethodValue;
#endif
  return true;
}

bool PropertyEmitter::prepareForPrivateStaticMethod(uint32_t keyPos) {
  MOZ_ASSERT(propertyState_ == PropertyState::Start ||
             propertyState_ == PropertyState::Init);
  MOZ_ASSERT(isClass_);

  //                [stack] CTOR OBJ

  if (!prepareForProp(keyPos,
                      /* isStatic_ = */ true,
                      /* isIndexOrComputed = */ true)) {
    //              [stack] CTOR OBJ CTOR
    return false;
  }

#ifdef DEBUG
  propertyState_ = PropertyState::PrivateStaticMethod;
#endif
  return true;
}

bool PropertyEmitter::prepareForPropValue(uint32_t keyPos, Kind kind) {
  MOZ_ASSERT(propertyState_ == PropertyState::Start ||
             propertyState_ == PropertyState::Init);

  //                [stack] CTOR? OBJ

  if (!prepareForProp(keyPos,
                      /* isStatic_ = */ kind == Kind::Static,
                      /* isIndexOrComputed = */ false)) {
    //              [stack] CTOR? OBJ CTOR?
    return false;
  }

#ifdef DEBUG
  propertyState_ = PropertyState::PropValue;
#endif
  return true;
}

bool PropertyEmitter::prepareForIndexPropKey(uint32_t keyPos, Kind kind) {
  MOZ_ASSERT(propertyState_ == PropertyState::Start ||
             propertyState_ == PropertyState::Init);

  //                [stack] CTOR? OBJ

  if (!prepareForProp(keyPos,
                      /* isStatic_ = */ kind == Kind::Static,
                      /* isIndexOrComputed = */ true)) {
    //              [stack] CTOR? OBJ CTOR?
    return false;
  }

#ifdef DEBUG
  propertyState_ = PropertyState::IndexKey;
#endif
  return true;
}

bool PropertyEmitter::prepareForIndexPropValue() {
  MOZ_ASSERT(propertyState_ == PropertyState::IndexKey);

  //                [stack] CTOR? OBJ CTOR? KEY

#ifdef DEBUG
  propertyState_ = PropertyState::IndexValue;
#endif
  return true;
}

bool PropertyEmitter::prepareForComputedPropKey(uint32_t keyPos, Kind kind) {
  MOZ_ASSERT(propertyState_ == PropertyState::Start ||
             propertyState_ == PropertyState::Init);

  //                [stack] CTOR? OBJ

  if (!prepareForProp(keyPos,
                      /* isStatic_ = */ kind == Kind::Static,
                      /* isIndexOrComputed = */ true)) {
    //              [stack] CTOR? OBJ CTOR?
    return false;
  }

#ifdef DEBUG
  propertyState_ = PropertyState::ComputedKey;
#endif
  return true;
}

bool PropertyEmitter::prepareForComputedPropValue() {
  MOZ_ASSERT(propertyState_ == PropertyState::ComputedKey);

  //                [stack] CTOR? OBJ CTOR? KEY

  if (!bce_->emit1(JSOp::ToPropertyKey)) {
    //              [stack] CTOR? OBJ CTOR? KEY
    return false;
  }

#ifdef DEBUG
  propertyState_ = PropertyState::ComputedValue;
#endif
  return true;
}

bool PropertyEmitter::emitInitHomeObject() {
  MOZ_ASSERT(propertyState_ == PropertyState::PropValue ||
             propertyState_ == PropertyState::PrivateMethodValue ||
             propertyState_ == PropertyState::PrivateStaticMethod ||
             propertyState_ == PropertyState::IndexValue ||
             propertyState_ == PropertyState::ComputedValue);

  //                [stack] CTOR? HOMEOBJ CTOR? KEY? FUN

  // There are the following values on the stack conditionally, between
  // HOMEOBJ and FUN:
  //   * the 2nd CTOR if isStatic_
  //   * KEY if isIndexOrComputed_
  //
  // JSOp::InitHomeObject uses one of the following:
  //   * HOMEOBJ if !isStatic_
  //     (`super.foo` points the super prototype property)
  //   * the 2nd CTOR if isStatic_
  //     (`super.foo` points the super constructor property)
  if (!bce_->emitDupAt(1 + isIndexOrComputed_)) {
    //              [stack] # non-static method
    //              [stack] CTOR? HOMEOBJ CTOR KEY? FUN CTOR
    //              [stack] # static method
    //              [stack] CTOR? HOMEOBJ KEY? FUN HOMEOBJ
    return false;
  }
  if (!bce_->emit1(JSOp::InitHomeObject)) {
    //              [stack] CTOR? HOMEOBJ CTOR? KEY? FUN
    return false;
  }

#ifdef DEBUG
  if (propertyState_ == PropertyState::PropValue) {
    propertyState_ = PropertyState::InitHomeObj;
  } else if (propertyState_ == PropertyState::PrivateMethodValue) {
    propertyState_ = PropertyState::InitHomeObjForPrivateMethod;
  } else if (propertyState_ == PropertyState::PrivateStaticMethod) {
    propertyState_ = PropertyState::InitHomeObjForPrivateStaticMethod;
  } else if (propertyState_ == PropertyState::IndexValue) {
    propertyState_ = PropertyState::InitHomeObjForIndex;
  } else {
    propertyState_ = PropertyState::InitHomeObjForComputed;
  }
#endif
  return true;
}

bool PropertyEmitter::emitInit(AccessorType accessorType,
                               TaggedParserAtomIndex key) {
  switch (accessorType) {
    case AccessorType::None:
      return emitInit(isClass_ ? JSOp::InitHiddenProp : JSOp::InitProp, key);
    case AccessorType::Getter:
      return emitInit(
          isClass_ ? JSOp::InitHiddenPropGetter : JSOp::InitPropGetter, key);
    case AccessorType::Setter:
      return emitInit(
          isClass_ ? JSOp::InitHiddenPropSetter : JSOp::InitPropSetter, key);
  }
  MOZ_CRASH("Invalid op");
}

bool PropertyEmitter::emitInitIndexOrComputed(AccessorType accessorType) {
  switch (accessorType) {
    case AccessorType::None:
      return emitInitIndexOrComputed(isClass_ ? JSOp::InitHiddenElem
                                              : JSOp::InitElem);
    case AccessorType::Getter:
      return emitInitIndexOrComputed(isClass_ ? JSOp::InitHiddenElemGetter
                                              : JSOp::InitElemGetter);
    case AccessorType::Setter:
      return emitInitIndexOrComputed(isClass_ ? JSOp::InitHiddenElemSetter
                                              : JSOp::InitElemSetter);
  }
  MOZ_CRASH("Invalid op");
}

bool PropertyEmitter::emitPrivateStaticMethod(AccessorType accessorType) {
  MOZ_ASSERT(isClass_);

  switch (accessorType) {
    case AccessorType::None:
      return emitInitIndexOrComputed(JSOp::InitLockedElem);
    case AccessorType::Getter:
      return emitInitIndexOrComputed(JSOp::InitHiddenElemGetter);
    case AccessorType::Setter:
      return emitInitIndexOrComputed(JSOp::InitHiddenElemSetter);
  }
  MOZ_CRASH("Invalid op");
}

bool PropertyEmitter::emitInit(JSOp op, TaggedParserAtomIndex key) {
  MOZ_ASSERT(propertyState_ == PropertyState::PropValue ||
             propertyState_ == PropertyState::InitHomeObj);

  MOZ_ASSERT(op == JSOp::InitProp || op == JSOp::InitHiddenProp ||
             op == JSOp::InitPropGetter || op == JSOp::InitHiddenPropGetter ||
             op == JSOp::InitPropSetter || op == JSOp::InitHiddenPropSetter);

  //                [stack] CTOR? OBJ CTOR? VAL

  if (!bce_->emitAtomOp(op, key)) {
    //              [stack] CTOR? OBJ CTOR?
    return false;
  }

  if (!emitPopClassConstructor()) {
    return false;
  }

#ifdef DEBUG
  propertyState_ = PropertyState::Init;
#endif
  return true;
}

bool PropertyEmitter::skipInit() {
  MOZ_ASSERT(propertyState_ == PropertyState::PrivateMethodValue ||
             propertyState_ == PropertyState::InitHomeObjForPrivateMethod);
#ifdef DEBUG
  propertyState_ = PropertyState::Init;
#endif
  return true;
}

bool PropertyEmitter::emitInitIndexOrComputed(JSOp op) {
  MOZ_ASSERT(propertyState_ == PropertyState::IndexValue ||
             propertyState_ == PropertyState::InitHomeObjForIndex ||
             propertyState_ == PropertyState::ComputedValue ||
             propertyState_ == PropertyState::InitHomeObjForComputed ||
             propertyState_ == PropertyState::PrivateStaticMethod ||
             propertyState_ ==
                 PropertyState::InitHomeObjForPrivateStaticMethod);

  MOZ_ASSERT(op == JSOp::InitElem || op == JSOp::InitHiddenElem ||
             op == JSOp::InitLockedElem || op == JSOp::InitElemGetter ||
             op == JSOp::InitHiddenElemGetter || op == JSOp::InitElemSetter ||
             op == JSOp::InitHiddenElemSetter);

  //                [stack] CTOR? OBJ CTOR? KEY VAL

  if (!bce_->emit1(op)) {
    //              [stack] CTOR? OBJ CTOR?
    return false;
  }

  if (!emitPopClassConstructor()) {
    return false;
  }

#ifdef DEBUG
  propertyState_ = PropertyState::Init;
#endif
  return true;
}

bool PropertyEmitter::emitPopClassConstructor() {
  if (isStatic_) {
    //              [stack] CTOR HOMEOBJ CTOR

    if (!bce_->emit1(JSOp::Pop)) {
      //            [stack] CTOR HOMEOBJ
      return false;
    }
  }

  return true;
}

ObjectEmitter::ObjectEmitter(BytecodeEmitter* bce) : PropertyEmitter(bce) {}

bool ObjectEmitter::emitObject(size_t propertyCount) {
  MOZ_ASSERT(propertyState_ == PropertyState::Start);
  MOZ_ASSERT(objectState_ == ObjectState::Start);

  //                [stack]

  // Emit code for {p:a, '%q':b, 2:c} that is equivalent to constructing
  // a new object and defining (in source order) each property on the object
  // (or mutating the object's [[Prototype]], in the case of __proto__).
  if (!bce_->emit1(JSOp::NewInit)) {
    //              [stack] OBJ
    return false;
  }

#ifdef DEBUG
  objectState_ = ObjectState::Object;
#endif
  return true;
}

bool ObjectEmitter::emitObjectWithTemplateOnStack() {
  MOZ_ASSERT(propertyState_ == PropertyState::Start);
  MOZ_ASSERT(objectState_ == ObjectState::Start);

#ifdef DEBUG
  objectState_ = ObjectState::Object;
#endif
  return true;
}

bool ObjectEmitter::emitEnd() {
  MOZ_ASSERT(propertyState_ == PropertyState::Start ||
             propertyState_ == PropertyState::Init);
  MOZ_ASSERT(objectState_ == ObjectState::Object);

  //                [stack] OBJ

#ifdef DEBUG
  objectState_ = ObjectState::End;
#endif
  return true;
}

AutoSaveLocalStrictMode::AutoSaveLocalStrictMode(SharedContext* sc) : sc_(sc) {
  savedStrictness_ = sc_->setLocalStrictMode(true);
}

AutoSaveLocalStrictMode::~AutoSaveLocalStrictMode() {
  if (sc_) {
    restore();
  }
}

void AutoSaveLocalStrictMode::restore() {
  MOZ_ALWAYS_TRUE(sc_->setLocalStrictMode(savedStrictness_));
  sc_ = nullptr;
}

ClassEmitter::ClassEmitter(BytecodeEmitter* bce)
    : PropertyEmitter(bce), strictMode_(bce->sc) {
  isClass_ = true;
}

bool ClassEmitter::emitScope(LexicalScope::ParserData* scopeBindings) {
  MOZ_ASSERT(propertyState_ == PropertyState::Start);
  MOZ_ASSERT(classState_ == ClassState::Start);

  tdzCache_.emplace(bce_);

  innerScope_.emplace(bce_);
  if (!innerScope_->enterLexical(bce_, ScopeKind::Lexical, scopeBindings)) {
    return false;
  }

#ifdef DEBUG
  classState_ = ClassState::Scope;
#endif

  return true;
}

bool ClassEmitter::emitBodyScope(ClassBodyScope::ParserData* scopeBindings) {
  MOZ_ASSERT(propertyState_ == PropertyState::Start);
  MOZ_ASSERT(classState_ == ClassState::Start ||
             classState_ == ClassState::Scope);

  bodyTdzCache_.emplace(bce_);

  bodyScope_.emplace(bce_);
  if (!bodyScope_->enterClassBody(bce_, ScopeKind::ClassBody, scopeBindings)) {
    return false;
  }

#ifdef DEBUG
  classState_ = ClassState::BodyScope;
#endif

  return true;
}

bool ClassEmitter::emitClass(TaggedParserAtomIndex name,
                             TaggedParserAtomIndex nameForAnonymousClass,
                             bool hasNameOnStack) {
  MOZ_ASSERT(propertyState_ == PropertyState::Start);
  MOZ_ASSERT(classState_ == ClassState::Start ||
             classState_ == ClassState::Scope ||
             classState_ == ClassState::BodyScope);
  MOZ_ASSERT_IF(nameForAnonymousClass || hasNameOnStack, !name);
  MOZ_ASSERT(!(nameForAnonymousClass && hasNameOnStack));

  //                [stack]

  name_ = name;
  nameForAnonymousClass_ = nameForAnonymousClass;
  hasNameOnStack_ = hasNameOnStack;
  isDerived_ = false;

  if (!bce_->emit1(JSOp::NewInit)) {
    //              [stack] HOMEOBJ
    return false;
  }

#ifdef DEBUG
  classState_ = ClassState::Class;
#endif
  return true;
}

bool ClassEmitter::emitDerivedClass(TaggedParserAtomIndex name,
                                    TaggedParserAtomIndex nameForAnonymousClass,
                                    bool hasNameOnStack) {
  MOZ_ASSERT(propertyState_ == PropertyState::Start);
  MOZ_ASSERT(classState_ == ClassState::Start ||
             classState_ == ClassState::Scope ||
             classState_ == ClassState::BodyScope);
  MOZ_ASSERT_IF(nameForAnonymousClass || hasNameOnStack, !name);
  MOZ_ASSERT(!nameForAnonymousClass || !hasNameOnStack);

  //                [stack] HERITAGE

  name_ = name;
  nameForAnonymousClass_ = nameForAnonymousClass;
  hasNameOnStack_ = hasNameOnStack;
  isDerived_ = true;

  InternalIfEmitter ifThenElse(bce_);

  // Heritage must be null or a non-generator constructor
  if (!bce_->emit1(JSOp::CheckClassHeritage)) {
    //              [stack] HERITAGE
    return false;
  }

  // [IF] (heritage !== null)
  if (!bce_->emit1(JSOp::Dup)) {
    //              [stack] HERITAGE HERITAGE
    return false;
  }
  if (!bce_->emit1(JSOp::Null)) {
    //              [stack] HERITAGE HERITAGE NULL
    return false;
  }
  if (!bce_->emit1(JSOp::StrictNe)) {
    //              [stack] HERITAGE NE
    return false;
  }

  // [THEN] funProto = heritage, objProto = heritage.prototype
  if (!ifThenElse.emitThenElse()) {
    return false;
  }
  if (!bce_->emit1(JSOp::Dup)) {
    //              [stack] HERITAGE HERITAGE
    return false;
  }
  if (!bce_->emitAtomOp(JSOp::GetProp,
                        TaggedParserAtomIndex::WellKnown::prototype())) {
    //              [stack] HERITAGE PROTO
    return false;
  }

  // [ELSE] funProto = %FunctionPrototype%, objProto = null
  if (!ifThenElse.emitElse()) {
    return false;
  }
  if (!bce_->emit1(JSOp::Pop)) {
    //              [stack]
    return false;
  }
  if (!bce_->emitBuiltinObject(BuiltinObjectKind::FunctionPrototype)) {
    //              [stack] PROTO
    return false;
  }
  if (!bce_->emit1(JSOp::Null)) {
    //              [stack] PROTO NULL
    return false;
  }

  // [ENDIF]
  if (!ifThenElse.emitEnd()) {
    return false;
  }

  if (!bce_->emit1(JSOp::ObjWithProto)) {
    //              [stack] HERITAGE HOMEOBJ
    return false;
  }
  if (!bce_->emit1(JSOp::Swap)) {
    //              [stack] HOMEOBJ HERITAGE
    return false;
  }

#ifdef DEBUG
  classState_ = ClassState::Class;
#endif
  return true;
}

bool ClassEmitter::emitInitConstructor(bool needsHomeObject) {
  MOZ_ASSERT(classState_ == ClassState::Class ||
             classState_ == ClassState::InstanceMemberInitializersEnd);

  //                [stack] HOMEOBJ CTOR

  if (needsHomeObject) {
    if (!bce_->emitDupAt(1)) {
      //            [stack] HOMEOBJ CTOR HOMEOBJ
      return false;
    }
    if (!bce_->emit1(JSOp::InitHomeObject)) {
      //            [stack] HOMEOBJ CTOR
      return false;
    }
  }

  if (!initProtoAndCtor()) {
    //              [stack] CTOR HOMEOBJ
    return false;
  }

#ifdef DEBUG
  classState_ = ClassState::InitConstructor;
#endif
  return true;
}

bool ClassEmitter::initProtoAndCtor() {
  //                [stack] NAME? HOMEOBJ CTOR

  if (hasNameOnStack_) {
    if (!bce_->emitDupAt(2)) {
      //            [stack] NAME HOMEOBJ CTOR NAME
      return false;
    }
    if (!bce_->emit2(JSOp::SetFunName, uint8_t(FunctionPrefixKind::None))) {
      //            [stack] NAME HOMEOBJ CTOR
      return false;
    }
  }

  if (!bce_->emit1(JSOp::Swap)) {
    //              [stack] NAME? CTOR HOMEOBJ
    return false;
  }
  if (!bce_->emit1(JSOp::Dup2)) {
    //              [stack] NAME? CTOR HOMEOBJ CTOR HOMEOBJ
    return false;
  }
  if (!bce_->emitAtomOp(JSOp::InitLockedProp,
                        TaggedParserAtomIndex::WellKnown::prototype())) {
    //              [stack] NAME? CTOR HOMEOBJ CTOR
    return false;
  }
  if (!bce_->emitAtomOp(JSOp::InitHiddenProp,
                        TaggedParserAtomIndex::WellKnown::constructor())) {
    //              [stack] NAME? CTOR HOMEOBJ
    return false;
  }

  return true;
}

bool ClassEmitter::prepareForMemberInitializers(size_t numInitializers,
                                                bool isStatic) {
  MOZ_ASSERT_IF(!isStatic, classState_ == ClassState::Class);
  MOZ_ASSERT_IF(isStatic, classState_ == ClassState::InitConstructor);
  MOZ_ASSERT(memberState_ == MemberState::Start);

  // .initializers is a variable that stores an array of lambdas containing
  // code (the initializer) for each field. Upon an object's construction,
  // these lambdas will be called, defining the values.
  auto initializers =
      isStatic ? TaggedParserAtomIndex::WellKnown::dot_staticInitializers_()
               : TaggedParserAtomIndex::WellKnown::dot_initializers_();
  initializersAssignment_.emplace(bce_, initializers,
                                  NameOpEmitter::Kind::Initialize);
  if (!initializersAssignment_->prepareForRhs()) {
    return false;
  }

  if (!bce_->emitUint32Operand(JSOp::NewArray, numInitializers)) {
    //              [stack] ARRAY
    return false;
  }

  initializerIndex_ = 0;
#ifdef DEBUG
  if (isStatic) {
    classState_ = ClassState::StaticMemberInitializers;
  } else {
    classState_ = ClassState::InstanceMemberInitializers;
  }
  numInitializers_ = numInitializers;
#endif
  return true;
}

bool ClassEmitter::prepareForMemberInitializer() {
  MOZ_ASSERT(classState_ == ClassState::InstanceMemberInitializers ||
             classState_ == ClassState::StaticMemberInitializers);
  MOZ_ASSERT(memberState_ == MemberState::Start);

#ifdef DEBUG
  memberState_ = MemberState::Initializer;
#endif
  return true;
}

bool ClassEmitter::emitMemberInitializerHomeObject(bool isStatic) {
  MOZ_ASSERT(memberState_ == MemberState::Initializer);
  //                [stack] OBJ HERITAGE? ARRAY METHOD
  // or:
  //                [stack] CTOR HOMEOBJ ARRAY METHOD

  if (isStatic) {
    if (!bce_->emitDupAt(3)) {
      //            [stack] CTOR HOMEOBJ ARRAY METHOD CTOR
      return false;
    }
  } else {
    if (!bce_->emitDupAt(isDerived_ ? 3 : 2)) {
      //            [stack] OBJ HERITAGE? ARRAY METHOD OBJ
      return false;
    }
  }
  if (!bce_->emit1(JSOp::InitHomeObject)) {
    //              [stack] OBJ HERITAGE? ARRAY METHOD
    // or:
    //              [stack] CTOR HOMEOBJ ARRAY METHOD
    return false;
  }

#ifdef DEBUG
  memberState_ = MemberState::InitializerWithHomeObject;
#endif
  return true;
}

bool ClassEmitter::emitStoreMemberInitializer() {
  MOZ_ASSERT(memberState_ == MemberState::Initializer ||
             memberState_ == MemberState::InitializerWithHomeObject);
  MOZ_ASSERT(initializerIndex_ < numInitializers_);
  //                [stack] HOMEOBJ HERITAGE? ARRAY METHOD

  if (!bce_->emitUint32Operand(JSOp::InitElemArray, initializerIndex_)) {
    //              [stack] HOMEOBJ HERITAGE? ARRAY
    return false;
  }

  initializerIndex_++;
#ifdef DEBUG
  memberState_ = MemberState::Start;
#endif
  return true;
}

bool ClassEmitter::emitMemberInitializersEnd() {
  MOZ_ASSERT(propertyState_ == PropertyState::Start ||
             propertyState_ == PropertyState::Init);
  MOZ_ASSERT(classState_ == ClassState::InstanceMemberInitializers ||
             classState_ == ClassState::StaticMemberInitializers);
  MOZ_ASSERT(memberState_ == MemberState::Start);
  MOZ_ASSERT(initializerIndex_ == numInitializers_);

  if (!initializersAssignment_->emitAssignment()) {
    //              [stack] HOMEOBJ HERITAGE? ARRAY
    return false;
  }
  initializersAssignment_.reset();

  if (!bce_->emit1(JSOp::Pop)) {
    //              [stack] HOMEOBJ HERITAGE?
    return false;
  }

#ifdef DEBUG
  if (classState_ == ClassState::InstanceMemberInitializers) {
    classState_ = ClassState::InstanceMemberInitializersEnd;
  } else {
    classState_ = ClassState::StaticMemberInitializersEnd;
  }
#endif
  return true;
}

#ifdef ENABLE_DECORATORS
bool ClassEmitter::prepareForExtraInitializers(
    TaggedParserAtomIndex initializers) {
  // TODO: Add support for static and class extra initializers, see bug 1868220
  // and bug 1868221.
  MOZ_ASSERT(
      initializers ==
      TaggedParserAtomIndex::WellKnown::dot_instanceExtraInitializers_());

  NameOpEmitter noe(bce_, initializers, NameOpEmitter::Kind::Initialize);
  if (!noe.prepareForRhs()) {
    return false;
  }

  // Because the initializers are created while executing decorators, we don't
  // know beforehand how many there will be.
  if (!bce_->emitUint32Operand(JSOp::NewArray, 0)) {
    //              [stack] ARRAY
    return false;
  }

  if (!noe.emitAssignment()) {
    //              [stack] ARRAY
    return false;
  }

  return bce_->emit1(JSOp::Pop);
  //                [stack]
}
#endif

bool ClassEmitter::emitBinding() {
  MOZ_ASSERT(propertyState_ == PropertyState::Start ||
             propertyState_ == PropertyState::Init);
  MOZ_ASSERT(classState_ == ClassState::InitConstructor ||
             classState_ == ClassState::InstanceMemberInitializersEnd ||
             classState_ == ClassState::StaticMemberInitializersEnd);
  //                [stack] CTOR HOMEOBJ

  if (!bce_->emit1(JSOp::Pop)) {
    //              [stack] CTOR
    return false;
  }

  if (name_) {
    MOZ_ASSERT(innerScope_.isSome());

    if (!bce_->emitLexicalInitialization(name_)) {
      //            [stack] CTOR
      return false;
    }
  }

  //                [stack] CTOR

#ifdef DEBUG
  classState_ = ClassState::BoundName;
#endif
  return true;
}

#ifdef ENABLE_DECORATORS
bool ClassEmitter::prepareForDecorators() { return leaveBodyAndInnerScope(); }
#endif

bool ClassEmitter::leaveBodyAndInnerScope() {
  if (bodyScope_.isSome()) {
    MOZ_ASSERT(bodyTdzCache_.isSome());

    if (!bodyScope_->leave(bce_)) {
      return false;
    }
    bodyScope_.reset();
    bodyTdzCache_.reset();
  }

  if (innerScope_.isSome()) {
    MOZ_ASSERT(tdzCache_.isSome());

    if (!innerScope_->leave(bce_)) {
      return false;
    }
    innerScope_.reset();
    tdzCache_.reset();
  } else {
    MOZ_ASSERT(tdzCache_.isNothing());
  }

  return true;
}

bool ClassEmitter::emitEnd(Kind kind) {
  MOZ_ASSERT(classState_ == ClassState::BoundName);
  //                [stack] CTOR

#ifndef ENABLE_DECORATORS
  if (!leaveBodyAndInnerScope()) {
    return false;
  }
#endif

  if (kind == Kind::Declaration) {
    MOZ_ASSERT(name_);

    if (!bce_->emitLexicalInitialization(name_)) {
      //            [stack] CTOR
      return false;
    }
    // Only class statements make outer bindings, and they do not leave
    // themselves on the stack.
    if (!bce_->emit1(JSOp::Pop)) {
      //            [stack]
      return false;
    }
  }

  //                [stack] # class declaration
  //                [stack]
  //                [stack] # class expression
  //                [stack] CTOR

  strictMode_.restore();

#ifdef DEBUG
  classState_ = ClassState::End;
#endif
  return true;
}
