/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_VMFunctions_h
#define jit_VMFunctions_h

#include "mozilla/Attributes.h"
#include "mozilla/HashFunctions.h"

#include "jspubtd.h"

#include "jit/CompileInfo.h"
#include "jit/JitFrames.h"
#include "vm/Interpreter.h"

namespace js {

class NamedLambdaObject;
class WithScope;
class InlineTypedObject;
class GeneratorObject;
class RegExpObject;
class TypedArrayObject;

namespace jit {

enum DataType : uint8_t {
    Type_Void,
    Type_Bool,
    Type_Int32,
    Type_Double,
    Type_Pointer,
    Type_Object,
    Type_Value,
    Type_Handle
};

struct PopValues
{
    uint8_t numValues;

    explicit constexpr PopValues(uint8_t numValues)
      : numValues(numValues)
    { }
};

enum MaybeTailCall : bool {
    TailCall,
    NonTailCall
};

// Contains information about a virtual machine function that can be called
// from JIT code. Functions described in this manner must conform to a simple
// protocol: the return type must have a special "failure" value (for example,
// false for bool, or nullptr for Objects). If the function is designed to
// return a value that does not meet this requirement - such as
// object-or-nullptr, or an integer, an optional, final outParam can be
// specified. In this case, the return type must be boolean to indicate
// failure.
//
// All functions described by VMFunction take a JSContext * as a first
// argument, and are treated as re-entrant into the VM and therefore fallible.
struct VMFunction
{
    // Global linked list of all VMFunctions.
    static VMFunction* functions;
    VMFunction* next;

    // Address of the C function.
    void* wrapped;

#ifdef JS_TRACE_LOGGING
    const char* name_;
#endif

    // Note: a maximum of seven root types is supported.
    enum RootType : uint8_t {
        RootNone = 0,
        RootObject,
        RootString,
        RootId,
        RootFunction,
        RootValue,
        RootCell
    };

    // Contains an combination of enumerated types used by the gc for marking
    // arguments of the VM wrapper.
    uint64_t argumentRootTypes;

    enum ArgProperties {
        WordByValue = 0,
        DoubleByValue = 1,
        WordByRef = 2,
        DoubleByRef = 3,
        // BitMask version.
        Word = 0,
        Double = 1,
        ByRef = 2
    };

    // Contains properties about the first 16 arguments.
    uint32_t argumentProperties;

    // Which arguments should be passed in float register on platforms that
    // have them.
    uint32_t argumentPassedInFloatRegs;

    // Number of arguments expected, excluding JSContext * as an implicit
    // first argument and an outparam as a possible implicit final argument.
    uint8_t explicitArgs;

    // The root type of the out param if outParam == Type_Handle.
    RootType outParamRootType;

    // The outparam may be any Type_*, and must be the final argument to the
    // function, if not Void. outParam != Void implies that the return type
    // has a boolean failure mode.
    DataType outParam;

    // Type returned by the C function and used by the VMFunction wrapper to
    // check for failures of the C function.  Valid failure/return types are
    // boolean and object pointers which are asserted inside the VMFunction
    // constructor. If the C function use an outparam (!= Type_Void), then
    // the only valid failure/return type is boolean -- object pointers are
    // pointless because the wrapper will only use it to compare it against
    // nullptr before discarding its value.
    DataType returnType;

    // Number of Values the VM wrapper should pop from the stack when it returns.
    // Used by baseline IC stubs so that they can use tail calls to call the VM
    // wrapper.
    uint8_t extraValuesToPop;

    // On some architectures, called functions need to explicitly push their
    // return address, for a tail call, there is nothing to push, so tail-callness
    // needs to be known at compile time.
    MaybeTailCall expectTailCall;

    uint32_t argc() const {
        // JSContext * + args + (OutParam? *)
        return 1 + explicitArgc() + ((outParam == Type_Void) ? 0 : 1);
    }

    DataType failType() const {
        return returnType;
    }

    // Whether this function returns anything more than a boolean flag for
    // failures.
    bool returnsData() const {
        return returnType == Type_Pointer || outParam != Type_Void;
    }

    ArgProperties argProperties(uint32_t explicitArg) const {
        return ArgProperties((argumentProperties >> (2 * explicitArg)) & 3);
    }

    RootType argRootType(uint32_t explicitArg) const {
        return RootType((argumentRootTypes >> (3 * explicitArg)) & 7);
    }

    bool argPassedInFloatReg(uint32_t explicitArg) const {
        return ((argumentPassedInFloatRegs >> explicitArg) & 1) == 1;
    }

#ifdef JS_TRACE_LOGGING
    const char* name() const {
        return name_;
    }
#endif

    // Return the stack size consumed by explicit arguments.
    size_t explicitStackSlots() const {
        size_t stackSlots = explicitArgs;

        // Fetch all double-word flags of explicit arguments.
        uint32_t n =
            ((1 << (explicitArgs * 2)) - 1) // = Explicit argument mask.
            & 0x55555555                    // = Mask double-size args.
            & argumentProperties;

        // Add the number of double-word flags. (expect a few loop
        // iteration)
        while (n) {
            stackSlots++;
            n &= n - 1;
        }
        return stackSlots;
    }

