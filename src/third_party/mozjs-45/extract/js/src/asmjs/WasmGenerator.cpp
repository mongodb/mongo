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

#include "asmjs/WasmGenerator.h"

#include "asmjs/AsmJSModule.h"
#include "asmjs/WasmStubs.h"
#ifdef MOZ_VTUNE
# include "vtune/VTuneWrapper.h"
#endif

using namespace js;
using namespace js::jit;
using namespace js::wasm;

static bool
ParallelCompilationEnabled(ExclusiveContext* cx)
{
    // Since there are a fixed number of helper threads and one is already being
    // consumed by this parsing task, ensure that there another free thread to
    // avoid deadlock. (Note: there is at most one thread used for parsing so we
    // don't have to worry about general dining philosophers.)
    if (HelperThreadState().threadCount <= 1 || !CanUseExtraThreads())
        return false;

    // If 'cx' isn't a JSContext, then we are already off the main thread so
    // off-thread compilation must be enabled.
    return !cx->isJSContext() || cx->asJSContext()->runtime()->canUseOffthreadIonCompilation();
}

// ****************************************************************************
// ModuleGenerator

static const unsigned GENERATOR_LIFO_DEFAULT_CHUNK_SIZE = 4 * 1024;
static const unsigned COMPILATION_LIFO_DEFAULT_CHUNK_SIZE = 64 * 1024;

ModuleGenerator::ModuleGenerator(ExclusiveContext* cx)
  : cx_(cx),
    lifo_(GENERATOR_LIFO_DEFAULT_CHUNK_SIZE),
    alloc_(&lifo_),
    masm_(MacroAssembler::AsmJSToken(), &alloc_),
    sigs_(cx),
    parallel_(false),
    outstanding_(0),
    tasks_(cx),
    freeTasks_(cx),
    funcEntryOffsets_(cx),
    funcPtrTables_(cx),
    slowFuncs_(cx),
    active_(nullptr)
{}

ModuleGenerator::~ModuleGenerator()
{
    if (parallel_) {
        // Wait for any outstanding jobs to fail or complete.
        if (outstanding_) {
            AutoLockHelperThreadState lock;
            while (true) {
                CompileTaskVector& worklist = HelperThreadState().wasmWorklist();
                MOZ_ASSERT(outstanding_ >= worklist.length());
                outstanding_ -= worklist.length();
                worklist.clear();

                CompileTaskVector& finished = HelperThreadState().wasmFinishedList();
                MOZ_ASSERT(outstanding_ >= finished.length());
                outstanding_ -= finished.length();
                finished.clear();

                uint32_t numFailed = HelperThreadState().harvestFailedWasmJobs();
                MOZ_ASSERT(outstanding_ >= numFailed);
                outstanding_ -= numFailed;

                if (!outstanding_)
                    break;

                HelperThreadState().wait(GlobalHelperThreadState::CONSUMER);
            }
        }

        MOZ_ASSERT(HelperThreadState().wasmCompilationInProgress);
        HelperThreadState().wasmCompilationInProgress = false;
    } else {
        MOZ_ASSERT(!outstanding_);
    }
}

bool
ModuleGenerator::init(ScriptSource* ss, uint32_t srcStart, uint32_t srcBodyStart, bool strict)
{
    if (!sigs_.init())
        return false;

    module_ = cx_->new_<AsmJSModule>(ss, srcStart, srcBodyStart, strict, cx_->canUseSignalHandlers());
    if (!module_)
        return false;

    uint32_t numTasks;
    if (ParallelCompilationEnabled(cx_) &&
        HelperThreadState().wasmCompilationInProgress.compareExchange(false, true))
    {
#ifdef DEBUG
        {
            AutoLockHelperThreadState lock;
            MOZ_ASSERT(!HelperThreadState().wasmFailed());
            MOZ_ASSERT(HelperThreadState().wasmWorklist().empty());
            MOZ_ASSERT(HelperThreadState().wasmFinishedList().empty());
        }
#endif

        parallel_ = true;
        numTasks = HelperThreadState().maxWasmCompilationThreads();
    } else {
        numTasks = 1;
    }

    if (!tasks_.initCapacity(numTasks))
        return false;
    for (size_t i = 0; i < numTasks; i++)
        tasks_.infallibleEmplaceBack(COMPILATION_LIFO_DEFAULT_CHUNK_SIZE, args());

    if (!freeTasks_.reserve(numTasks))
        return false;
    for (size_t i = 0; i < numTasks; i++)
        freeTasks_.infallibleAppend(&tasks_[i]);

    return true;
}

bool
ModuleGenerator::startFunc(PropertyName* name, unsigned line, unsigned column,
                           FunctionGenerator* fg)
{
    MOZ_ASSERT(!active_);

    if (freeTasks_.empty() && !finishOutstandingTask())
        return false;

    CompileTask* task = freeTasks_.popCopy();
    FuncIR* func = task->lifo().new_<FuncIR>(task->lifo(), name, line, column);
    if (!func)
        return false;

    task->init(*func);
    fg->m_ = this;
    fg->task_ = task;
    fg->func_ = func;
    active_ = fg;
    return true;
}

bool
ModuleGenerator::finishFunc(uint32_t funcIndex, const LifoSig& sig, unsigned generateTime,
                            FunctionGenerator* fg)
{
    MOZ_ASSERT(active_ == fg);

    fg->func_->finish(funcIndex, sig, generateTime);

    if (parallel_) {
        if (!StartOffThreadWasmCompile(cx_, fg->task_))
            return false;
        outstanding_++;
    } else {
        if (!CompileFunction(fg->task_))
            return false;
        if (!finishTask(fg->task_))
            return false;
    }

    fg->m_ = nullptr;
    fg->task_ = nullptr;
    fg->func_ = nullptr;
    active_ = nullptr;
    return true;
}

