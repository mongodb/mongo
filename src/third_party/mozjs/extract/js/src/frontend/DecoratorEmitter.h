/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_DecoratorEmitter_h
#define frontend_DecoratorEmitter_h

#include "mozilla/Attributes.h"

#include "frontend/ParseNode.h"

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

  [[nodiscard]] bool emitInitializeFieldOrAccessor();

 private:
  [[nodiscard]] bool emitPropertyKey(ParseNode* key);

  [[nodiscard]] bool emitDecorationState();

  [[nodiscard]] bool emitUpdateDecorationState();

  [[nodiscard]] bool emitCallDecorator(Kind kind, ParseNode* key, bool isStatic,
                                       ParseNode* decorator);

  [[nodiscard]] bool emitCreateDecoratorAccessObject();

  [[nodiscard]] bool emitCheckIsUndefined();

  [[nodiscard]] bool emitCheckIsCallable();

  [[nodiscard]] bool emitCreateAddInitializerFunction();

  [[nodiscard]] bool emitCreateDecoratorContextObject(Kind kind, ParseNode* key,
                                                      bool isStatic,
                                                      TokenPos pos);
};

} /* namespace js::frontend */

#endif /* frontend_DecoratorEmitter_h */
