/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2015 Mozilla Foundation
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

#ifndef asmjs_wasm_generator_h
#define asmjs_wasm_generator_h

#include "asmjs/WasmIonCompile.h"
#include "asmjs/WasmStubs.h"
#include "jit/MacroAssembler.h"

namespace js {

class AsmJSModule;
namespace fronted { class TokenStream; }

namespace wasm {

class FunctionGenerator;

struct SlowFunction
{
    SlowFunction(PropertyName* name, unsigned ms, unsigned line, unsigned column)
     : name(name), ms(ms), line(line), column(column)
    {}

    static const unsigned msThreshold = 250;

    PropertyName* name;
    unsigned ms;
    unsigned line;
    unsigned column;
};

typedef Vector<SlowFunction> SlowFunctionVector;

// A ModuleGenerator encapsulates the creation of a wasm module. During the
// lifetime of a ModuleGenerator, a sequence of FunctionGenerators are created
// and destroyed to compile the individual function bodies. After generating all
// functions, ModuleGenerator::finish() must be called to complete the
// compilation and extract the resulting wasm module.
class MOZ_STACK_CLASS ModuleGenerator
{
  public:
    typedef Vector<uint32_t, 0, SystemAllocPolicy> FuncIndexVector;

  private:
    struct FuncPtrTable
    {
        uint32_t numDeclared;
        FuncIndexVector elems;

        explicit FuncPtrTable(uint32_t numDeclared) : numDeclared(numDeclared) {}
        FuncPtrTable(FuncPtrTable&& rhs) : numDeclared(rhs.numDeclared), elems(Move(rhs.elems)) {}
    };
    typedef Vector<FuncPtrTable> FuncPtrTableVector;

    struct SigHashPolicy
    {
        typedef const MallocSig& Lookup;
        static HashNumber hash(Lookup l) { return l.hash(); }
        static bool match(const LifoSig* lhs, Lookup rhs) { return *lhs == rhs; }
    };
    typedef HashSet<const LifoSig*, SigHashPolicy> SigSet;

    ExclusiveContext*                      cx_;
    ScopedJSDeletePtr<AsmJSModule>         module_;

    LifoAlloc                              lifo_;
    jit::TempAllocator                     alloc_;
    jit::MacroAssembler                    masm_;
    SigSet                                 sigs_;

    bool                                   parallel_;
    uint32_t                               outstanding_;
    Vector<CompileTask>                    tasks_;
    Vector<CompileTask*>                   freeTasks_;

    FuncOffsetVector                       funcEntryOffsets_;
    FuncPtrTableVector                     funcPtrTables_;

    SlowFunctionVector                     slowFuncs_;
    mozilla::DebugOnly<FunctionGenerator*> active_;

    bool finishOutstandingTask();
    bool finishTask(CompileTask* task);
    CompileArgs args() const;

  public:
    explicit ModuleGenerator(ExclusiveContext* cx);
    ~ModuleGenerator();

    bool init(ScriptSource* ss, uint32_t srcStart, uint32_t srcBodyStart, bool strict);
    AsmJSModule& module() const { return *module_; }

    const LifoSig* newLifoSig(const MallocSig& sig);
    bool declareFuncPtrTable(uint32_t numElems, uint32_t* funcPtrTableIndex);
    bool defineFuncPtrTable(uint32_t funcPtrTableIndex, FuncIndexVector&& elems);

    bool startFunc(PropertyName* name, unsigned line, unsigned column, FunctionGenerator* fg);
    bool finishFunc(uint32_t funcIndex, const LifoSig& sig, unsigned generateTime, FunctionGenerator* fg);

    bool finish(frontend::TokenStream& ts, ScopedJSDeletePtr<AsmJSModule>* module,
                SlowFunctionVector* slowFuncs);
};

// A FunctionGenerator encapsulates the generation of a single function body.
// ModuleGenerator::startFunc must be called after construction and before doing
// anything else. After the body is complete, ModuleGenerator::finishFunc must
// be called before the FunctionGenerator is destroyed and the next function is
// started.
class MOZ_STACK_CLASS FunctionGenerator
{
    friend class ModuleGenerator;

    ModuleGenerator* m_;
    CompileTask*     task_;
    FuncIR*          func_;

  public:
    FunctionGenerator() : m_(nullptr), task_(nullptr), func_(nullptr) {}
    FuncIR& func() const { MOZ_ASSERT(func_); return *func_; }
};

} // namespace wasm
} // namespace js

#endif // asmjs_wasm_generator_h
