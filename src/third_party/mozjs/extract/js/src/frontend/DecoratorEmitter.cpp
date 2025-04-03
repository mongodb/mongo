/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/DecoratorEmitter.h"

#include "frontend/BytecodeEmitter.h"
#include "frontend/CallOrNewEmitter.h"
#include "frontend/IfEmitter.h"
#include "frontend/NameAnalysisTypes.h"
#include "frontend/ObjectEmitter.h"
#include "frontend/ParseNode.h"
#include "frontend/ParserAtom.h"
#include "frontend/WhileEmitter.h"
#include "vm/ThrowMsgKind.h"

using namespace js;
using namespace js::frontend;

DecoratorEmitter::DecoratorEmitter(BytecodeEmitter* bce) : bce_(bce) {}

bool DecoratorEmitter::emitApplyDecoratorsToElementDefinition(
    DecoratorEmitter::Kind kind, ParseNode* key, ListNode* decorators,
    bool isStatic) {
  MOZ_ASSERT(kind != Kind::Field);

  // The DecoratorEmitter expects the value to be decorated to be at the top
  // of the stack prior to this call. It will apply the decorators to this
  // value, possibly replacing the value with a value returned by a decorator.
  //          [stack] VAL

  // Decorators Proposal
  // https://arai-a.github.io/ecma262-compare/?pr=2417&id=sec-applydecoratorstoelementdefinition.
  // 1. Let decorators be elementRecord.[[Decorators]].
  // 2. If decorators is empty, return unused.
  // This is checked by the caller.
  MOZ_ASSERT(!decorators->empty());

  // 3. Let key be elementRecord.[[Key]].
  // 4. Let kind be elementRecord.[[Kind]].
  // 5. For each element decorator of decorators, do
  for (ParseNode* decorator : decorators->contents()) {
    //     5.a. Let decorationState be the Record { [[Finished]]: false }.
    if (!emitDecorationState()) {
      return false;
    }

    if (!emitCallDecorator(kind, key, isStatic, decorator)) {
      return false;
    }

    //     5.i. Set decorationState.[[Finished]] to true.
    if (!emitUpdateDecorationState()) {
      return false;
    }

    // We need to check if the decorator returned undefined, a callable value,
    // or any other value.
    IfEmitter ie(bce_);
    if (!ie.emitIf(mozilla::Nothing())) {
      return false;
    }

    if (!emitCheckIsUndefined()) {
      //          [stack] VAL RETVAL ISUNDEFINED
      return false;
    }

    if (!ie.emitThenElse()) {
      //          [stack] VAL RETVAL
      return false;
    }

    // Pop the undefined RETVAL from the stack, leaving the original value in
    // place.
    if (!bce_->emitPopN(1)) {
      //          [stack] VAL
      return false;
    }

    if (!ie.emitElseIf(mozilla::Nothing())) {
      return false;
    }

    //         5.l.i. If IsCallable(newValue) is true, then
    if (!emitCheckIsCallable()) {
      //              [stack] VAL RETVAL ISCALLABLE_RESULT
      return false;
    }

    if (!ie.emitThenElse()) {
      //          [stack] VAL RETVAL
      return false;
    }

    // clang-format off
    //     5.k. Else if kind is accessor, then
    //         5.k.i. If newValue is an Object, then
    //             5.k.i.1. Let newGetter be ? Get(newValue, "get").
    //             5.k.i.2. If IsCallable(newGetter) is true, set elementRecord.[[Get]] to newGetter.
    //             5.k.i.3. Else if newGetter is not undefined, throw a TypeError exception.
    //             5.k.i.4. Let newSetter be ? Get(newValue, "set").
    //             5.k.i.5. If IsCallable(newSetter) is true, set elementRecord.[[Set]] to newSetter.
    //             5.k.i.6. Else if newSetter is not undefined, throw a TypeError exception.
    //             5.k.i.7. Let initializer be ? Get(newValue, "init").
    //             5.k.i.8. If IsCallable(initializer) is true, append initializer to elementRecord.[[Initializers]].
    //             5.k.i.9. Else if initializer is not undefined, throw a TypeError exception.
    //         5.k.ii. Else if newValue is not undefined, throw a TypeError exception.
    // TODO: See https://bugzilla.mozilla.org/show_bug.cgi?id=1793961.
    // clang-format on
    //     5.l. Else,

    //             5.l.i.1. Perform MakeMethod(newValue, homeObject).
    // MakeMethod occurs in the caller, here we just drop the original method
    // which was an argument to the decorator, and leave the new method
    // returned by the decorator on the stack.
    if (!bce_->emit1(JSOp::Swap)) {
      //          [stack] RETVAL VAL
      return false;
    }
    if (!bce_->emitPopN(1)) {
      //          [stack] RETVAL
      return false;
    }
    //         5.j.ii. Else if initializer is not undefined, throw a TypeError
    //         exception. 5.l.ii. Else if newValue is not undefined, throw a
    //         TypeError exception.
    if (!ie.emitElse()) {
      return false;
    }

    if (!bce_->emitPopN(1)) {
      //          [stack] RETVAL
      return false;
    }

    if (!bce_->emit2(JSOp::ThrowMsg,
                     uint8_t(ThrowMsgKind::DecoratorInvalidReturnType))) {
      return false;
    }

    if (!ie.emitEnd()) {
      return false;
    }
  }

  return true;
}

