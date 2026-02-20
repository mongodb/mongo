/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_DecoratorEmitter_h
#define frontend_DecoratorEmitter_h

#include "mozilla/Attributes.h"
#include "mozilla/Maybe.h"

#include "frontend/ParseNode.h"

#include "js/AllocPolicy.h"
#include "js/Vector.h"

namespace js::frontend {

struct BytecodeEmitter;

class MOZ_STACK_CLASS DecoratorEmitter {
 private:
  BytecodeEmitter* bce_;

 public:
  enum Kind { Method, Getter, Setter, Field, Accessor, Class };

  explicit DecoratorEmitter(BytecodeEmitter* bce);

  [[nodiscard]] bool emitApplyDecoratorsToElementDefinition(
      Kind kind, ParseNode* key, ListNode* decorators, bool isStatic);

  [[nodiscard]] bool emitApplyDecoratorsToFieldDefinition(ParseNode* key,
                                                          ListNode* decorators,
                                                          bool isStatic);

  [[nodiscard]] bool emitApplyDecoratorsToAccessorDefinition(
      ParseNode* key, ListNode* decorators, bool isStatic);

  [[nodiscard]] bool emitApplyDecoratorsToClassDefinition(ParseNode* key,
                                                          ListNode* decorators);

  [[nodiscard]] bool emitInitializeFieldOrAccessor();

  [[nodiscard]] bool emitCreateAddInitializerFunction(
      FunctionNode* addInitializerFunction, TaggedParserAtomIndex initializers);

  [[nodiscard]] bool emitCallExtraInitializers(
      TaggedParserAtomIndex extraInitializers);

 private:
  [[nodiscard]] bool emitPropertyKey(ParseNode* key);

  [[nodiscard]] bool emitDecorationState();

  [[nodiscard]] bool emitUpdateDecorationState();

  [[nodiscard]] bool emitCallDecoratorForElement(Kind kind, ParseNode* key,
                                                 bool isStatic,
                                                 ParseNode* decorator);

  [[nodiscard]] bool emitCreateDecoratorAccessObject();

  [[nodiscard]] bool emitCheckIsUndefined();

  [[nodiscard]] bool emitCreateAddInitializerFunction();

  [[nodiscard]] bool emitCreateDecoratorContextObject(Kind kind, ParseNode* key,
                                                      bool isStatic,
                                                      TokenPos pos);

  [[nodiscard]] bool emitHandleNewValueField(TaggedParserAtomIndex atom,
                                             int8_t offset);

  using DecoratorsVector = js::Vector<ParseNode*, 2, js::SystemAllocPolicy>;

  [[nodiscard]] bool reverseDecoratorsToApplicationOrder(
      const ListNode* decorators, DecoratorsVector& vec) const;
};

} /* namespace js::frontend */

#endif /* frontend_DecoratorEmitter_h */