    // Double-size argument which are passed by value are taking the space
    // of 2 C arguments.  This function is used to compute the number of
    // argument expected by the C function.  This is not the same as
    // explicitStackSlots because reference to stack slots may take one less
    // register in the total count.
    size_t explicitArgc() const {
        size_t stackSlots = explicitArgs;

        // Fetch all explicit arguments.
        uint32_t n =
            ((1 << (explicitArgs * 2)) - 1) // = Explicit argument mask.
            & argumentProperties;

        // Filter double-size arguments (0x5 = 0b0101) and remove (& ~)
        // arguments passed by reference (0b1010 >> 1 == 0b0101).
        n = (n & 0x55555555) & ~(n >> 1);

        // Add the number of double-word transfered by value. (expect a few
        // loop iteration)
        while (n) {
            stackSlots++;
            n &= n - 1;
        }
        return stackSlots;
    }

    size_t doubleByRefArgs() const {
        size_t count = 0;

        // Fetch all explicit arguments.
        uint32_t n =
            ((1 << (explicitArgs * 2)) - 1) // = Explicit argument mask.
            & argumentProperties;

        // Filter double-size arguments (0x5 = 0b0101) and take (&) only
        // arguments passed by reference (0b1010 >> 1 == 0b0101).
        n = (n & 0x55555555) & (n >> 1);

        // Add the number of double-word transfered by refference. (expect a
        // few loop iterations)
        while (n) {
            count++;
            n &= n - 1;
        }
        return count;
    }

    constexpr
    VMFunction(void* wrapped, const char* name, uint32_t explicitArgs, uint32_t argumentProperties,
               uint32_t argumentPassedInFloatRegs, uint64_t argRootTypes,
               DataType outParam, RootType outParamRootType, DataType returnType,
               uint8_t extraValuesToPop = 0, MaybeTailCall expectTailCall = NonTailCall)
      : next(nullptr),
        wrapped(wrapped),
#ifdef JS_TRACE_LOGGING
        name_(name),
#endif
        argumentRootTypes(argRootTypes),
        argumentProperties(argumentProperties),
        argumentPassedInFloatRegs(argumentPassedInFloatRegs),
        explicitArgs(explicitArgs),
        outParamRootType(outParamRootType),
        outParam(outParam),
        returnType(returnType),
        extraValuesToPop(extraValuesToPop),
        expectTailCall(expectTailCall)
    { }

    VMFunction(const VMFunction& o)
      : next(nullptr),
        wrapped(o.wrapped),
#ifdef JS_TRACE_LOGGING
        name_(o.name_),
#endif
        argumentRootTypes(o.argumentRootTypes),
        argumentProperties(o.argumentProperties),
        argumentPassedInFloatRegs(o.argumentPassedInFloatRegs),
        explicitArgs(o.explicitArgs),
        outParamRootType(o.outParamRootType),
        outParam(o.outParam),
        returnType(o.returnType),
        extraValuesToPop(o.extraValuesToPop),
        expectTailCall(o.expectTailCall)
    {
        // Check for valid failure/return type.
        MOZ_ASSERT_IF(outParam != Type_Void,
                      returnType == Type_Void ||
                      returnType == Type_Bool);
        MOZ_ASSERT(returnType == Type_Void ||
                   returnType == Type_Bool ||
                   returnType == Type_Object);
        addToFunctions();
    }

    typedef const VMFunction* Lookup;

    static HashNumber hash(const VMFunction* f) {
        // The hash is based on the wrapped function, not the VMFunction*, to
        // avoid generating duplicate wrapper code.
        HashNumber hash = 0;
        hash = mozilla::AddToHash(hash, f->wrapped);
        hash = mozilla::AddToHash(hash, f->expectTailCall);
        return hash;
    }
    static bool match(const VMFunction* f1, const VMFunction* f2) {
        if (f1->wrapped != f2->wrapped ||
            f1->expectTailCall != f2->expectTailCall)
        {
            return false;
        }

        // If this starts failing, add extraValuesToPop to the if-statement and
        // hash() method above.
        MOZ_ASSERT(f1->extraValuesToPop == f2->extraValuesToPop);

        MOZ_ASSERT(strcmp(f1->name_, f2->name_) == 0);
        MOZ_ASSERT(f1->explicitArgs == f2->explicitArgs);
        MOZ_ASSERT(f1->argumentProperties == f2->argumentProperties);
        MOZ_ASSERT(f1->argumentPassedInFloatRegs == f2->argumentPassedInFloatRegs);
        MOZ_ASSERT(f1->outParam == f2->outParam);
        MOZ_ASSERT(f1->returnType == f2->returnType);
        MOZ_ASSERT(f1->argumentRootTypes == f2->argumentRootTypes);
        MOZ_ASSERT(f1->outParamRootType == f2->outParamRootType);
        return true;
    }

