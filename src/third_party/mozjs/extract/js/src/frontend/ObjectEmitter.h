/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_ObjectEmitter_h
#define frontend_ObjectEmitter_h

#include "mozilla/Attributes.h"  // MOZ_STACK_CLASS, MOZ_ALWAYS_INLINE, MOZ_RAII
#include "mozilla/Maybe.h"       // Maybe

#include <stddef.h>  // size_t
#include <stdint.h>  // uint32_t

#include "frontend/BytecodeOffset.h"  // BytecodeOffset
#include "frontend/EmitterScope.h"    // EmitterScope
#include "frontend/NameOpEmitter.h"   // NameOpEmitter
#include "frontend/ParseNode.h"       // AccessorType
#include "frontend/ParserAtom.h"      // TaggedParserAtomIndex
#include "frontend/TDZCheckCache.h"   // TDZCheckCache
#include "vm/BytecodeUtil.h"          // JSOp
#include "vm/NativeObject.h"          // PlainObject
#include "vm/Scope.h"                 // LexicalScope

namespace js {

namespace frontend {

struct BytecodeEmitter;
class SharedContext;

// Class for emitting bytecode for object and class properties.
// See ObjectEmitter and ClassEmitter for usage.
class MOZ_STACK_CLASS PropertyEmitter {
 public:
  enum class Kind {
    // Prototype property.
    Prototype,

    // Class static property.
    Static
  };

 protected:
  BytecodeEmitter* bce_;

  // True if the object is class.
  // Set by ClassEmitter.
  bool isClass_ = false;

  // True if the property is class static method.
  bool isStatic_ = false;

