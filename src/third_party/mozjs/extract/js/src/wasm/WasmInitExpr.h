/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2021 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef wasm_initexpr_h
#define wasm_initexpr_h

#include "wasm/WasmConstants.h"
#include "wasm/WasmSerialize.h"
#include "wasm/WasmTypeDecls.h"
#include "wasm/WasmValType.h"
#include "wasm/WasmValue.h"

namespace js {
namespace wasm {

class Decoder;
struct ModuleEnvironment;

// Validates a constant expression. Returns an optional literal value if the
// final value was from a simple instruction such as i32.const.
[[nodiscard]] bool DecodeConstantExpression(Decoder& d, ModuleEnvironment* env,
                                            ValType expected,
                                            Maybe<LitVal>* literal);

enum class InitExprKind {
  None,
  Literal,
  Variable,
};

// A InitExpr describes a WebAssembly constant expression, such as those used to
// initialize a global, specify memory offsets in data segments, or populate
// element segments. Such expressions are created during decoding and actually
// executed on module instantiation.
//
// An InitExpr can only contain constant instructions, which are defined here in
// the spec:
//
// https://webassembly.github.io/spec/core/valid/instructions.html#constant-expressions

class InitExpr {
  InitExprKind kind_;
  // The bytecode for this constant expression if this is not a literal.
  Bytes bytecode_;
  // The value if this is a literal.
  LitVal literal_;
  // The value type of this constant expression in either case.
  ValType type_;

 public:
  InitExpr() : kind_(InitExprKind::None) {}

  explicit InitExpr(LitVal literal)
      : kind_(InitExprKind::Literal),
        literal_(literal),
        type_(literal.type()) {}

  // Decode and validate a constant expression given at the current
  // position of the decoder. Upon failure, the decoder contains the failure
  // message or else the failure was an OOM.
  static bool decodeAndValidate(Decoder& d, ModuleEnvironment* env,
                                ValType expected, InitExpr* expr);

  // Decode and evaluate a constant expression at the current position of the
  // decoder. Does not validate the expression first, since all InitExprs are
  // required to have been validated. Failure conditions are the same as
  // InitExpr::evaluate, i.e. failing only on OOM.
  [[nodiscard]] static bool decodeAndEvaluate(
      JSContext* cx, Handle<WasmInstanceObject*> instanceObj, Decoder& d,
      ValType expectedType, MutableHandleVal result);

  // Evaluate the constant expresssion with the given context. This may only
  // fail due to an OOM, as all InitExpr's are required to have been validated.
  bool evaluate(JSContext* cx, Handle<WasmInstanceObject*> instanceObj,
                MutableHandleVal result) const;

  bool isLiteral() const { return kind_ == InitExprKind::Literal; }

  // Gets the result of this expression if it was determined to be a literal.
  LitVal literal() const {
    MOZ_ASSERT(isLiteral());
    return literal_;
  }

  // Get the type of the resulting value of this expression.
  ValType type() const { return type_; }

  // Allow moving, but not implicit copying
  InitExpr(const InitExpr&) = delete;
  InitExpr& operator=(const InitExpr&) = delete;
  InitExpr(InitExpr&&) = default;
  InitExpr& operator=(InitExpr&&) = default;

  // Allow explicit cloning
  [[nodiscard]] bool clone(const InitExpr& src);

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
  WASM_DECLARE_FRIEND_SERIALIZE(InitExpr);
};

}  // namespace wasm
}  // namespace js

#endif  // wasm_initexpr_h