  private:
    // Add this to the global list of VMFunctions.
    void addToFunctions();
};

template <class> struct TypeToDataType { /* Unexpected return type for a VMFunction. */ };
template <> struct TypeToDataType<void> { static const DataType result = Type_Void; };
template <> struct TypeToDataType<bool> { static const DataType result = Type_Bool; };
template <> struct TypeToDataType<JSObject*> { static const DataType result = Type_Object; };
template <> struct TypeToDataType<JSFunction*> { static const DataType result = Type_Object; };
template <> struct TypeToDataType<NativeObject*> { static const DataType result = Type_Object; };
template <> struct TypeToDataType<PlainObject*> { static const DataType result = Type_Object; };
template <> struct TypeToDataType<InlineTypedObject*> { static const DataType result = Type_Object; };
template <> struct TypeToDataType<NamedLambdaObject*> { static const DataType result = Type_Object; };
template <> struct TypeToDataType<LexicalEnvironmentObject*> { static const DataType result = Type_Object; };
template <> struct TypeToDataType<ArrayObject*> { static const DataType result = Type_Object; };
template <> struct TypeToDataType<TypedArrayObject*> { static const DataType result = Type_Object; };
template <> struct TypeToDataType<ArrayIteratorObject*> { static const DataType result = Type_Object; };
template <> struct TypeToDataType<StringIteratorObject*> { static const DataType result = Type_Object; };
template <> struct TypeToDataType<JSString*> { static const DataType result = Type_Object; };
template <> struct TypeToDataType<JSFlatString*> { static const DataType result = Type_Object; };
template <> struct TypeToDataType<HandleObject> { static const DataType result = Type_Handle; };
template <> struct TypeToDataType<HandleString> { static const DataType result = Type_Handle; };
template <> struct TypeToDataType<HandlePropertyName> { static const DataType result = Type_Handle; };
template <> struct TypeToDataType<HandleFunction> { static const DataType result = Type_Handle; };
template <> struct TypeToDataType<Handle<NativeObject*> > { static const DataType result = Type_Handle; };
template <> struct TypeToDataType<Handle<InlineTypedObject*> > { static const DataType result = Type_Handle; };
template <> struct TypeToDataType<Handle<ArrayObject*> > { static const DataType result = Type_Handle; };
template <> struct TypeToDataType<Handle<GeneratorObject*> > { static const DataType result = Type_Handle; };
template <> struct TypeToDataType<Handle<PlainObject*> > { static const DataType result = Type_Handle; };
template <> struct TypeToDataType<Handle<WithScope*> > { static const DataType result = Type_Handle; };
template <> struct TypeToDataType<Handle<LexicalScope*> > { static const DataType result = Type_Handle; };
template <> struct TypeToDataType<Handle<Scope*> > { static const DataType result = Type_Handle; };
template <> struct TypeToDataType<HandleScript> { static const DataType result = Type_Handle; };
template <> struct TypeToDataType<HandleValue> { static const DataType result = Type_Handle; };
template <> struct TypeToDataType<MutableHandleValue> { static const DataType result = Type_Handle; };
template <> struct TypeToDataType<HandleId> { static const DataType result = Type_Handle; };

// Convert argument types to properties of the argument known by the jit.
template <class T> struct TypeToArgProperties {
    static const uint32_t result =
        (sizeof(T) <= sizeof(void*) ? VMFunction::Word : VMFunction::Double);
};
template <> struct TypeToArgProperties<const Value&> {
    static const uint32_t result = TypeToArgProperties<Value>::result | VMFunction::ByRef;
};
template <> struct TypeToArgProperties<HandleObject> {
    static const uint32_t result = TypeToArgProperties<JSObject*>::result | VMFunction::ByRef;
};
template <> struct TypeToArgProperties<HandleString> {
    static const uint32_t result = TypeToArgProperties<JSString*>::result | VMFunction::ByRef;
};
template <> struct TypeToArgProperties<HandlePropertyName> {
    static const uint32_t result = TypeToArgProperties<PropertyName*>::result | VMFunction::ByRef;
};
template <> struct TypeToArgProperties<HandleFunction> {
    static const uint32_t result = TypeToArgProperties<JSFunction*>::result | VMFunction::ByRef;
};
template <> struct TypeToArgProperties<Handle<NativeObject*> > {
    static const uint32_t result = TypeToArgProperties<NativeObject*>::result | VMFunction::ByRef;
};
template <> struct TypeToArgProperties<Handle<InlineTypedObject*> > {
    static const uint32_t result = TypeToArgProperties<InlineTypedObject*>::result | VMFunction::ByRef;
};
template <> struct TypeToArgProperties<Handle<ArrayObject*> > {
    static const uint32_t result = TypeToArgProperties<ArrayObject*>::result | VMFunction::ByRef;
};
template <> struct TypeToArgProperties<Handle<GeneratorObject*> > {
    static const uint32_t result = TypeToArgProperties<GeneratorObject*>::result | VMFunction::ByRef;
};
template <> struct TypeToArgProperties<Handle<PlainObject*> > {
    static const uint32_t result = TypeToArgProperties<PlainObject*>::result | VMFunction::ByRef;
};
template <> struct TypeToArgProperties<Handle<RegExpObject*> > {
    static const uint32_t result = TypeToArgProperties<RegExpObject*>::result | VMFunction::ByRef;
};
template <> struct TypeToArgProperties<Handle<WithScope*> > {
    static const uint32_t result = TypeToArgProperties<WithScope*>::result | VMFunction::ByRef;
};
template <> struct TypeToArgProperties<Handle<LexicalScope*> > {
    static const uint32_t result = TypeToArgProperties<LexicalScope*>::result | VMFunction::ByRef;
};
template <> struct TypeToArgProperties<Handle<Scope*> > {
    static const uint32_t result = TypeToArgProperties<Scope*>::result | VMFunction::ByRef;
};
template <> struct TypeToArgProperties<HandleScript> {
    static const uint32_t result = TypeToArgProperties<JSScript*>::result | VMFunction::ByRef;
};
template <> struct TypeToArgProperties<HandleValue> {
    static const uint32_t result = TypeToArgProperties<Value>::result | VMFunction::ByRef;
};
template <> struct TypeToArgProperties<MutableHandleValue> {
    static const uint32_t result = TypeToArgProperties<Value>::result | VMFunction::ByRef;
};
template <> struct TypeToArgProperties<HandleId> {
    static const uint32_t result = TypeToArgProperties<jsid>::result | VMFunction::ByRef;
};
template <> struct TypeToArgProperties<HandleShape> {
    static const uint32_t result = TypeToArgProperties<Shape*>::result | VMFunction::ByRef;
};
template <> struct TypeToArgProperties<HandleObjectGroup> {
    static const uint32_t result = TypeToArgProperties<ObjectGroup*>::result | VMFunction::ByRef;
};

// Convert argument type to whether or not it should be passed in a float
// register on platforms that have them, like x64.
template <class T> struct TypeToPassInFloatReg {
    static const uint32_t result = 0;
};
template <> struct TypeToPassInFloatReg<double> {
    static const uint32_t result = 1;
};

// Convert argument types to root types used by the gc, see MarkJitExitFrame.
template <class T> struct TypeToRootType {
    static const uint32_t result = VMFunction::RootNone;
};
template <> struct TypeToRootType<HandleObject> {
    static const uint32_t result = VMFunction::RootObject;
};
template <> struct TypeToRootType<HandleString> {
    static const uint32_t result = VMFunction::RootString;
};
template <> struct TypeToRootType<HandlePropertyName> {
    static const uint32_t result = VMFunction::RootString;
};
template <> struct TypeToRootType<HandleFunction> {
    static const uint32_t result = VMFunction::RootFunction;
};
template <> struct TypeToRootType<HandleValue> {
    static const uint32_t result = VMFunction::RootValue;
};
template <> struct TypeToRootType<MutableHandleValue> {
    static const uint32_t result = VMFunction::RootValue;
};
template <> struct TypeToRootType<HandleId> {
    static const uint32_t result = VMFunction::RootId;
};
template <> struct TypeToRootType<HandleShape> {
    static const uint32_t result = VMFunction::RootCell;
};
template <> struct TypeToRootType<HandleObjectGroup> {
    static const uint32_t result = VMFunction::RootCell;
};
template <> struct TypeToRootType<HandleScript> {
    static const uint32_t result = VMFunction::RootCell;
};
template <> struct TypeToRootType<Handle<NativeObject*> > {
    static const uint32_t result = VMFunction::RootObject;
};
template <> struct TypeToRootType<Handle<InlineTypedObject*> > {
    static const uint32_t result = VMFunction::RootObject;
};
template <> struct TypeToRootType<Handle<ArrayObject*> > {
    static const uint32_t result = VMFunction::RootObject;
};
template <> struct TypeToRootType<Handle<GeneratorObject*> > {
    static const uint32_t result = VMFunction::RootObject;
};
template <> struct TypeToRootType<Handle<PlainObject*> > {
    static const uint32_t result = VMFunction::RootObject;
};
template <> struct TypeToRootType<Handle<RegExpObject*> > {
    static const uint32_t result = VMFunction::RootObject;
};
template <> struct TypeToRootType<Handle<LexicalScope*> > {
    static const uint32_t result = VMFunction::RootCell;
};
template <> struct TypeToRootType<Handle<WithScope*> > {
    static const uint32_t result = VMFunction::RootCell;
};
template <> struct TypeToRootType<Handle<Scope*> > {
    static const uint32_t result = VMFunction::RootCell;
};
template <class T> struct TypeToRootType<Handle<T> > {
    // Fail for Handle types that aren't specialized above.
};

template <class> struct OutParamToDataType { static const DataType result = Type_Void; };
template <> struct OutParamToDataType<Value*> { static const DataType result = Type_Value; };
template <> struct OutParamToDataType<int*> { static const DataType result = Type_Int32; };
template <> struct OutParamToDataType<uint32_t*> { static const DataType result = Type_Int32; };
template <> struct OutParamToDataType<uint8_t**> { static const DataType result = Type_Pointer; };
template <> struct OutParamToDataType<bool*> { static const DataType result = Type_Bool; };
template <> struct OutParamToDataType<double*> { static const DataType result = Type_Double; };
template <> struct OutParamToDataType<MutableHandleValue> { static const DataType result = Type_Handle; };
template <> struct OutParamToDataType<MutableHandleObject> { static const DataType result = Type_Handle; };
template <> struct OutParamToDataType<MutableHandleString> { static const DataType result = Type_Handle; };

template <class> struct OutParamToRootType {
    static const VMFunction::RootType result = VMFunction::RootNone;
};
template <> struct OutParamToRootType<MutableHandleValue> {
    static const VMFunction::RootType result = VMFunction::RootValue;
};
template <> struct OutParamToRootType<MutableHandleObject> {
    static const VMFunction::RootType result = VMFunction::RootObject;
};
template <> struct OutParamToRootType<MutableHandleString> {
    static const VMFunction::RootType result = VMFunction::RootString;
};

template <class> struct MatchContext { };
template <> struct MatchContext<JSContext*> {
    static const bool valid = true;
};

// Extract the last element of a list of types.
template <typename... ArgTypes>
struct LastArg;

template <>
struct LastArg<>
{
    typedef void Type;
    static constexpr size_t nbArgs = 0;
};

template <typename HeadType>
struct LastArg<HeadType>
{
    typedef HeadType Type;
    static constexpr size_t nbArgs = 1;
};

template <typename HeadType, typename... TailTypes>
struct LastArg<HeadType, TailTypes...>
{
    typedef typename LastArg<TailTypes...>::Type Type;
    static constexpr size_t nbArgs = LastArg<TailTypes...>::nbArgs + 1;
};

// Construct a bit mask from a list of types.  The mask is constructed as an OR
// of the mask produced for each argument. The result of each argument is
// shifted by its index, such that the result of the first argument is on the
// low bits of the mask, and the result of the last argument in part of the
// high bits of the mask.
template <template<typename> class Each, typename ResultType, size_t Shift,
          typename... Args>
struct BitMask;

template <template<typename> class Each, typename ResultType, size_t Shift>
struct BitMask<Each, ResultType, Shift>
{
    static constexpr ResultType result = ResultType();
};

template <template<typename> class Each, typename ResultType, size_t Shift,
          typename HeadType, typename... TailTypes>
struct BitMask<Each, ResultType, Shift, HeadType, TailTypes...>
{
    static_assert(ResultType(Each<HeadType>::result) < (1 << Shift),
                  "not enough bits reserved by the shift for individual results");
    static_assert(LastArg<TailTypes...>::nbArgs < (8 * sizeof(ResultType) / Shift),
                  "not enough bits in the result type to store all bit masks");