  // True if the property has computed or index key.
  bool isIndexOrComputed_ = false;

#ifdef DEBUG
  // The state of this emitter.
  //
  // +-------+
  // | Start |-+
  // +-------+ |
  //           |
  // +---------+
  // |
  // |  +------------------------------------------------------------+
  // |  |                                                            |
  // |  | [normal property/method/accessor]                          |
  // |  v   prepareForPropValue  +-----------+              +------+ |
  // +->+----------------------->| PropValue |-+         +->| Init |-+
  //    |                        +-----------+ |         |  +------+
  //    |                                      |         |
  //    |  +-----------------------------------+         +-----------+
  //    |  |                                                         |
  //    |  +-+---------------------------------------+               |
  //    |    |                                       |               |
  //    |    | [method with super]                   |               |
  //    |    |   emitInitHomeObject +-------------+  v               |
  //    |    +--------------------->| InitHomeObj |->+               |
  //    |                           +-------------+  |               |
  //    |                                            |               |
  //    |    +-------------------------------------- +               |
  //    |    |                                                       |
  //    |    | emitInit                                              |
  //    |    +------------------------------------------------------>+
  //    |                                                            ^
  //    | [optimized private non-static method]                      |
  //    |   prepareForPrivateMethod   +--------------------+         |
  //    +---------------------------->| PrivateMethodValue |-+       |
  //    |                             +--------------------+ |       |
  //    |                                                    |       |
  //    |  +-------------------------------------------------+       |
  //    |  |                                                         |
  //    |  +-+---------------------------------------------+         |
  //    |    |                                             |         |
  //    |    | [method with super                          |         |
  //    |    | emitInitHomeObject   +-----------------+    v         |
  //    |    +--------------------->| InitHomeObjFor- |----+         |
  //    |                           | PrivateMethod   |    |         |
  //    |                           +-----------------+    |         |
  //    |                                                  |         |
  //    |    +---------------------------------------------+         |
  //    |    |                                                       |
  //    |    | skipInit                                              |
  //    |    +------------------------------------------------------>+
  //    |                                                            |
  //    | [index property/method/accessor]                           |
  //    |   prepareForIndexPropKey  +----------+                     |
  //    +-------------------------->| IndexKey |-+                   |
  //    |                           +----------+ |                   |
  //    |                                        |                   |
  //    |  +-------------------------------------+                   |
  //    |  |                                                         |
  //    |  | prepareForIndexPropValue +------------+                 |
  //    |  +------------------------->| IndexValue |-+               |
  //    |                             +------------+ |               |
  //    |                                            |               |
  //    |    +---------------------------------------+               |
  //    |    |                                                       |
  //    |    +-+--------------------------------------------------+  |
  //    |      |                                                  |  |
  //    |      | [method with super]                              |  |
  //    |      |   emitInitHomeObject +---------------------+     v  |
  //    |      +--------------------->| InitHomeObjForIndex |---->+  |
  //    |                             +---------------------+     |  |
  //    |                                                         |  |
  //    |      +--------------------------------------------------+  |
  //    |      |                                                     |
  //    |      | emitInitIndexOrComputed                             |
  //    |      +---------------------------------------------------->+
  //    |                                                            |
  //    | [computed property/method/accessor]                        |
  //    |   prepareForComputedPropKey  +-------------+               |
  //    +----------------------------->| ComputedKey |-+             |
  //    |                              +-------------+ |             |
  //    |                                              |             |
  //    |  +-------------------------------------------+             |
  //    |  |                                                         |
  //    |  | prepareForComputedPropValue +---------------+           |
  //    |  +---------------------------->| ComputedValue |-+         |
  //    |                                +---------------+ |         |
  //    |                                                  |         |
  //    |    +---------------------------------------------+         |
  //    |    |                                                       |
  //    |    +-+--------------------------------------------------+  |
  //    |      |                                                  |  |
  //    |      | [method with super]                              |  |
  //    |      |   emitInitHomeObject +------------------------+  v  |
  //    |      +--------------------->| InitHomeObjForComputed |->+  |
  //    |                             +------------------------+  |  |
  //    |                                                         |  |
  //    |      +--------------------------------------------------+  |
  //    |      |                                                     |
  //    |      | emitInitIndexOrComputed                             |
  //    |      +---------------------------------------------------->+
  //    |                                                            ^
  //    |                                                            |
  //    | [__proto__]                                                |
  //    |   prepareForProtoValue  +------------+ emitMutateProto     |
  //    +------------------------>| ProtoValue |-------------------->+
  //    |                         +------------+                     ^
  //    |                                                            |
  //    | [...prop]                                                  |
  //    |   prepareForSpreadOperand +---------------+ emitSpread     |
  //    +-------------------------->| SpreadOperand |----------------+
  //                                +---------------+
  enum class PropertyState {
    // The initial state.
    Start,

    // After calling prepareForPropValue.
    PropValue,

    // After calling emitInitHomeObject, from PropValue.
    InitHomeObj,

    // After calling prepareForPrivateMethod.
    PrivateMethodValue,

    // After calling emitInitHomeObject, from PrivateMethod.
    InitHomeObjForPrivateMethod,

    // After calling prepareForIndexPropKey.
    IndexKey,

    // prepareForIndexPropValue.
    IndexValue,

    // After calling emitInitHomeObject, from IndexValue.
    InitHomeObjForIndex,

    // After calling prepareForComputedPropKey.
    ComputedKey,

    // prepareForComputedPropValue.
    ComputedValue,

    // After calling emitInitHomeObject, from ComputedValue.
    InitHomeObjForComputed,

    // After calling prepareForProtoValue.
    ProtoValue,

    // After calling prepareForSpreadOperand.
    SpreadOperand,

    // After calling one of emitInit, emitInitIndexOrComputed, emitMutateProto,
    // or emitSpread.
    Init,
  };
  PropertyState propertyState_ = PropertyState::Start;
#endif

 public:
  explicit PropertyEmitter(BytecodeEmitter* bce);

  // Parameters are the offset in the source code for each character below:
  //
  // { __proto__: protoValue }
  //   ^
  //   |
  //   keyPos
  [[nodiscard]] bool prepareForProtoValue(
      const mozilla::Maybe<uint32_t>& keyPos);
  [[nodiscard]] bool emitMutateProto();

