/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2023 Mozilla Foundation
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

#include "wasm/WasmStaticTypeDefs.h"

#include "wasm/WasmTypeDef.h"

using namespace js;
using namespace js::wasm;

const TypeDef* StaticTypeDefs::arrayMutI16 = nullptr;
const TypeDef* StaticTypeDefs::jsTag = nullptr;

bool StaticTypeDefs::init() {
  RefPtr<TypeContext> types = js_new<TypeContext>();
  if (!types) {
    return false;
  }

  arrayMutI16 = types->addType(ArrayType(StorageType::I16, true));
  if (!arrayMutI16) {
    return false;
  }
  arrayMutI16->recGroup().AddRef();

  ValTypeVector params;
  if (!params.append(ValType(RefType::extern_()))) {
    return false;
  }
  jsTag = types->addType(FuncType(std::move(params), ValTypeVector()));
  if (!jsTag) {
    return false;
  }
  jsTag->recGroup().AddRef();

  return true;
}

void StaticTypeDefs::destroy() {
  if (arrayMutI16) {
    arrayMutI16->recGroup().Release();
    arrayMutI16 = nullptr;
  }
  if (jsTag) {
    jsTag->recGroup().Release();
    jsTag = nullptr;
  }
}

bool StaticTypeDefs::addAllToTypeContext(TypeContext* types) {
  for (const TypeDef* type : {arrayMutI16, jsTag}) {
    MOZ_ASSERT(type, "static TypeDef was not initialized");
    SharedRecGroup recGroup = &type->recGroup();
    MOZ_ASSERT(recGroup->numTypes() == 1);
    if (!types->addRecGroup(recGroup)) {
      return false;
    }
  }
  return true;
}