bool DecoratorEmitter::emitApplyDecoratorsToFieldDefinition(
    ParseNode* key, ListNode* decorators, bool isStatic) {
  // This method creates a new array to contain initializers added by decorators
  // to the stack. start:
  //          [stack]
  // end:
  //          [stack] ARRAY

  // Decorators Proposal
  // https://arai-a.github.io/ecma262-compare/?pr=2417&id=sec-applydecoratorstoelementdefinition.
  // 1. Let decorators be elementRecord.[[Decorators]].
  // 2. If decorators is empty, return unused.
  // This is checked by the caller.
  MOZ_ASSERT(!decorators->empty());

  // If we're apply decorators to a field, we'll push a new array to the stack
  // to hold newly created initializers.
  if (!bce_->emitUint32Operand(JSOp::NewArray, 1)) {
    //          [stack] ARRAY
    return false;
  }

  if (!emitPropertyKey(key)) {
    //          [stack] ARRAY NAME
    return false;
  }

  if (!bce_->emitUint32Operand(JSOp::InitElemArray, 0)) {
    //          [stack] ARRAY
    return false;
  }

  if (!bce_->emit1(JSOp::One)) {
    //          [stack] ARRAY INDEX
    return false;
  }

  // 3. Let key be elementRecord.[[Key]].
  // 4. Let kind be elementRecord.[[Kind]].
  // 5. For each element decorator of decorators, do
  for (ParseNode* decorator : decorators->contents()) {
    //     5.a. Let decorationState be the Record { [[Finished]]: false }.
    if (!emitDecorationState()) {
      return false;
    }

    if (!emitCallDecorator(Kind::Field, key, isStatic, decorator)) {
      //          [stack] ARRAY INDEX RETVAL
      return false;
    }

    //     5.i. Set decorationState.[[Finished]] to true.
    if (!emitUpdateDecorationState()) {
      //          [stack] ARRAY INDEX RETVAL
      return false;
    }

    // We need to check if the decorator returned undefined, a callable value,
    // or any other value.
    IfEmitter ie(bce_);
    if (!ie.emitIf(mozilla::Nothing())) {
      return false;
    }

    if (!emitCheckIsUndefined()) {
      //          [stack] ARRAY INDEX RETVAL ISUNDEFINED
      return false;
    }

    if (!ie.emitThenElse()) {
      //          [stack] ARRAY INDEX RETVAL
      return false;
    }

    // Pop the undefined RETVAL from the stack, leaving the original value in
    // place.
    if (!bce_->emitPopN(1)) {
      //          [stack] ARRAY INDEX
      return false;
    }

    if (!ie.emitElseIf(mozilla::Nothing())) {
      return false;
    }

    //         5.l.i. If IsCallable(newValue) is true, then
    if (!emitCheckIsCallable()) {
      //              [stack] ARRAY INDEX RETVAL ISCALLABLE_RESULT
      return false;
    }

    if (!ie.emitThenElse()) {
      //          [stack] ARRAY INDEX RETVAL
      return false;
    }

    //     5.j. If kind is field, then
    //         5.j.i. If IsCallable(initializer) is true, append initializer
    //         to elementRecord.[[Initializers]].
    if (!bce_->emit1(JSOp::InitElemInc)) {
      //          [stack] ARRAY INDEX
      return false;
    }

    //         5.j.ii. Else if initializer is not undefined, throw a TypeError
    //         exception. 5.l.ii. Else if newValue is not undefined, throw a
    //         TypeError exception.
    if (!ie.emitElse()) {
      return false;
    }

    if (!bce_->emitPopN(1)) {
      //          [stack] ARRAY INDEX
      return false;
    }

    if (!bce_->emit2(JSOp::ThrowMsg,
                     uint8_t(ThrowMsgKind::DecoratorInvalidReturnType))) {
      return false;
    }

    if (!ie.emitEnd()) {
      return false;
    }
  }

  // Pop INDEX
  return bce_->emitPopN(1);
  //          [stack] ARRAY
}