  // { ...obj }
  //   ^
  //   |
  //   spreadPos
  [[nodiscard]] bool prepareForSpreadOperand(
      const mozilla::Maybe<uint32_t>& spreadPos);
  [[nodiscard]] bool emitSpread();

  // { key: value }
  //   ^
  //   |
  //   keyPos
  [[nodiscard]] bool prepareForPropValue(const mozilla::Maybe<uint32_t>& keyPos,
                                         Kind kind = Kind::Prototype);

  [[nodiscard]] bool prepareForPrivateMethod();

  // { 1: value }
  //   ^
  //   |
  //   keyPos
  [[nodiscard]] bool prepareForIndexPropKey(
      const mozilla::Maybe<uint32_t>& keyPos, Kind kind = Kind::Prototype);
  [[nodiscard]] bool prepareForIndexPropValue();

  // { [ key ]: value }
  //   ^
  //   |
  //   keyPos
  [[nodiscard]] bool prepareForComputedPropKey(
      const mozilla::Maybe<uint32_t>& keyPos, Kind kind = Kind::Prototype);
  [[nodiscard]] bool prepareForComputedPropValue();

  [[nodiscard]] bool emitInitHomeObject();

  // @param key
  //        Property key
  [[nodiscard]] bool emitInit(AccessorType accessorType,
                              TaggedParserAtomIndex key);

  [[nodiscard]] bool emitInitIndexOrComputed(AccessorType accessorType);

  [[nodiscard]] bool skipInit();

 private:
  [[nodiscard]] MOZ_ALWAYS_INLINE bool prepareForProp(
      const mozilla::Maybe<uint32_t>& keyPos, bool isStatic, bool isComputed);

  // @param op
  //        Opcode for initializing property
  // @param key
  //        Atom of the property if the property key is not computed
  [[nodiscard]] bool emitInit(JSOp op, TaggedParserAtomIndex key);
  [[nodiscard]] bool emitInitIndexOrComputed(JSOp op);