    static constexpr ResultType result =
        ResultType(Each<HeadType>::result) |
        (BitMask<Each, ResultType, Shift, TailTypes...>::result << Shift);
};

// Extract VMFunction properties based on the signature of the function. The
// properties are used to generate the logic for calling the VM function, and
// also for marking the stack during GCs.
template <typename... Args>
struct FunctionInfo;

template <class R, class Context, typename... Args>
struct FunctionInfo<R (*)(Context, Args...)> : public VMFunction
{
    typedef R (*pf)(Context, Args...);

    static DataType returnType() {
        return TypeToDataType<R>::result;
    }
    static DataType outParam() {
        return OutParamToDataType<typename LastArg<Args...>::Type>::result;
    }
    static RootType outParamRootType() {
        return OutParamToRootType<typename LastArg<Args...>::Type>::result;
    }
    static size_t NbArgs() {
        return LastArg<Args...>::nbArgs;
    }
    static size_t explicitArgs() {
        return NbArgs() - (outParam() != Type_Void ? 1 : 0);
    }
    static uint32_t argumentProperties() {
        return BitMask<TypeToArgProperties, uint32_t, 2, Args...>::result;
    }
    static uint32_t argumentPassedInFloatRegs() {
        return BitMask<TypeToPassInFloatReg, uint32_t, 2, Args...>::result;
    }
    static uint64_t argumentRootTypes() {
        return BitMask<TypeToRootType, uint64_t, 3, Args...>::result;
    }
    explicit FunctionInfo(pf fun, const char* name, PopValues extraValuesToPop = PopValues(0))
        : VMFunction(JS_FUNC_TO_DATA_PTR(void*, fun), name, explicitArgs(),
                     argumentProperties(), argumentPassedInFloatRegs(),
                     argumentRootTypes(), outParam(), outParamRootType(),
                     returnType(), extraValuesToPop.numValues, NonTailCall)
    {
        static_assert(MatchContext<Context>::valid, "Invalid cx type in VMFunction");
    }
    explicit FunctionInfo(pf fun, const char* name, MaybeTailCall expectTailCall,
                          PopValues extraValuesToPop = PopValues(0))
        : VMFunction(JS_FUNC_TO_DATA_PTR(void*, fun), name, explicitArgs(),
                     argumentProperties(), argumentPassedInFloatRegs(),
                     argumentRootTypes(), outParam(), outParamRootType(),
                     returnType(), extraValuesToPop.numValues, expectTailCall)
    {
        static_assert(MatchContext<Context>::valid, "Invalid cx type in VMFunction");
    }
};

class AutoDetectInvalidation
{
    JSContext* cx_;
    IonScript* ionScript_;
    MutableHandleValue rval_;
    bool disabled_;