bool DecoratorEmitter::emitInitializeFieldOrAccessor() {
  //          [stack] THIS INITIALIZERS

  // Decorators Proposal
  // https://arai-a.github.io/ecma262-compare/?pr=2417&id=sec-applydecoratorstoelementdefinition.
  //
  // 1. Assert: elementRecord.[[Kind]] is field or accessor.
  // 2. If elementRecord.[[BackingStorageKey]] is present, let fieldName be
  // elementRecord.[[BackingStorageKey]].
  // 3. Else, let fieldName be elementRecord.[[Key]].
  // We've stored the fieldname in the first element of the initializers array.
  if (!bce_->emit1(JSOp::Dup)) {
    //          [stack] THIS INITIALIZERS INITIALIZERS
    return false;
  }

  if (!bce_->emit1(JSOp::Zero)) {
    //          [stack] THIS INITIALIZERS INITIALIZERS INDEX
    return false;
  }

  if (!bce_->emit1(JSOp::GetElem)) {
    //          [stack] THIS INITIALIZERS FIELDNAME
    return false;
  }

  // Retrieve initial value of the field
  if (!bce_->emit1(JSOp::Dup)) {
    //          [stack] THIS INITIALIZERS FIELDNAME FIELDNAME
    return false;
  }

  if (!bce_->emitDupAt(3)) {
    //          [stack] THIS INITIALIZERS FIELDNAME FIELDNAME THIS
    return false;
  }

  if (!bce_->emit1(JSOp::Swap)) {
    //          [stack] THIS INITIALIZERS FIELDNAME THIS FIELDNAME
    return false;
  }

  // 4. Let initValue be undefined.
  // TODO: (See Bug 1817993) At the moment, we're applying the initialization
  // logic in two steps. The pre-decorator initialization code runs, stores
  // the initial value, and then we retrieve it here and apply the initializers
  // added by decorators. We should unify these two steps.
  if (!bce_->emit1(JSOp::GetElem)) {
    //          [stack] THIS INITIALIZERS FIELDNAME VALUE
    return false;
  }

  if (!bce_->emit2(JSOp::Pick, 2)) {
    //            [stack] THIS FIELDNAME VALUE INITIALIZERS
    return false;
  }

  // Retrieve the length of the initializers array.
  if (!bce_->emit1(JSOp::Dup)) {
    //          [stack] THIS FIELDNAME VALUE INITIALIZERS INITIALIZERS
    return false;
  }

  if (!bce_->emitAtomOp(JSOp::GetProp,
                        TaggedParserAtomIndex::WellKnown::length())) {
    //          [stack] THIS FIELDNAME VALUE INITIALIZERS LENGTH
    return false;
  }

  if (!bce_->emit1(JSOp::One)) {
    //          [stack] THIS FIELDNAME VALUE INITIALIZERS LENGTH INDEX
    return false;
  }

  // 5. For each element initializer of elementRecord.[[Initializers]], do
  WhileEmitter wh(bce_);
  // At this point, we have no context to determine offsets in the
  // code for this while statement. Ideally, it would correspond to
  // the field we're initializing.
  if (!wh.emitCond(0, 0, 0)) {
    //          [stack] THIS FIELDNAME VALUE INITIALIZERS LENGTH INDEX
    return false;
  }

  if (!bce_->emit1(JSOp::Dup)) {
    //          [stack] THIS FIELDNAME VALUE INITIALIZERS LENGTH INDEX INDEX
    return false;
  }

  if (!bce_->emitDupAt(2)) {
    //          [stack] THIS FIELDNAME VALUE INITIALIZERS LENGTH INDEX INDEX
    //          LENGTH
    return false;
  }

  if (!bce_->emit1(JSOp::Lt)) {
    //          [stack] THIS FIELDNAME VALUE INITIALIZERS LENGTH INDEX BOOL
    return false;
  }

  //     a. Set initValue to ? Call(initializer, receiver, « initValue »).
  if (!wh.emitBody()) {
    //          [stack] THIS FIELDNAME VALUE INITIALIZERS LENGTH INDEX
    return false;
  }

  if (!bce_->emitDupAt(2)) {
    //          [stack] THIS FIELDNAME VALUE INITIALIZERS LENGTH INDEX
    //          INITIALIZERS
    return false;
  }

  if (!bce_->emitDupAt(1)) {
    //          [stack] THIS FIELDNAME VALUE INITIALIZERS LENGTH INDEX
    //          INITIALIZERS INDEX
    return false;
  }

  if (!bce_->emit1(JSOp::GetElem)) {
    //          [stack] THIS FIELDNAME VALUE INITIALIZERS LENGTH INDEX FUNC
    return false;
  }

  // This is guaranteed to run after super(), so we don't need TDZ checks.
  if (!bce_->emitGetName(TaggedParserAtomIndex::WellKnown::dotThis())) {
    //            [stack] THIS FIELDNAME VALUE INITIALIZERS LENGTH INDEX FUNC
    //            THIS
    return false;
  }

  // Pass value in as argument to the initializer
  if (!bce_->emit2(JSOp::Pick, 5)) {
    //            [stack] THIS FIELDNAME INITIALIZERS LENGTH INDEX FUNC THIS
    //            VALUE
    return false;
  }

  // Callee is always internal function.
  if (!bce_->emitCall(JSOp::Call, 1)) {
    //            [stack] THIS FIELDNAME INITIALIZERS LENGTH INDEX RVAL
    return false;
  }

  // Store returned value for next iteration
  if (!bce_->emit2(JSOp::Unpick, 3)) {
    //            [stack] THIS FIELDNAME VALUE INITIALIZERS LENGTH INDEX
    return false;
  }

  if (!bce_->emit1(JSOp::Inc)) {
    //          [stack] THIS FIELDNAME VALUE INITIALIZERS LENGTH INDEX
    return false;
  }

  if (!wh.emitEnd()) {
    //          [stack] THIS FIELDNAME VALUE INITIALIZERS LENGTH INDEX
    return false;
  }

  // 6. If fieldName is a Private Name, then
  //     6.a. Perform ? PrivateFieldAdd(receiver, fieldName, initValue).
  // 7. Else,
  //     6.a. Assert: IsPropertyKey(fieldName) is true.
  //     6.b. Perform ? CreateDataPropertyOrThrow(receiver, fieldName,
  //     initValue).
  // TODO: (See Bug 1817993) Because the field already exists, we just store the
  // updated value here.
  if (!bce_->emitPopN(3)) {
    //          [stack] THIS FIELDNAME VALUE
    return false;
  }

  if (!bce_->emit1(JSOp::InitElem)) {
    //          [stack] THIS
    return false;
  }

  // 8. Return unused.
  return bce_->emitPopN(1);
  //          [stack]
}