  [[nodiscard]] bool emitPopClassConstructor();
};

// Class for emitting bytecode for object literal.
//
// Usage: (check for the return value is omitted for simplicity)
//
//   `{}`
//     ObjectEmitter oe(this);
//     oe.emitObject(0);
//     oe.emitEnd();
//
//   `{ prop: 10 }`
//     ObjectEmitter oe(this);
//     oe.emitObject(1);
//
//     oe.prepareForPropValue(Some(offset_of_prop));
//     emit(10);
//     oe.emitInitProp(atom_of_prop);
//
//     oe.emitEnd();
//
//   `{ prop: function() {} }`, when property value is anonymous function
//     ObjectEmitter oe(this);
//     oe.emitObject(1);
//
//     oe.prepareForPropValue(Some(offset_of_prop));
//     emit(function);
//     oe.emitInitProp(atom_of_prop);
//
//     oe.emitEnd();
//
//   `{ get prop() { ... }, set prop(v) { ... } }`
//     ObjectEmitter oe(this);
//     oe.emitObject(2);
//
//     oe.prepareForPropValue(Some(offset_of_prop));
//     emit(function_for_getter);
//     oe.emitInitGetter(atom_of_prop);
//
//     oe.prepareForPropValue(Some(offset_of_prop));
//     emit(function_for_setter);
//     oe.emitInitSetter(atom_of_prop);
//
//     oe.emitEnd();
//
//   `{ 1: 10, get 2() { ... }, set 3(v) { ... } }`
//     ObjectEmitter oe(this);
//     oe.emitObject(3);
//
//     oe.prepareForIndexPropKey(Some(offset_of_prop));
//     emit(1);
//     oe.prepareForIndexPropValue();
//     emit(10);
//     oe.emitInitIndexedProp();
//
//     oe.prepareForIndexPropKey(Some(offset_of_opening_bracket));
//     emit(2);
//     oe.prepareForIndexPropValue();
//     emit(function_for_getter);
//     oe.emitInitIndexGetter();
//
//     oe.prepareForIndexPropKey(Some(offset_of_opening_bracket));
//     emit(3);
//     oe.prepareForIndexPropValue();
//     emit(function_for_setter);
//     oe.emitInitIndexSetter();
//
//     oe.emitEnd();
//
//   `{ [prop1]: 10, get [prop2]() { ... }, set [prop3](v) { ... } }`
//     ObjectEmitter oe(this);
//     oe.emitObject(3);
//
//     oe.prepareForComputedPropKey(Some(offset_of_opening_bracket));
//     emit(prop1);
//     oe.prepareForComputedPropValue();
//     emit(10);
//     oe.emitInitComputedProp();
//
//     oe.prepareForComputedPropKey(Some(offset_of_opening_bracket));
//     emit(prop2);
//     oe.prepareForComputedPropValue();
//     emit(function_for_getter);
//     oe.emitInitComputedGetter();
//
//     oe.prepareForComputedPropKey(Some(offset_of_opening_bracket));
//     emit(prop3);
//     oe.prepareForComputedPropValue();
//     emit(function_for_setter);
//     oe.emitInitComputedSetter();
//
//     oe.emitEnd();
//
//   `{ __proto__: obj }`
//     ObjectEmitter oe(this);
//     oe.emitObject(1);
//     oe.prepareForProtoValue(Some(offset_of___proto__));
//     emit(obj);
//     oe.emitMutateProto();
//     oe.emitEnd();
//
//   `{ ...obj }`
//     ObjectEmitter oe(this);
//     oe.emitObject(1);
//     oe.prepareForSpreadOperand(Some(offset_of_triple_dots));
//     emit(obj);
//     oe.emitSpread();
//     oe.emitEnd();
//
class MOZ_STACK_CLASS ObjectEmitter : public PropertyEmitter {
 private:
#ifdef DEBUG
  // The state of this emitter.
  //
  // +-------+ emitObject +--------+
  // | Start |----------->| Object |-+
  // +-------+            +--------+ |
  //                                 |
  //   +-----------------------------+
  //   |
  //   | (do PropertyEmitter operation)  emitEnd  +-----+
  //   +-------------------------------+--------->| End |
  //                                              +-----+
  enum class ObjectState {
    // The initial state.
    Start,

    // After calling emitObject.
    Object,

    // After calling emitEnd.
    End,
  };
  ObjectState objectState_ = ObjectState::Start;
#endif

 public:
  explicit ObjectEmitter(BytecodeEmitter* bce);

  [[nodiscard]] bool emitObject(size_t propertyCount);
  // Same as `emitObject()`, but start with an empty template object already on
  // the stack.
  [[nodiscard]] bool emitObjectWithTemplateOnStack();
  [[nodiscard]] bool emitEnd();
};

// Save and restore the strictness.
// Used by class declaration/expression to temporarily enable strict mode.
class MOZ_RAII AutoSaveLocalStrictMode {
  SharedContext* sc_;
  bool savedStrictness_;

 public:
  explicit AutoSaveLocalStrictMode(SharedContext* sc);
  ~AutoSaveLocalStrictMode();