    void setReturnOverride();

  public:
    AutoDetectInvalidation(JSContext* cx, MutableHandleValue rval, IonScript* ionScript)
      : cx_(cx), ionScript_(ionScript), rval_(rval), disabled_(false)
    {
        MOZ_ASSERT(ionScript);
    }

    AutoDetectInvalidation(JSContext* cx, MutableHandleValue rval);

    void disable() {
        MOZ_ASSERT(!disabled_);
        disabled_ = true;
    }

    bool shouldSetReturnOverride() const {
        return !disabled_ && ionScript_->invalidated();
    }

    ~AutoDetectInvalidation() {
        if (MOZ_UNLIKELY(shouldSetReturnOverride()))
            setReturnOverride();
    }
};

MOZ_MUST_USE bool
InvokeFunction(JSContext* cx, HandleObject obj0, bool constructing, bool ignoresReturnValue,
               uint32_t argc, Value* argv, MutableHandleValue rval);
MOZ_MUST_USE bool
InvokeFunctionShuffleNewTarget(JSContext* cx, HandleObject obj, uint32_t numActualArgs,
                               uint32_t numFormalArgs, Value* argv, MutableHandleValue rval);

class InterpreterStubExitFrameLayout;
bool InvokeFromInterpreterStub(JSContext* cx, InterpreterStubExitFrameLayout* frame);

bool CheckOverRecursed(JSContext* cx);
bool CheckOverRecursedWithExtra(JSContext* cx, BaselineFrame* frame,
                                uint32_t extra, uint32_t earlyCheck);

JSObject* BindVar(JSContext* cx, HandleObject scopeChain);
MOZ_MUST_USE bool
DefVar(JSContext* cx, HandlePropertyName dn, unsigned attrs, HandleObject scopeChain);
MOZ_MUST_USE bool
DefLexical(JSContext* cx, HandlePropertyName dn, unsigned attrs, HandleObject scopeChain);
MOZ_MUST_USE bool
DefGlobalLexical(JSContext* cx, HandlePropertyName dn, unsigned attrs);
MOZ_MUST_USE bool
MutatePrototype(JSContext* cx, HandlePlainObject obj, HandleValue value);
MOZ_MUST_USE bool
InitProp(JSContext* cx, HandleObject obj, HandlePropertyName name, HandleValue value,
         jsbytecode* pc);

template<bool Equal>
bool LooselyEqual(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, bool* res);

template<bool Equal>
bool StrictlyEqual(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, bool* res);

bool LessThan(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, bool* res);
bool LessThanOrEqual(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, bool* res);
bool GreaterThan(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, bool* res);
bool GreaterThanOrEqual(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, bool* res);

template<bool Equal>
bool StringsEqual(JSContext* cx, HandleString left, HandleString right, bool* res);

MOZ_MUST_USE bool StringSplitHelper(JSContext* cx, HandleString str, HandleString sep,
                                    HandleObjectGroup group, uint32_t limit,
                                    MutableHandleValue result);

MOZ_MUST_USE bool ArrayPopDense(JSContext* cx, HandleObject obj, MutableHandleValue rval);
MOZ_MUST_USE bool ArrayPushDense(JSContext* cx, HandleArrayObject arr, HandleValue v,
                                 uint32_t* length);
MOZ_MUST_USE bool ArrayShiftDense(JSContext* cx, HandleObject obj, MutableHandleValue rval);
JSString* ArrayJoin(JSContext* cx, HandleObject array, HandleString sep);
MOZ_MUST_USE bool SetArrayLength(JSContext* cx, HandleObject obj, HandleValue value, bool strict);

MOZ_MUST_USE bool
CharCodeAt(JSContext* cx, HandleString str, int32_t index, uint32_t* code);
JSFlatString* StringFromCharCode(JSContext* cx, int32_t code);
JSString* StringFromCodePoint(JSContext* cx, int32_t codePoint);

MOZ_MUST_USE bool
SetProperty(JSContext* cx, HandleObject obj, HandlePropertyName name, HandleValue value,
            bool strict, jsbytecode* pc);

MOZ_MUST_USE bool
InterruptCheck(JSContext* cx);

void* MallocWrapper(JS::Zone* zone, size_t nbytes);
JSObject* NewCallObject(JSContext* cx, HandleShape shape, HandleObjectGroup group);
JSObject* NewSingletonCallObject(JSContext* cx, HandleShape shape);
JSObject* NewStringObject(JSContext* cx, HandleString str);

bool OperatorIn(JSContext* cx, HandleValue key, HandleObject obj, bool* out);
bool OperatorInI(JSContext* cx, uint32_t index, HandleObject obj, bool* out);

MOZ_MUST_USE bool
GetIntrinsicValue(JSContext* cx, HandlePropertyName name, MutableHandleValue rval);

MOZ_MUST_USE bool
CreateThis(JSContext* cx, HandleObject callee, HandleObject newTarget, MutableHandleValue rval);

void GetDynamicName(JSContext* cx, JSObject* scopeChain, JSString* str, Value* vp);

void PostWriteBarrier(JSRuntime* rt, JSObject* obj);
void PostGlobalWriteBarrier(JSRuntime* rt, JSObject* obj);

enum class IndexInBounds { Yes, Maybe };

template <IndexInBounds InBounds>
void PostWriteElementBarrier(JSRuntime* rt, JSObject* obj, int32_t index);

// If |str| is an index in the range [0, INT32_MAX], return it. If the string
// is not an index in this range, return -1.
int32_t GetIndexFromString(JSString* str);

JSObject* WrapObjectPure(JSContext* cx, JSObject* obj);

MOZ_MUST_USE bool
DebugPrologue(JSContext* cx, BaselineFrame* frame, jsbytecode* pc, bool* mustReturn);
MOZ_MUST_USE bool
DebugEpilogue(JSContext* cx, BaselineFrame* frame, jsbytecode* pc, bool ok);
MOZ_MUST_USE bool
DebugEpilogueOnBaselineReturn(JSContext* cx, BaselineFrame* frame, jsbytecode* pc);
void FrameIsDebuggeeCheck(BaselineFrame* frame);

JSObject* CreateGenerator(JSContext* cx, BaselineFrame* frame);

MOZ_MUST_USE bool
NormalSuspend(JSContext* cx, HandleObject obj, BaselineFrame* frame, jsbytecode* pc,
              uint32_t stackDepth);
MOZ_MUST_USE bool
FinalSuspend(JSContext* cx, HandleObject obj, jsbytecode* pc);
MOZ_MUST_USE bool
InterpretResume(JSContext* cx, HandleObject obj, HandleValue val, HandlePropertyName kind,
                MutableHandleValue rval);
MOZ_MUST_USE bool
DebugAfterYield(JSContext* cx, BaselineFrame* frame);
MOZ_MUST_USE bool
GeneratorThrowOrReturn(JSContext* cx, BaselineFrame* frame, Handle<GeneratorObject*> genObj,
                       HandleValue arg, uint32_t resumeKind);

MOZ_MUST_USE bool
GlobalNameConflictsCheckFromIon(JSContext* cx, HandleScript script);
MOZ_MUST_USE bool
CheckGlobalOrEvalDeclarationConflicts(JSContext* cx, BaselineFrame* frame);
MOZ_MUST_USE bool
InitFunctionEnvironmentObjects(JSContext* cx, BaselineFrame* frame);

MOZ_MUST_USE bool
NewArgumentsObject(JSContext* cx, BaselineFrame* frame, MutableHandleValue res);

JSObject* CopyLexicalEnvironmentObject(JSContext* cx, HandleObject env, bool copySlots);

JSObject* InitRestParameter(JSContext* cx, uint32_t length, Value* rest, HandleObject templateObj,
                            HandleObject res);

MOZ_MUST_USE bool
HandleDebugTrap(JSContext* cx, BaselineFrame* frame, uint8_t* retAddr, bool* mustReturn);
MOZ_MUST_USE bool
OnDebuggerStatement(JSContext* cx, BaselineFrame* frame, jsbytecode* pc, bool* mustReturn);
MOZ_MUST_USE bool
GlobalHasLiveOnDebuggerStatement(JSContext* cx);

MOZ_MUST_USE bool
EnterWith(JSContext* cx, BaselineFrame* frame, HandleValue val, Handle<WithScope*> templ);
MOZ_MUST_USE bool
LeaveWith(JSContext* cx, BaselineFrame* frame);

MOZ_MUST_USE bool
PushLexicalEnv(JSContext* cx, BaselineFrame* frame, Handle<LexicalScope*> scope);
MOZ_MUST_USE bool
PopLexicalEnv(JSContext* cx, BaselineFrame* frame);
MOZ_MUST_USE bool
DebugLeaveThenPopLexicalEnv(JSContext* cx, BaselineFrame* frame, jsbytecode* pc);
MOZ_MUST_USE bool
FreshenLexicalEnv(JSContext* cx, BaselineFrame* frame);
MOZ_MUST_USE bool
DebugLeaveThenFreshenLexicalEnv(JSContext* cx, BaselineFrame* frame, jsbytecode* pc);
MOZ_MUST_USE bool
RecreateLexicalEnv(JSContext* cx, BaselineFrame* frame);
MOZ_MUST_USE bool
DebugLeaveThenRecreateLexicalEnv(JSContext* cx, BaselineFrame* frame, jsbytecode* pc);
MOZ_MUST_USE bool
DebugLeaveLexicalEnv(JSContext* cx, BaselineFrame* frame, jsbytecode* pc);

MOZ_MUST_USE bool
PushVarEnv(JSContext* cx, BaselineFrame* frame, HandleScope scope);
MOZ_MUST_USE bool
PopVarEnv(JSContext* cx, BaselineFrame* frame);

MOZ_MUST_USE bool
InitBaselineFrameForOsr(BaselineFrame* frame, InterpreterFrame* interpFrame,
                             uint32_t numStackValues);

JSObject* CreateDerivedTypedObj(JSContext* cx, HandleObject descr,
                                HandleObject owner, int32_t offset);

MOZ_MUST_USE bool
Recompile(JSContext* cx);
MOZ_MUST_USE bool
ForcedRecompile(JSContext* cx);
JSString* StringReplace(JSContext* cx, HandleString string, HandleString pattern,
                        HandleString repl);

MOZ_MUST_USE bool SetDenseElement(JSContext* cx, HandleNativeObject obj, int32_t index,
                                  HandleValue value, bool strict);

void AssertValidObjectPtr(JSContext* cx, JSObject* obj);
void AssertValidObjectOrNullPtr(JSContext* cx, JSObject* obj);
void AssertValidStringPtr(JSContext* cx, JSString* str);
void AssertValidSymbolPtr(JSContext* cx, JS::Symbol* sym);
void AssertValidValue(JSContext* cx, Value* v);

void MarkValueFromJit(JSRuntime* rt, Value* vp);
void MarkStringFromJit(JSRuntime* rt, JSString** stringp);
void MarkObjectFromJit(JSRuntime* rt, JSObject** objp);
void MarkShapeFromJit(JSRuntime* rt, Shape** shapep);
void MarkObjectGroupFromJit(JSRuntime* rt, ObjectGroup** groupp);

// Helper for generatePreBarrier.
inline void*
JitMarkFunction(MIRType type)
{
    switch (type) {
      case MIRType::Value:
        return JS_FUNC_TO_DATA_PTR(void*, MarkValueFromJit);
      case MIRType::String:
        return JS_FUNC_TO_DATA_PTR(void*, MarkStringFromJit);
      case MIRType::Object:
        return JS_FUNC_TO_DATA_PTR(void*, MarkObjectFromJit);
      case MIRType::Shape:
        return JS_FUNC_TO_DATA_PTR(void*, MarkShapeFromJit);
      case MIRType::ObjectGroup:
        return JS_FUNC_TO_DATA_PTR(void*, MarkObjectGroupFromJit);
      default: MOZ_CRASH();
    }
}

bool ObjectIsCallable(JSObject* obj);
bool ObjectIsConstructor(JSObject* obj);

MOZ_MUST_USE bool
ThrowRuntimeLexicalError(JSContext* cx, unsigned errorNumber);

MOZ_MUST_USE bool
ThrowReadOnlyError(JSContext* cx, HandleObject obj, int32_t index);

MOZ_MUST_USE bool
BaselineThrowUninitializedThis(JSContext* cx, BaselineFrame* frame);

MOZ_MUST_USE bool
BaselineThrowInitializedThis(JSContext* cx);

MOZ_MUST_USE bool
ThrowBadDerivedReturn(JSContext* cx, HandleValue v);

MOZ_MUST_USE bool
ThrowObjectCoercible(JSContext* cx, HandleValue v);

MOZ_MUST_USE bool
BaselineGetFunctionThis(JSContext* cx, BaselineFrame* frame, MutableHandleValue res);

MOZ_MUST_USE bool
CallNativeGetter(JSContext* cx, HandleFunction callee, HandleObject obj,
                 MutableHandleValue result);

MOZ_MUST_USE bool
CallNativeSetter(JSContext* cx, HandleFunction callee, HandleObject obj,
                 HandleValue rhs);

MOZ_MUST_USE bool
EqualStringsHelper(JSString* str1, JSString* str2);

MOZ_MUST_USE bool
CheckIsCallable(JSContext* cx, HandleValue v, CheckIsCallableKind kind);

template <bool HandleMissing>
bool
GetNativeDataProperty(JSContext* cx, JSObject* obj, PropertyName* name, Value* vp);

template <bool HandleMissing>
bool
GetNativeDataPropertyByValue(JSContext* cx, JSObject* obj, Value* vp);

template <bool HasOwn>
bool
HasNativeDataProperty(JSContext* cx, JSObject* obj, Value* vp);

bool
HasNativeElement(JSContext* cx, NativeObject* obj, int32_t index, Value* vp);

template <bool NeedsTypeBarrier>
bool
SetNativeDataProperty(JSContext* cx, JSObject* obj, PropertyName* name, Value* val);

bool
ObjectHasGetterSetter(JSContext* cx, JSObject* obj, Shape* propShape);

JSString*
TypeOfObject(JSObject* obj, JSRuntime* rt);

bool
GetPrototypeOf(JSContext* cx, HandleObject target, MutableHandleValue rval);

void
CloseIteratorFromIon(JSContext* cx, JSObject* obj);

extern const VMFunction SetObjectElementInfo;

} // namespace jit
} // namespace js

#endif /* jit_VMFunctions_h */