bool
ModuleGenerator::finish(frontend::TokenStream& ts, ScopedJSDeletePtr<AsmJSModule>* module,
                        SlowFunctionVector* slowFuncs)
{
    MOZ_ASSERT(!active_);

    while (outstanding_ > 0) {
        if (!finishOutstandingTask())
            return false;
    }

    module_->setFunctionBytes(masm_.size());

    JitContext jitContext(CompileRuntime::get(args().runtime));

    // Now that all function definitions have been compiled and their function-
    // entry offsets are all known, patch inter-function calls and fill in the
    // function-pointer table offsets.

    if (!GenerateStubs(masm_, *module_, funcEntryOffsets_))
        return false;

    for (auto& cs : masm_.callSites()) {
        if (!cs.isInternal())
            continue;
        MOZ_ASSERT(cs.kind() == CallSiteDesc::Relative);
        uint32_t callerOffset = cs.returnAddressOffset();
        uint32_t calleeOffset = funcEntryOffsets_[cs.targetIndex()];
        masm_.patchCall(callerOffset, calleeOffset);
    }

    for (unsigned tableIndex = 0; tableIndex < funcPtrTables_.length(); tableIndex++) {
        FuncPtrTable& table = funcPtrTables_[tableIndex];
        AsmJSModule::OffsetVector entryOffsets;
        for (uint32_t funcIndex : table.elems)
            entryOffsets.append(funcEntryOffsets_[funcIndex]);
        module_->funcPtrTable(tableIndex).define(Move(entryOffsets));
    }

    masm_.finish();
    if (masm_.oom())
        return false;

    if (!module_->finish(cx_, ts, masm_))
        return false;

    *module = module_.forget();
    *slowFuncs = Move(slowFuncs_);
    return true;
}

bool
ModuleGenerator::finishOutstandingTask()
{
    MOZ_ASSERT(parallel_);

    CompileTask* task = nullptr;
    {
        AutoLockHelperThreadState lock;
        while (true) {
            MOZ_ASSERT(outstanding_ > 0);

            if (HelperThreadState().wasmFailed())
                return false;

            if (!HelperThreadState().wasmFinishedList().empty()) {
                outstanding_--;
                task = HelperThreadState().wasmFinishedList().popCopy();
                break;
            }

            HelperThreadState().wait(GlobalHelperThreadState::CONSUMER);
        }
    }

    return finishTask(task);
}

bool
ModuleGenerator::finishTask(CompileTask* task)
{
    const FuncIR& func = task->func();
    FunctionCompileResults& results = task->results();

    // Merge the compiled results into the whole-module masm.
    size_t offset = masm_.size();
    if (!masm_.asmMergeWith(results.masm()))
        return false;

    // Create the code range now that we know offset of results in whole masm.
    AsmJSModule::CodeRange codeRange(func.line(), results.offsets());
    codeRange.functionOffsetBy(offset);
    if (!module_->addFunctionCodeRange(func.name(), codeRange))
         return false;

    // Compilation may complete out of order, so cannot simply append().
    if (func.index() >= funcEntryOffsets_.length()) {
        if (!funcEntryOffsets_.resize(func.index() + 1))
            return false;
    }
    funcEntryOffsets_[func.index()] = codeRange.entry();

    // Keep a record of slow functions for printing in the final console message.
    unsigned totalTime = func.generateTime() + results.compileTime();
    if (totalTime >= SlowFunction::msThreshold) {
        if (!slowFuncs_.append(SlowFunction(func.name(), totalTime, func.line(), func.column())))
            return false;
    }

#if defined(MOZ_VTUNE) || defined(JS_ION_PERF)
    AsmJSModule::ProfiledFunction pf(func.name(), codeRange.entry(), codeRange.end(),
                                     func.line(), func.column());
    if (!module().addProfiledFunction(pf))
        return false;
#endif

    task->reset();
    freeTasks_.infallibleAppend(task);
    return true;
}

CompileArgs
ModuleGenerator::args() const
{
    return CompileArgs(cx_->compartment()->runtimeFromAnyThread(),
                       module().usesSignalHandlersForOOB());
}

const LifoSig*
ModuleGenerator::newLifoSig(const MallocSig& sig)
{
    SigSet::AddPtr p = sigs_.lookupForAdd(sig);
    if (p)
        return *p;

    LifoSig* lifoSig = LifoSig::new_(lifo_, sig);
    if (!lifoSig || !sigs_.add(p, lifoSig))
        return nullptr;

    return lifoSig;
}

bool
ModuleGenerator::declareFuncPtrTable(uint32_t numElems, uint32_t* funcPtrTableIndex)
{
    // Here just add an uninitialized FuncPtrTable and claim space in the global
    // data section. Later, 'defineFuncPtrTable' will be called with function
    // indices for all the elements of the table.

    // Avoid easy way to OOM the process.
    if (numElems > 1024 * 1024)
        return false;

    if (!module_->declareFuncPtrTable(numElems, funcPtrTableIndex))
        return false;

    MOZ_ASSERT(*funcPtrTableIndex == funcPtrTables_.length());
    return funcPtrTables_.emplaceBack(numElems);
}

bool
ModuleGenerator::defineFuncPtrTable(uint32_t funcPtrTableIndex, FuncIndexVector&& elems)
{
    // The AsmJSModule needs to know the offsets in the code section which won't
    // be known until 'finish'. So just remember the function indices for now
    // and wait until 'finish' to hand over the offsets to the AsmJSModule.

    FuncPtrTable& table = funcPtrTables_[funcPtrTableIndex];
    if (table.numDeclared != elems.length() || !table.elems.empty())
        return false;

    table.elems = Move(elems);
    return true;
}

