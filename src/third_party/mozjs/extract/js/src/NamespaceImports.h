/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// This file imports some common JS:: names into the js namespace so we can
// make unqualified references to them.

#ifndef NamespaceImports_h
#define NamespaceImports_h

// These includes are needed these for some typedefs (e.g. HandleValue) and
// functions (e.g. NullValue())...
#include "js/CallNonGenericMethod.h"
#include "js/GCHashTable.h"
#include "js/GCVector.h"
#include "js/TypeDecls.h"
#include "js/Value.h"

// ... but we do forward declarations of the structs and classes not pulled in
// by the headers included above.
namespace JS {

using ValueVector = JS::GCVector<JS::Value>;
using IdVector = JS::GCVector<jsid>;
using ScriptVector = JS::GCVector<JSScript*>;

class HandleValueArray;

class ObjectOpResult;

class JS_PUBLIC_API PropertyDescriptor;

}  // namespace JS

// Do the importing.
namespace js {

using JS::BooleanValue;
using JS::DoubleValue;
using JS::Float32Value;
using JS::Int32Value;
using JS::MagicValue;
using JS::NullValue;
using JS::NumberValue;
using JS::ObjectOrNullValue;
using JS::ObjectValue;
using JS::PrivateGCThingValue;
using JS::PrivateUint32Value;
using JS::PrivateValue;
using JS::StringValue;
using JS::UndefinedValue;
using JS::Value;
using JS::ValueType;

using JS::Latin1Char;
using JS::UniqueChars;
using JS::UniqueLatin1Chars;
using JS::UniqueTwoByteChars;

using JS::Ok;
using JS::OOM;
using JS::Result;

using JS::HandleIdVector;
using JS::HandleObjectVector;
using JS::HandleValueVector;
using JS::MutableHandleIdVector;
using JS::MutableHandleObjectVector;
using JS::MutableHandleValueVector;
using JS::RootedIdVector;
using JS::RootedObjectVector;
using JS::RootedValueVector;

using JS::IdVector;
using JS::ScriptVector;
using JS::ValueVector;

using JS::GCHashMap;
using JS::GCHashSet;
using JS::GCVector;

using JS::CallArgs;
using JS::CallNonGenericMethod;
using JS::IsAcceptableThis;
using JS::NativeImpl;

using JS::Rooted;
using JS::RootedBigInt;
using JS::RootedFunction;
using JS::RootedId;
using JS::RootedObject;
using JS::RootedScript;
using JS::RootedString;
using JS::RootedSymbol;
using JS::RootedValue;

using JS::PersistentRooted;
using JS::PersistentRootedBigInt;
using JS::PersistentRootedFunction;
using JS::PersistentRootedId;
using JS::PersistentRootedObject;
using JS::PersistentRootedScript;
using JS::PersistentRootedString;
using JS::PersistentRootedSymbol;
using JS::PersistentRootedValue;

using JS::Handle;
using JS::HandleBigInt;
using JS::HandleFunction;
using JS::HandleId;
using JS::HandleObject;
using JS::HandleScript;
using JS::HandleString;
using JS::HandleSymbol;
using JS::HandleValue;

using JS::MutableHandle;
using JS::MutableHandleBigInt;
using JS::MutableHandleFunction;
using JS::MutableHandleId;
using JS::MutableHandleObject;
using JS::MutableHandleScript;
using JS::MutableHandleString;
using JS::MutableHandleSymbol;
using JS::MutableHandleValue;

using JS::FalseHandleValue;
using JS::NullHandleValue;
using JS::TrueHandleValue;
using JS::UndefinedHandleValue;

using JS::HandleValueArray;

using JS::ObjectOpResult;

using JS::PropertyDescriptor;
using JS::PropertyKey;

using JS::Compartment;
using JS::Realm;
using JS::Zone;

using JS::BigInt;

} /* namespace js */

#endif /* NamespaceImports_h */