bool DecoratorEmitter::emitPropertyKey(ParseNode* key) {
  if (key->is<NameNode>()) {
    NameNode* keyAsNameNode = &key->as<NameNode>();
    if (keyAsNameNode->privateNameKind() == PrivateNameKind::None) {
      if (!bce_->emitStringOp(JSOp::String, keyAsNameNode->atom())) {
        //          [stack] NAME
        return false;
      }
    } else {
      MOZ_ASSERT(keyAsNameNode->privateNameKind() == PrivateNameKind::Field);
      if (!bce_->emitGetPrivateName(keyAsNameNode)) {
        //          [stack] NAME
        return false;
      }
    }
  } else if (key->isKind(ParseNodeKind::NumberExpr)) {
    if (!bce_->emitNumberOp(key->as<NumericLiteral>().value())) {
      //      [stack] NAME
      return false;
    }
  } else {
    // Otherwise this is a computed property name. BigInt keys are parsed
    // as (synthetic) computed property names, too.
    MOZ_ASSERT(key->isKind(ParseNodeKind::ComputedName));

    if (!bce_->emitComputedPropertyName(&key->as<UnaryNode>())) {
      //      [stack] NAME
      return false;
    }
  }

  return true;
}

bool DecoratorEmitter::emitDecorationState() {
  // TODO: See https://bugzilla.mozilla.org/show_bug.cgi?id=1800724.
  return true;
}

