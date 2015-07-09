/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
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
#include "js/TypeDecls.h"
#include "js/Value.h"

// ... but we do forward declarations of the structs and classes not pulled in
// by the headers included above.
namespace JS {

class Latin1Chars;
class Latin1CharsZ;
class ConstTwoByteChars;
class TwoByteChars;
class TwoByteCharsZ;
class UTF8Chars;
class UTF8CharsZ;

class AutoFunctionVector;
class AutoIdVector;
class AutoObjectVector;
class AutoScriptVector;
class AutoValueVector;

class AutoIdArray;

template <typename T> class AutoVectorRooter;
template<typename K, typename V> class AutoHashMapRooter;
template<typename T> class AutoHashSetRooter;
template<typename T> class RootedGeneric;

class MOZ_STACK_CLASS SourceBufferHolder;

class HandleValueArray;

}

// Do the importing.
namespace js {

using JS::Value;
using JS::BooleanValue;
using JS::DoubleValue;
using JS::Float32Value;
using JS::Int32Value;
using JS::IsPoisonedValue;
using JS::MagicValue;
using JS::NullValue;
using JS::NumberValue;
using JS::ObjectOrNullValue;
using JS::ObjectValue;
using JS::PrivateUint32Value;
using JS::PrivateValue;
using JS::StringValue;
using JS::UndefinedValue;

using JS::IsPoisonedPtr;

using JS::Latin1Char;
using JS::Latin1Chars;
using JS::Latin1CharsZ;
using JS::ConstTwoByteChars;
using JS::TwoByteChars;
using JS::TwoByteCharsZ;
using JS::UTF8Chars;
using JS::UTF8CharsZ;

using JS::AutoFunctionVector;
using JS::AutoIdVector;
using JS::AutoObjectVector;
using JS::AutoScriptVector;
using JS::AutoValueVector;

using JS::AutoIdArray;

using JS::AutoHashMapRooter;
using JS::AutoHashSetRooter;
using JS::AutoVectorRooter;
using JS::RootedGeneric;

using JS::CallArgs;
using JS::CallNonGenericMethod;
using JS::CallReceiver;
using JS::CompileOptions;
using JS::IsAcceptableThis;
using JS::NativeImpl;
using JS::OwningCompileOptions;
using JS::ReadOnlyCompileOptions;
using JS::SourceBufferHolder;

using JS::Rooted;
using JS::RootedFunction;
using JS::RootedId;
using JS::RootedObject;
using JS::RootedScript;
using JS::RootedString;
using JS::RootedSymbol;
using JS::RootedValue;

using JS::PersistentRooted;
using JS::PersistentRootedFunction;
using JS::PersistentRootedId;
using JS::PersistentRootedObject;
using JS::PersistentRootedScript;
using JS::PersistentRootedString;
using JS::PersistentRootedSymbol;
using JS::PersistentRootedValue;

using JS::Handle;
using JS::HandleFunction;
using JS::HandleId;
using JS::HandleObject;
using JS::HandleScript;
using JS::HandleString;
using JS::HandleSymbol;
using JS::HandleValue;

using JS::MutableHandle;
using JS::MutableHandleFunction;
using JS::MutableHandleId;
using JS::MutableHandleObject;
using JS::MutableHandleScript;
using JS::MutableHandleString;
using JS::MutableHandleSymbol;
using JS::MutableHandleValue;

using JS::NullHandleValue;
using JS::UndefinedHandleValue;
using JS::TrueHandleValue;
using JS::FalseHandleValue;

using JS::HandleValueArray;

using JS::Zone;

} /* namespace js */

#endif /* NamespaceImports_h */