  // Force restore the strictness now.
  void restore();
};

// Class for emitting bytecode for JS class.
//
// Usage: (check for the return value is omitted for simplicity)
//
//   `class { constructor() { ... } }`
//     ClassEmitter ce(this);
//     ce.emitScope(scopeBindings);
//     ce.emitClass(nullptr, nullptr, false);
//
//     emit(function_for_constructor);
//     ce.emitInitConstructor(/* needsHomeObject = */ false);
//
//     ce.emitEnd(ClassEmitter::Kind::Expression);
//
//   `class X { constructor() { ... } }`
//     ClassEmitter ce(this);
//     ce.emitScope(scopeBindings);
//     ce.emitClass(atom_of_X, nullptr, false);
//
//     emit(function_for_constructor);
//     ce.emitInitConstructor(/* needsHomeObject = */ false);
//
//     ce.emitEnd(ClassEmitter::Kind::Expression);
//
//   `class X extends Y { constructor() { ... } }`
//     ClassEmitter ce(this);
//     ce.emitScope(scopeBindings);
//
//     emit(Y);
//     ce.emitDerivedClass(atom_of_X, nullptr, false);
//
//     emit(function_for_constructor);
//     ce.emitInitConstructor(/* needsHomeObject = */ false);
//
//     ce.emitEnd(ClassEmitter::Kind::Expression);
//
//   `class X extends Y { constructor() { ... super.f(); ... } }`
//     ClassEmitter ce(this);
//     ce.emitScope(scopeBindings);
//
//     emit(Y);
//     ce.emitDerivedClass(atom_of_X, nullptr, false);
//
//     emit(function_for_constructor);
//     // pass true if constructor contains super.prop access
//     ce.emitInitConstructor(/* needsHomeObject = */ true);
//
//     ce.emitEnd(ClassEmitter::Kind::Expression);
//
//   `class X extends Y { field0 = expr0; ... }`
//     ClassEmitter ce(this);
//     ce.emitScope(scopeBindings);
//     emit(Y);
//     ce.emitDerivedClass(atom_of_X, nullptr, false);
//
//     ce.prepareForMemberInitializers(fields.length());
//     for (auto field : fields) {
//       emit(field.initializer_method());
//       ce.emitStoreMemberInitializer();
//     }
//     ce.emitMemberInitializersEnd();
//
//     emit(function_for_constructor);
//     ce.emitInitConstructor(/* needsHomeObject = */ false);
//     ce.emitEnd(ClassEmitter::Kind::Expression);
//
//   `class X { field0 = super.method(); ... }`
//     // after emitClass/emitDerivedClass
//     ce.prepareForMemberInitializers(1);
//     for (auto field : fields) {
//       emit(field.initializer_method());
//       if (field.initializer_contains_super_or_eval()) {
//         ce.emitMemberInitializerHomeObject();
//       }
//       ce.emitStoreMemberInitializer();
//     }
//     ce.emitMemberInitializersEnd();
//
//   `m() {}` in class
//     // after emitInitConstructor
//     ce.prepareForPropValue(Some(offset_of_m));
//     emit(function_for_m);
//     ce.emitInitProp(atom_of_m);
//
//   `m() { super.f(); }` in class
//     // after emitInitConstructor
//     ce.prepareForPropValue(Some(offset_of_m));
//     emit(function_for_m);
//     ce.emitInitHomeObject();
//     ce.emitInitProp(atom_of_m);
//
//   `async m() { super.f(); }` in class
//     // after emitInitConstructor
//     ce.prepareForPropValue(Some(offset_of_m));
//     emit(function_for_m);
//     ce.emitInitHomeObject();
//     ce.emitInitProp(atom_of_m);
//
//   `get p() { super.f(); }` in class
//     // after emitInitConstructor
//     ce.prepareForPropValue(Some(offset_of_p));
//     emit(function_for_p);
//     ce.emitInitHomeObject();
//     ce.emitInitGetter(atom_of_m);
//
//   `static m() {}` in class
//     // after emitInitConstructor
//     ce.prepareForPropValue(Some(offset_of_m),
//                            PropertyEmitter::Kind::Static);
//     emit(function_for_m);
//     ce.emitInitProp(atom_of_m);
//
//   `static get [p]() { super.f(); }` in class
//     // after emitInitConstructor
//     ce.prepareForComputedPropValue(Some(offset_of_m),
//                                    PropertyEmitter::Kind::Static);
//     emit(p);
//     ce.prepareForComputedPropValue();
//     emit(function_for_m);
//     ce.emitInitHomeObject();
//     ce.emitInitComputedGetter();
//
class MOZ_STACK_CLASS ClassEmitter : public PropertyEmitter {
 public:
  enum class Kind {
    // Class expression.
    Expression,