bool DecoratorEmitter::emitUpdateDecorationState() {
  // TODO: See https://bugzilla.mozilla.org/show_bug.cgi?id=1800724.
  return true;
}

[[nodiscard]] bool DecoratorEmitter::emitCallDecorator(Kind kind,
                                                       ParseNode* key,
                                                       bool isStatic,
                                                       ParseNode* decorator) {
  // Prepare to call decorator
  CallOrNewEmitter cone(bce_, JSOp::Call,
                        CallOrNewEmitter::ArgumentsKind::Other,
                        ValueUsage::WantValue);

  // DecoratorMemberExpression: IdentifierReference e.g. @dec
  if (decorator->is<NameNode>()) {
    if (!cone.emitNameCallee(decorator->as<NameNode>().name())) {
      //          [stack] VAL? CALLEE THIS?
      return false;
    }
  } else if (decorator->is<ListNode>()) {
    // DecoratorMemberExpression: DecoratorMemberExpression . IdentifierName
    // e.g. @decorators.nested.dec
    PropOpEmitter& poe = cone.prepareForPropCallee(false);
    if (!poe.prepareForObj()) {
      return false;
    }

    ListNode* ln = &decorator->as<ListNode>();
    bool first = true;
    for (ParseNode* node : ln->contentsTo(ln->last())) {
      // We should have only placed NameNode instances in this list while
      // parsing.
      MOZ_ASSERT(node->is<NameNode>());

      if (first) {
        NameNode* obj = &node->as<NameNode>();
        if (!bce_->emitGetName(obj)) {
          return false;
        }
        first = false;
      } else {
        NameNode* prop = &node->as<NameNode>();
        GCThingIndex propAtomIndex;

        if (!bce_->makeAtomIndex(prop->atom(), ParserAtom::Atomize::Yes,
                                 &propAtomIndex)) {
          return false;
        }

        if (!bce_->emitAtomOp(JSOp::GetProp, propAtomIndex)) {
          return false;
        }
      }
    }

    NameNode* prop = &ln->last()->as<NameNode>();
    if (!poe.emitGet(prop->atom())) {
      return false;
    }
  } else {
    // DecoratorCallExpression | DecoratorParenthesizedExpression,
    // e.g. @dec('argument') | @((value, context) => value)
    if (!cone.prepareForFunctionCallee()) {
      return false;
    }

    if (!bce_->emitTree(decorator)) {
      return false;
    }
  }

  if (!cone.emitThis()) {
    //          [stack] VAL? CALLEE THIS
    return false;
  }

  if (!cone.prepareForNonSpreadArguments()) {
    return false;
  }

  if (kind == Kind::Field) {
    //     5.c. Let value be undefined.
    if (!bce_->emit1(JSOp::Undefined)) {
      //          [stack] VAL? CALLEE THIS undefined
      return false;
    }
  } else if (kind == Kind::Getter || kind == Kind::Method ||
             kind == Kind::Setter) {
    //     5.d. If kind is method, set value to elementRecord.[[Value]].
    //     5.e. Else if kind is getter, set value to elementRecord.[[Get]].
    //     5.f. Else if kind is setter, set value to elementRecord.[[Set]].
    // The DecoratorEmitter expects the method to already be on the stack.
    // We dup the value here so we can use it as an argument to the decorator.
    if (!bce_->emitDupAt(2)) {
      //          [stack] VAL CALLEE THIS VAL
      return false;
    }
  }
  //     5.g. Else if kind is accessor, then
  //         5.g.i. Set value to OrdinaryObjectCreate(%Object.prototype%).
  //         5.g.ii. Perform ! CreateDataPropertyOrThrow(accessor, "get",
  //         elementRecord.[[Get]]). 5.g.iii. Perform
  //         ! CreateDataPropertyOrThrow(accessor, "set",
  //         elementRecord.[[Set]]).
  // TODO: See https://bugzilla.mozilla.org/show_bug.cgi?id=1793961.
  //     5.b. Let context be CreateDecoratorContextObject(kind, key,
  //     extraInitializers, decorationState, isStatic).
  if (!emitCreateDecoratorContextObject(kind, key, isStatic,
                                        decorator->pn_pos)) {
    //          [stack] VAL? CALLEE THIS VAL
    //          context
    return false;
  }

  //     5.h. Let newValue be ? Call(decorator, undefined, « value, context
  //     »).
  return cone.emitEnd(2, decorator->pn_pos.begin);
  //          [stack] VAL? RETVAL
}

