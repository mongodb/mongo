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

enum class InitExprKind {
  None,
  Literal,
  Variable,
};

// A InitExpr describes a deferred initializer expression, used to initialize
// a global or a table element offset. Such expressions are created during
// decoding and actually executed on module instantiation.

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

  // Evaluate the constant expresssion with the given context. This may only
  // fail due to an OOM, as all InitExpr's are required to have been validated.
  bool evaluate(JSContext* cx, const ValVector& globalImportValues,
                HandleWasmInstanceObject instanceObj,
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

  WASM_DECLARE_SERIALIZABLE(InitExpr)
};

}  // namespace wasm
}  // namespace js

#endif  // wasm_initexpr_h