    // Class declaration.
    Declaration,
  };

 private:
  // Pseudocode for class declarations:
  //
  //     class extends BaseExpression {
  //       constructor() { ... }
  //       ...
  //       }
  //
  //
  //   if defined <BaseExpression> {
  //     let heritage = BaseExpression;
  //
  //     if (heritage !== null) {
  //       funProto = heritage;
  //       objProto = heritage.prototype;
  //     } else {
  //       funProto = %FunctionPrototype%;
  //       objProto = null;
  //     }
  //   } else {
  //     objProto = %ObjectPrototype%;
  //   }
  //
  //   let homeObject = ObjectCreate(objProto);
  //
  //   if defined <constructor> {
  //     if defined <BaseExpression> {
  //       cons = DefineMethod(<constructor>, proto=homeObject,
  //       funProto=funProto);
  //     } else {
  //       cons = DefineMethod(<constructor>, proto=homeObject);
  //     }
  //   } else {
  //     if defined <BaseExpression> {
  //       cons = DefaultDerivedConstructor(proto=homeObject,
  //       funProto=funProto);
  //     } else {
  //       cons = DefaultConstructor(proto=homeObject);
  //     }
  //   }
  //
  //   cons.prototype = homeObject;
  //   homeObject.constructor = cons;
  //
  //   EmitPropertyList(...)

  bool isDerived_ = false;

  mozilla::Maybe<TDZCheckCache> tdzCache_;
  mozilla::Maybe<EmitterScope> innerScope_;
  mozilla::Maybe<TDZCheckCache> bodyTdzCache_;
  mozilla::Maybe<EmitterScope> bodyScope_;
  AutoSaveLocalStrictMode strictMode_;

#ifdef DEBUG
  // The state of this emitter.
  //
  // clang-format off
  // +-------+
  // | Start |-+------------------------>+--+------------------------------>+--+
  // +-------+ |                         ^  |                               ^  |
  //           | [has scope]             |  | [has body scope]              |  |
  //           |   emitScope   +-------+ |  |  emitBodyScope  +-----------+ |  |
  //           +-------------->| Scope |-+  +---------------->| BodyScope |-+  |
  //                           +-------+                      +-----------+    |
  //                                                                           |
  //   +-----------------------------------------------------------------------+
  //   |
  //   |   emitClass           +-------+
  //   +-+----------------->+->| Class |-+
  //     |                  ^  +-------+ |
  //     | emitDerivedClass |            |
  //     +------------------+            |
  //                                     |
  //     +-------------------------------+
  //     |
  //     |
  //     |  prepareForMemberInitializers(isStatic = false)
  //     +---------------+
  //     |               |
  //     |      +--------v-------------------+
  //     |      | InstanceMemberInitializers |
  //     |      +----------------------------+
  //     |               |
  //                     | emitMemberInitializersEnd
  //     |               |
  //     |      +--------v----------------------+
  //     |      | InstanceMemberInitializersEnd |
  //     |      +-------------------------------+
  //     |               |
  //     +<--------------+
  //     |
  //     |   emitInitConstructor           +-----------------+
  //     +-------------------------------->| InitConstructor |-+
  //                                       +-----------------+ |
  //                                                           |
  //                                                           |
  //                                                           |
  //     +-----------------------------------------------------+
  //     |
  //     |  prepareForMemberInitializers(isStatic = true)
  //     +---------------+
  //     |               |
  //     |      +--------v-----------------+
  //     |      | StaticMemberInitializers |
  //     |      +--------------------------+
  //     |               |
  //     |               | emitMemberInitializersEnd
  //     |               |
  //     |      +--------v--------------------+
  //     |      | StaticMemberInitializersEnd |
  //     |      +-----------------------------+
  //     |               |
  //     +<--------------+
  //     |
  //     | (do PropertyEmitter operation)
  //     +--------------------------------+
  //                                      |
  //     +-------------+    emitBinding   |
  //     |  BoundName  |<-----------------+
  //     +--+----------+
  //        |
  //        | emitEnd
  //        |
  //     +--v----+
  //     |  End  |
  //     +-------+
  //
  // clang-format on
  enum class ClassState {
    // The initial state.
    Start,