bool DecoratorEmitter::emitCreateDecoratorAccessObject() {
  // TODO: See https://bugzilla.mozilla.org/show_bug.cgi?id=1800725.
  ObjectEmitter oe(bce_);
  if (!oe.emitObject(0)) {
    return false;
  }
  return oe.emitEnd();
}

bool DecoratorEmitter::emitCheckIsUndefined() {
  // This emits code to check if the value at the top of the stack is
  // undefined. The value is left on the stack.
  //          [stack] VAL
  if (!bce_->emit1(JSOp::Dup)) {
    //          [stack] VAL VAL
    return false;
  }
  if (!bce_->emit1(JSOp::Undefined)) {
    //          [stack] VAL VAL undefined
    return false;
  }
  return bce_->emit1(JSOp::Eq);
  //          [stack] VAL ISUNDEFINED
}

bool DecoratorEmitter::emitCheckIsCallable() {
  // This emits code to check if the value at the top of the stack is
  // callable. The value is left on the stack.
  //            [stack] VAL
  if (!bce_->emitAtomOp(JSOp::GetIntrinsic,
                        TaggedParserAtomIndex::WellKnown::IsCallable())) {
    //            [stack] VAL ISCALLABLE
    return false;
  }
  if (!bce_->emit1(JSOp::Undefined)) {
    //            [stack] VAL ISCALLABLE UNDEFINED
    return false;
  }
  if (!bce_->emitDupAt(2)) {
    //            [stack] VAL ISCALLABLE UNDEFINED VAL
    return false;
  }
  return bce_->emitCall(JSOp::Call, 1);
  //              [stack] VAL ISCALLABLE_RESULT
}

bool DecoratorEmitter::emitCreateAddInitializerFunction() {
  // TODO: See https://bugzilla.mozilla.org/show_bug.cgi?id=1800724.
  ObjectEmitter oe(bce_);
  if (!oe.emitObject(0)) {
    return false;
  }
  return oe.emitEnd();
}