    // After calling emitScope.
    Scope,

    // After calling emitBodyScope.
    BodyScope,

    // After calling emitClass or emitDerivedClass.
    Class,

    // After calling emitInitConstructor.
    InitConstructor,

    // After calling prepareForMemberInitializers(isStatic = false).
    InstanceMemberInitializers,

    // After calling emitMemberInitializersEnd.
    InstanceMemberInitializersEnd,

    // After calling prepareForMemberInitializers(isStatic = true).
    StaticMemberInitializers,

    // After calling emitMemberInitializersEnd.
    StaticMemberInitializersEnd,

    // After calling emitBinding.
    BoundName,

    // After calling emitEnd.
    End,
  };
  ClassState classState_ = ClassState::Start;

  // The state of the members emitter.
  //
  // clang-format off
  //
  //   +-------+
  //   | Start +<-----------------------------+
  //   +-------+                              |
  //       |                                  |
  //       | prepareForMemberInitializer      | emitStoreMemberInitializer
  //       v                                  |
  // +-------------+                          |
  // | Initializer +------------------------->+
  // +-------------+                          |
  //       |                                  |
  //       | emitMemberInitializerHomeObject  |
  //       v                                  |
  // +---------------------------+            |
  // | InitializerWithHomeObject +------------+
  // +---------------------------+
  //
  // clang-format on
  enum class MemberState {
    // After calling prepareForMemberInitializers
    // and 0 or more calls to emitStoreMemberInitializer.
    Start,

    // After calling prepareForMemberInitializer
    Initializer,

    // After calling emitMemberInitializerHomeObject
    InitializerWithHomeObject,
  };
  MemberState memberState_ = MemberState::Start;

  size_t numInitializers_ = 0;
#endif

  TaggedParserAtomIndex name_;
  TaggedParserAtomIndex nameForAnonymousClass_;
  bool hasNameOnStack_ = false;
  mozilla::Maybe<NameOpEmitter> initializersAssignment_;
  size_t initializerIndex_ = 0;

 public:
  explicit ClassEmitter(BytecodeEmitter* bce);

  bool emitScope(LexicalScope::ParserData* scopeBindings);
  bool emitBodyScope(ClassBodyScope::ParserData* scopeBindings);

  // @param name
  //        Name of the class (nullptr if this is anonymous class)
  // @param nameForAnonymousClass
  //        Statically inferred name of the class (only for anonymous classes)
  // @param hasNameOnStack
  //        If true the name is on the stack (only for anonymous classes)
  [[nodiscard]] bool emitClass(TaggedParserAtomIndex name,
                               TaggedParserAtomIndex nameForAnonymousClass,
                               bool hasNameOnStack);
  [[nodiscard]] bool emitDerivedClass(
      TaggedParserAtomIndex name, TaggedParserAtomIndex nameForAnonymousClass,
      bool hasNameOnStack);

  // @param needsHomeObject
  //        True if the constructor contains `super.foo`
  [[nodiscard]] bool emitInitConstructor(bool needsHomeObject);

  [[nodiscard]] bool prepareForMemberInitializers(size_t numInitializers,
                                                  bool isStatic);
  [[nodiscard]] bool prepareForMemberInitializer();
  [[nodiscard]] bool emitMemberInitializerHomeObject(bool isStatic);
  [[nodiscard]] bool emitStoreMemberInitializer();
  [[nodiscard]] bool emitMemberInitializersEnd();

  [[nodiscard]] bool emitBinding();

  [[nodiscard]] bool emitEnd(Kind kind);

 private:
  [[nodiscard]] bool initProtoAndCtor();
};

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_ObjectEmitter_h */