bool DecoratorEmitter::emitCreateDecoratorContextObject(Kind kind,
                                                        ParseNode* key,
                                                        bool isStatic,
                                                        TokenPos pos) {
  // 1. Let contextObj be OrdinaryObjectCreate(%Object.prototype%).
  ObjectEmitter oe(bce_);
  if (!oe.emitObject(/* propertyCount */ 6)) {
    //          [stack] context
    return false;
  }
  if (!oe.prepareForPropValue(pos.begin, PropertyEmitter::Kind::Prototype)) {
    return false;
  }

  if (kind == Kind::Method) {
    // 2. If kind is method, let kindStr be "method".
    if (!bce_->emitStringOp(
            JSOp::String,
            frontend::TaggedParserAtomIndex::WellKnown::method())) {
      //          [stack] context "method"
      return false;
    }
  } else if (kind == Kind::Getter) {
    // 3. Else if kind is getter, let kindStr be "getter".
    if (!bce_->emitStringOp(
            JSOp::String,
            frontend::TaggedParserAtomIndex::WellKnown::getter())) {
      //          [stack] context "getter"
      return false;
    }
  } else if (kind == Kind::Setter) {
    // 4. Else if kind is setter, let kindStr be "setter".
    if (!bce_->emitStringOp(
            JSOp::String,
            frontend::TaggedParserAtomIndex::WellKnown::setter())) {
      //          [stack] context "setter"
      return false;
    }
  } else if (kind == Kind::Field) {
    // 6. Else if kind is field, let kindStr be "field".
    if (!bce_->emitStringOp(
            JSOp::String,
            frontend::TaggedParserAtomIndex::WellKnown::field())) {
      //          [stack] context "field"
      return false;
    }
  } else {
    // clang-format off


    // TODO: See https://bugzilla.mozilla.org/show_bug.cgi?id=1793960.
    // 5. Else if kind is accessor, let kindStr be "accessor".
    // TODO: See https://bugzilla.mozilla.org/show_bug.cgi?id=1793961.
    // 7. Else,
    //     a. Assert: kind is class.
    //     b. Let kindStr be "class".
    // TODO: See https://bugzilla.mozilla.org/show_bug.cgi?id=1793963.
    // clang-format on
    return false;
  }

  // 8. Perform ! CreateDataPropertyOrThrow(contextObj, "kind", kindStr).
  if (!oe.emitInit(frontend::AccessorType::None,
                   frontend::TaggedParserAtomIndex::WellKnown::kind())) {
    //          [stack] context
    return false;
  }
  // 9. If kind is not class, then
  if (kind != Kind::Class) {
    //     9.a. Perform ! CreateDataPropertyOrThrow(contextObj, "access",
    //     CreateDecoratorAccessObject(kind, name)).
    if (!oe.prepareForPropValue(pos.begin, PropertyEmitter::Kind::Prototype)) {
      return false;
    }
    if (!emitCreateDecoratorAccessObject()) {
      return false;
    }
    if (!oe.emitInit(frontend::AccessorType::None,
                     frontend::TaggedParserAtomIndex::WellKnown::access())) {
      //          [stack] context
      return false;
    }
    //     9.b. If isStatic is present, perform
    //     ! CreateDataPropertyOrThrow(contextObj, "static", isStatic).
    if (!oe.prepareForPropValue(pos.begin, PropertyEmitter::Kind::Prototype)) {
      return false;
    }
    if (!bce_->emit1(isStatic ? JSOp::True : JSOp::False)) {
      //          [stack] context isStatic
      return false;
    }
    if (!oe.emitInit(frontend::AccessorType::None,
                     frontend::TaggedParserAtomIndex::WellKnown::static_())) {
      //          [stack] context
      return false;
    }
    //     9.c. If name is a Private Name, then
    //         9.c.i. Perform ! CreateDataPropertyOrThrow(contextObj, "private",
    //         true).
    //     9.d. Else,
    //         9.d.i. Perform ! CreateDataPropertyOrThrow(contextObj, "private",
    //         false).
    if (!oe.prepareForPropValue(pos.begin, PropertyEmitter::Kind::Prototype)) {
      return false;
    }
    if (!bce_->emit1(key->isKind(ParseNodeKind::PrivateName) ? JSOp::True
                                                             : JSOp::False)) {
      //          [stack] context private
      return false;
    }
    if (!oe.emitInit(frontend::AccessorType::None,
                     frontend::TaggedParserAtomIndex::WellKnown::private_())) {
      //          [stack] context
      return false;
    }
    //         9.c.ii. Perform ! CreateDataPropertyOrThrow(contextObj, "name",
    //         name.[[Description]]).
    //
    //         9.d.ii. Perform
    //         ! CreateDataPropertyOrThrow(contextObj, "name", name).)
    if (!oe.prepareForPropValue(pos.begin, PropertyEmitter::Kind::Prototype)) {
      return false;
    }
    if (key->is<NameNode>()) {
      if (!bce_->emitStringOp(JSOp::String, key->as<NameNode>().atom())) {
        return false;
      }
    } else {
      if (!emitPropertyKey(key)) {
        return false;
      }
    }
    if (!oe.emitInit(frontend::AccessorType::None,
                     frontend::TaggedParserAtomIndex::WellKnown::name())) {
      //          [stack] context
      return false;
    }
  } else {
    // 10. Else,
    //     10.a. Perform ! CreateDataPropertyOrThrow(contextObj, "name", name).
    // TODO: See https://bugzilla.mozilla.org/show_bug.cgi?id=1793963
    return false;
  }
  // 11. Let addInitializer be CreateAddInitializerFunction(initializers,
  // decorationState).
  if (!oe.prepareForPropValue(pos.begin, PropertyEmitter::Kind::Prototype)) {
    return false;
  }
  if (!emitCreateAddInitializerFunction()) {
    //          [stack] context addInitializer
    return false;
  }
  // 12. Perform ! CreateDataPropertyOrThrow(contextObj, "addInitializer",
  // addInitializer).
  if (!oe.emitInit(
          frontend::AccessorType::None,
          frontend::TaggedParserAtomIndex::WellKnown::addInitializer())) {
    //          [stack] context
    return false;
  }
  // 13. Return contextObj.
  return oe.emitEnd();
}
