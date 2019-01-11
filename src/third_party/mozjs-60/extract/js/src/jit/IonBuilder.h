/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_IonBuilder_h
#define jit_IonBuilder_h

// This file declares the data structures for building a MIRGraph from a
// JSScript.

#include "mozilla/LinkedList.h"

#include "jit/BaselineInspector.h"
#include "jit/BytecodeAnalysis.h"
#include "jit/IonAnalysis.h"
#include "jit/IonControlFlow.h"
#include "jit/IonOptimizationLevels.h"
#include "jit/MIR.h"
#include "jit/MIRGenerator.h"
#include "jit/MIRGraph.h"
#include "jit/OptimizationTracking.h"

namespace js {
namespace jit {

class CodeGenerator;
class CallInfo;
class BaselineFrameInspector;

enum class InlinableNative : uint16_t;

// Records information about a baseline frame for compilation that is stable
// when later used off thread.
BaselineFrameInspector*
NewBaselineFrameInspector(TempAllocator* temp, BaselineFrame* frame);

using CallTargets = Vector<JSFunction*, 6, JitAllocPolicy>;

class IonBuilder
  : public MIRGenerator,
    public mozilla::LinkedListElement<IonBuilder>
{

  public:
    IonBuilder(JSContext* analysisContext, CompileCompartment* comp,
               const JitCompileOptions& options, TempAllocator* temp,
               MIRGraph* graph, CompilerConstraintList* constraints,
               BaselineInspector* inspector, CompileInfo* info,
               const OptimizationInfo* optimizationInfo, BaselineFrameInspector* baselineFrame,
               size_t inliningDepth = 0, uint32_t loopDepth = 0);

    // Callers of build() and buildInline() should always check whether the
    // call overrecursed, if false is returned.  Overrecursion is not
    // signaled as OOM and will not in general be caught by OOM paths.
    AbortReasonOr<Ok> build();
    AbortReasonOr<Ok> buildInline(IonBuilder* callerBuilder, MResumePoint* callerResumePoint,
                                  CallInfo& callInfo);

    mozilla::GenericErrorResult<AbortReason> abort(AbortReason r);
    mozilla::GenericErrorResult<AbortReason>
    abort(AbortReason r, const char* message, ...) MOZ_FORMAT_PRINTF(3, 4);

  private:
    AbortReasonOr<Ok> traverseBytecode();
    AbortReasonOr<Ok> processIterators();
    AbortReasonOr<Ok> inspectOpcode(JSOp op);
    uint32_t readIndex(jsbytecode* pc);
    JSAtom* readAtom(jsbytecode* pc);

    void trackActionableAbort(const char* message);
    void spew(const char* message);

    JSFunction* getSingleCallTarget(TemporaryTypeSet* calleeTypes);
    AbortReasonOr<Ok> getPolyCallTargets(TemporaryTypeSet* calleeTypes, bool constructing,
                                         InliningTargets& targets, uint32_t maxTargets);

    AbortReasonOr<Ok> analyzeNewLoopTypes(const CFGBlock* loopEntryBlock);

    AbortReasonOr<MBasicBlock*> newBlock(size_t stackDepth, jsbytecode* pc,
                                         MBasicBlock* maybePredecessor = nullptr);
    AbortReasonOr<MBasicBlock*> newBlock(MBasicBlock* predecessor, jsbytecode* pc,
                                         MResumePoint* priorResumePoint);
    AbortReasonOr<MBasicBlock*> newBlockPopN(MBasicBlock* predecessor, jsbytecode* pc,
                                             uint32_t popped);
    AbortReasonOr<MBasicBlock*> newBlockAfter(MBasicBlock* at, size_t stackDepth,
                                              jsbytecode* pc, MBasicBlock* maybePredecessor = nullptr);
    AbortReasonOr<MBasicBlock*> newOsrPreheader(MBasicBlock* header, jsbytecode* loopEntry,
                                                jsbytecode* beforeLoopEntry);
    AbortReasonOr<MBasicBlock*> newPendingLoopHeader(MBasicBlock* predecessor, jsbytecode* pc,
                                                     bool osr, bool canOsr, unsigned stackPhiCount);

    AbortReasonOr<MBasicBlock*> newBlock(MBasicBlock* predecessor, jsbytecode* pc) {
        return newBlock(predecessor->stackDepth(), pc, predecessor);
    }

    AbortReasonOr<Ok> visitBlock(const CFGBlock* hblock, MBasicBlock* mblock);
    AbortReasonOr<Ok> visitControlInstruction(CFGControlInstruction* ins, bool* restarted);
    AbortReasonOr<Ok> visitTest(CFGTest* test);
    AbortReasonOr<Ok> visitCompare(CFGCompare* compare);
    AbortReasonOr<Ok> visitLoopEntry(CFGLoopEntry* loopEntry);
    AbortReasonOr<Ok> visitReturn(CFGControlInstruction* ins);
    AbortReasonOr<Ok> visitGoto(CFGGoto* ins);
    AbortReasonOr<Ok> visitBackEdge(CFGBackEdge* ins, bool* restarted);
    AbortReasonOr<Ok> visitTry(CFGTry* test);
    AbortReasonOr<Ok> visitThrow(CFGThrow* ins);
    AbortReasonOr<Ok> visitTableSwitch(CFGTableSwitch* ins);

    // We want to make sure that our MTest instructions all check whether the
    // thing being tested might emulate undefined.  So we funnel their creation
    // through this method, to make sure that happens.  We don't want to just do
    // the check in MTest::New, because that can run on background compilation
    // threads, and we're not sure it's safe to touch that part of the typeset
    // from a background thread.
    MTest* newTest(MDefinition* ins, MBasicBlock* ifTrue, MBasicBlock* ifFalse);

    // Incorporates a type/typeSet into an OSR value for a loop, after the loop
    // body has been processed.
    AbortReasonOr<Ok> addOsrValueTypeBarrier(uint32_t slot, MInstruction** def,
                                             MIRType type, TemporaryTypeSet* typeSet);
    AbortReasonOr<Ok> maybeAddOsrTypeBarriers();

    // Restarts processing of a loop if the type information at its header was
    // incomplete.
    AbortReasonOr<Ok> restartLoop(const CFGBlock* header);
    bool initLoopEntry();

    // Please see the Big Honkin' Comment about how resume points work in
    // IonBuilder.cpp, near the definition for this function.
    AbortReasonOr<Ok> resume(MInstruction* ins, jsbytecode* pc, MResumePoint::Mode mode);
    AbortReasonOr<Ok> resumeAt(MInstruction* ins, jsbytecode* pc);
    AbortReasonOr<Ok> resumeAfter(MInstruction* ins);
    AbortReasonOr<Ok> maybeInsertResume();

    bool blockIsOSREntry(const CFGBlock* block, const CFGBlock* predecessor);

    void insertRecompileCheck();

    bool usesEnvironmentChain();

    AbortReasonOr<Ok> initParameters();
    void initLocals();
    void rewriteParameter(uint32_t slotIdx, MDefinition* param);
    AbortReasonOr<Ok> rewriteParameters();
    AbortReasonOr<Ok> initEnvironmentChain(MDefinition* callee = nullptr);
    void initArgumentsObject();
    void pushConstant(const Value& v);

    MConstant* constant(const Value& v);
    MConstant* constantInt(int32_t i);
    MInstruction* initializedLength(MDefinition* elements);
    MInstruction* setInitializedLength(MDefinition* obj, size_t count);

    // Improve the type information at tests
    AbortReasonOr<Ok> improveTypesAtTest(MDefinition* ins, bool trueBranch, MTest* test);
    AbortReasonOr<Ok> improveTypesAtCompare(MCompare* ins, bool trueBranch, MTest* test);
    AbortReasonOr<Ok> improveTypesAtNullOrUndefinedCompare(MCompare* ins, bool trueBranch,
                                                           MTest* test);
    AbortReasonOr<Ok> improveTypesAtTypeOfCompare(MCompare* ins, bool trueBranch, MTest* test);

    // Used to detect triangular structure at test.
    bool detectAndOrStructure(MPhi* ins, bool* branchIsTrue);
    AbortReasonOr<Ok> replaceTypeSet(MDefinition* subject, TemporaryTypeSet* type, MTest* test);

    // Add a guard which ensure that the set of type which goes through this
    // generated code correspond to the observed types for the bytecode.
    MDefinition* addTypeBarrier(MDefinition* def, TemporaryTypeSet* observed,
                                BarrierKind kind, MTypeBarrier** pbarrier = nullptr);
    AbortReasonOr<Ok> pushTypeBarrier(MDefinition* def, TemporaryTypeSet* observed,
                                      BarrierKind kind);

    // As pushTypeBarrier, but will compute the needBarrier boolean itself based
    // on observed and the JSFunction that we're planning to call. The
    // JSFunction must be a DOM method or getter.
    AbortReasonOr<Ok> pushDOMTypeBarrier(MInstruction* ins, TemporaryTypeSet* observed,
                                         JSFunction* func);

    // If definiteType is not known or def already has the right type, just
    // returns def.  Otherwise, returns an MInstruction that has that definite
    // type, infallibly unboxing ins as needed.  The new instruction will be
    // added to |current| in this case.
    MDefinition* ensureDefiniteType(MDefinition* def, MIRType definiteType);

    void maybeMarkEmpty(MDefinition* ins);

    JSObject* getSingletonPrototype(JSFunction* target);

    MDefinition* createThisScripted(MDefinition* callee, MDefinition* newTarget);
    MDefinition* createThisScriptedSingleton(JSFunction* target);
    MDefinition* createThisScriptedBaseline(MDefinition* callee);
    MDefinition* createThis(JSFunction* target, MDefinition* callee, MDefinition* newTarget);
    MInstruction* createNamedLambdaObject(MDefinition* callee, MDefinition* envObj);
    AbortReasonOr<MInstruction*> createCallObject(MDefinition* callee, MDefinition* envObj);

    MDefinition* walkEnvironmentChain(unsigned hops);

    MInstruction* addConvertElementsToDoubles(MDefinition* elements);
    MDefinition* addMaybeCopyElementsForWrite(MDefinition* object, bool checkNative);

    MInstruction* addBoundsCheck(MDefinition* index, MDefinition* length);

    MInstruction* addShapeGuard(MDefinition* obj, Shape* const shape, BailoutKind bailoutKind);
    MInstruction* addGroupGuard(MDefinition* obj, ObjectGroup* group, BailoutKind bailoutKind);
    MInstruction* addUnboxedExpandoGuard(MDefinition* obj, bool hasExpando, BailoutKind bailoutKind);
    MInstruction* addSharedTypedArrayGuard(MDefinition* obj);

    MInstruction*
    addGuardReceiverPolymorphic(MDefinition* obj, const BaselineInspector::ReceiverVector& receivers);

    bool invalidatedIdempotentCache();

    bool hasStaticEnvironmentObject(JSObject** pcall);
    AbortReasonOr<Ok> loadSlot(MDefinition* obj, size_t slot, size_t nfixed, MIRType rvalType,
                               BarrierKind barrier, TemporaryTypeSet* types);
    AbortReasonOr<Ok> loadSlot(MDefinition* obj, Shape* shape, MIRType rvalType,
                               BarrierKind barrier, TemporaryTypeSet* types);
    AbortReasonOr<Ok> storeSlot(MDefinition* obj, size_t slot, size_t nfixed, MDefinition* value,
                                bool needsBarrier, MIRType slotType = MIRType::None);
    AbortReasonOr<Ok> storeSlot(MDefinition* obj, Shape* shape, MDefinition* value,
                                bool needsBarrier, MIRType slotType = MIRType::None);
    bool shouldAbortOnPreliminaryGroups(MDefinition *obj);

    MDefinition* tryInnerizeWindow(MDefinition* obj);
    MDefinition* maybeUnboxForPropertyAccess(MDefinition* def);

    // jsop_getprop() helpers.
    AbortReasonOr<Ok> checkIsDefinitelyOptimizedArguments(MDefinition* obj, bool* isOptimizedArgs);
    AbortReasonOr<Ok> getPropTryInferredConstant(bool* emitted, MDefinition* obj,
                                                 PropertyName* name, TemporaryTypeSet* types);
    AbortReasonOr<Ok> getPropTryArgumentsLength(bool* emitted, MDefinition* obj);
    AbortReasonOr<Ok> getPropTryArgumentsCallee(bool* emitted, MDefinition* obj,
                                                PropertyName* name);
    AbortReasonOr<Ok> getPropTryConstant(bool* emitted, MDefinition* obj, jsid id,
                                         TemporaryTypeSet* types);
    AbortReasonOr<Ok> getPropTryNotDefined(bool* emitted, MDefinition* obj, jsid id,
                                           TemporaryTypeSet* types);
    AbortReasonOr<Ok> getPropTryDefiniteSlot(bool* emitted, MDefinition* obj, PropertyName* name,
                                             BarrierKind barrier, TemporaryTypeSet* types);
    AbortReasonOr<Ok> getPropTryModuleNamespace(bool* emitted, MDefinition* obj, PropertyName* name,
                                                BarrierKind barrier, TemporaryTypeSet* types);
    AbortReasonOr<Ok> getPropTryUnboxed(bool* emitted, MDefinition* obj, PropertyName* name,
                                        BarrierKind barrier, TemporaryTypeSet* types);
    AbortReasonOr<Ok> getPropTryCommonGetter(bool* emitted, MDefinition* obj, PropertyName* name,
                                             TemporaryTypeSet* types, bool innerized = false);
    AbortReasonOr<Ok> getPropTryInlineAccess(bool* emitted, MDefinition* obj, PropertyName* name,
                                             BarrierKind barrier, TemporaryTypeSet* types);
    AbortReasonOr<Ok> getPropTryInlineProtoAccess(bool* emitted, MDefinition* obj, PropertyName* name,
                                                  TemporaryTypeSet* types);
    AbortReasonOr<Ok> getPropTryTypedObject(bool* emitted, MDefinition* obj, PropertyName* name);
    AbortReasonOr<Ok> getPropTryScalarPropOfTypedObject(bool* emitted, MDefinition* typedObj,
                                                        int32_t fieldOffset,
                                                        TypedObjectPrediction fieldTypeReprs);
    AbortReasonOr<Ok> getPropTryReferencePropOfTypedObject(bool* emitted, MDefinition* typedObj,
                                                           int32_t fieldOffset,
                                                           TypedObjectPrediction fieldPrediction,
                                                           PropertyName* name);
    AbortReasonOr<Ok> getPropTryComplexPropOfTypedObject(bool* emitted, MDefinition* typedObj,
                                                         int32_t fieldOffset,
                                                         TypedObjectPrediction fieldTypeReprs,
                                                         size_t fieldIndex);
    AbortReasonOr<Ok> getPropTryInnerize(bool* emitted, MDefinition* obj, PropertyName* name,
                                         TemporaryTypeSet* types);
    AbortReasonOr<Ok> getPropAddCache(MDefinition* obj, PropertyName* name,
                                      BarrierKind barrier, TemporaryTypeSet* types);

    // jsop_setprop() helpers.
    AbortReasonOr<Ok> setPropTryCommonSetter(bool* emitted, MDefinition* obj,
                                             PropertyName* name, MDefinition* value);
    AbortReasonOr<Ok> setPropTryCommonDOMSetter(bool* emitted, MDefinition* obj,
                                                MDefinition* value, JSFunction* setter,
                                                TemporaryTypeSet* objTypes);
    AbortReasonOr<Ok> setPropTryDefiniteSlot(bool* emitted, MDefinition* obj,
                                             PropertyName* name, MDefinition* value,
                                             bool barrier);
    AbortReasonOr<Ok> setPropTryUnboxed(bool* emitted, MDefinition* obj,
                                        PropertyName* name, MDefinition* value,
                                        bool barrier);
    AbortReasonOr<Ok> setPropTryInlineAccess(bool* emitted, MDefinition* obj,
                                             PropertyName* name, MDefinition* value,
                                             bool barrier, TemporaryTypeSet* objTypes);
    AbortReasonOr<Ok> setPropTryTypedObject(bool* emitted, MDefinition* obj,
                                            PropertyName* name, MDefinition* value);
    AbortReasonOr<Ok> setPropTryReferencePropOfTypedObject(bool* emitted, MDefinition* obj,
                                                           int32_t fieldOffset, MDefinition* value,
                                                           TypedObjectPrediction fieldPrediction,
                                                           PropertyName* name);
    AbortReasonOr<Ok> setPropTryReferenceTypedObjectValue(bool* emitted,
                                                          MDefinition* typedObj,
                                                          const LinearSum& byteOffset,
                                                          ReferenceTypeDescr::Type type,
                                                          MDefinition* value,
                                                          PropertyName* name);
    AbortReasonOr<Ok> setPropTryScalarPropOfTypedObject(bool* emitted,
                                                        MDefinition* obj,
                                                        int32_t fieldOffset,
                                                        MDefinition* value,
                                                        TypedObjectPrediction fieldTypeReprs);
    AbortReasonOr<Ok> setPropTryScalarTypedObjectValue(bool* emitted,
                                                       MDefinition* typedObj,
                                                       const LinearSum& byteOffset,
                                                       ScalarTypeDescr::Type type,
                                                       MDefinition* value);
    AbortReasonOr<Ok> setPropTryCache(bool* emitted, MDefinition* obj,
                                      PropertyName* name, MDefinition* value,
                                      bool barrier);

    // jsop_binary_arith helpers.
    MBinaryArithInstruction* binaryArithInstruction(JSOp op, MDefinition* left, MDefinition* right);
    AbortReasonOr<Ok> binaryArithTryConcat(bool* emitted, JSOp op, MDefinition* left,
                                           MDefinition* right);
    AbortReasonOr<Ok> binaryArithTrySpecialized(bool* emitted, JSOp op, MDefinition* left,
                                                MDefinition* right);
    AbortReasonOr<Ok> binaryArithTrySpecializedOnBaselineInspector(bool* emitted, JSOp op,
                                                                   MDefinition* left,
                                                                   MDefinition* right);
    AbortReasonOr<Ok> arithTrySharedStub(bool* emitted, JSOp op, MDefinition* left,
                                         MDefinition* right);

    // jsop_bitnot helpers.
    AbortReasonOr<Ok> bitnotTrySpecialized(bool* emitted, MDefinition* input);

    // jsop_pow helpers.
    AbortReasonOr<Ok> powTrySpecialized(bool* emitted, MDefinition* base, MDefinition* power,
                                        MIRType outputType);

    // jsop_compare helpers.
    AbortReasonOr<Ok> compareTrySpecialized(bool* emitted, JSOp op, MDefinition* left,
                                            MDefinition* right, bool canTrackOptimization);
    AbortReasonOr<Ok> compareTryBitwise(bool* emitted, JSOp op, MDefinition* left,
                                        MDefinition* right);
    AbortReasonOr<Ok> compareTrySpecializedOnBaselineInspector(bool* emitted, JSOp op,
                                                               MDefinition* left,
                                                               MDefinition* right);
    AbortReasonOr<Ok> compareTrySharedStub(bool* emitted, MDefinition* left, MDefinition* right);

    // jsop_newarray helpers.
    AbortReasonOr<Ok> newArrayTrySharedStub(bool* emitted);
    AbortReasonOr<Ok> newArrayTryTemplateObject(bool* emitted, JSObject* templateObject,
                                                uint32_t length);
    AbortReasonOr<Ok> newArrayTryVM(bool* emitted, JSObject* templateObject, uint32_t length);

    // jsop_newobject helpers.
    AbortReasonOr<Ok> newObjectTrySharedStub(bool* emitted);
    AbortReasonOr<Ok> newObjectTryTemplateObject(bool* emitted, JSObject* templateObject);
    AbortReasonOr<Ok> newObjectTryVM(bool* emitted, JSObject* templateObject);

    // jsop_in/jsop_hasown helpers.
    AbortReasonOr<Ok> inTryDense(bool* emitted, MDefinition* obj, MDefinition* id);
    AbortReasonOr<Ok> hasTryNotDefined(bool* emitted, MDefinition* obj, MDefinition* id, bool ownProperty);
    AbortReasonOr<Ok> hasTryDefiniteSlotOrUnboxed(bool* emitted, MDefinition* obj, MDefinition* id);

    // binary data lookup helpers.
    TypedObjectPrediction typedObjectPrediction(MDefinition* typedObj);
    TypedObjectPrediction typedObjectPrediction(TemporaryTypeSet* types);
    bool typedObjectHasField(MDefinition* typedObj,
                             PropertyName* name,
                             size_t* fieldOffset,
                             TypedObjectPrediction* fieldTypeReprs,
                             size_t* fieldIndex);
    MDefinition* loadTypedObjectType(MDefinition* value);
    AbortReasonOr<Ok> loadTypedObjectData(MDefinition* typedObj,
                                          MDefinition** owner,
                                          LinearSum* ownerOffset);
    AbortReasonOr<Ok> loadTypedObjectElements(MDefinition* typedObj,
                                              const LinearSum& byteOffset,
                                              uint32_t scale,
                                              MDefinition** ownerElements,
                                              MDefinition** ownerScaledOffset,
                                              int32_t* ownerByteAdjustment);
    MDefinition* typeObjectForElementFromArrayStructType(MDefinition* typedObj);
    MDefinition* typeObjectForFieldFromStructType(MDefinition* type,
                                                  size_t fieldIndex);
    bool checkTypedObjectIndexInBounds(uint32_t elemSize,
                                       MDefinition* index,
                                       TypedObjectPrediction objTypeDescrs,
                                       LinearSum* indexAsByteOffset);
    AbortReasonOr<Ok> pushDerivedTypedObject(bool* emitted,
                                             MDefinition* obj,
                                             const LinearSum& byteOffset,
                                             TypedObjectPrediction derivedTypeDescrs,
                                             MDefinition* derivedTypeObj);
    AbortReasonOr<Ok> pushScalarLoadFromTypedObject(MDefinition* obj,
                                                    const LinearSum& byteoffset,
                                                    ScalarTypeDescr::Type type);
    AbortReasonOr<Ok> pushReferenceLoadFromTypedObject(MDefinition* typedObj,
                                                       const LinearSum& byteOffset,
                                                       ReferenceTypeDescr::Type type,
                                                       PropertyName* name);

    // jsop_setelem() helpers.
    AbortReasonOr<Ok> setElemTryTypedArray(bool* emitted, MDefinition* object,
                                           MDefinition* index, MDefinition* value);
    AbortReasonOr<Ok> setElemTryTypedObject(bool* emitted, MDefinition* obj,
                                            MDefinition* index, MDefinition* value);
    AbortReasonOr<Ok> initOrSetElemTryDense(bool* emitted, MDefinition* object,
                                            MDefinition* index, MDefinition* value,
                                            bool writeHole);
    AbortReasonOr<Ok> setElemTryArguments(bool* emitted, MDefinition* object);
    AbortReasonOr<Ok> initOrSetElemTryCache(bool* emitted, MDefinition* object,
                                            MDefinition* index, MDefinition* value);
    AbortReasonOr<Ok> setElemTryReferenceElemOfTypedObject(bool* emitted,
                                                           MDefinition* obj,
                                                           MDefinition* index,
                                                           TypedObjectPrediction objPrediction,
                                                           MDefinition* value,
                                                           TypedObjectPrediction elemPrediction);
    AbortReasonOr<Ok> setElemTryScalarElemOfTypedObject(bool* emitted,
                                                        MDefinition* obj,
                                                        MDefinition* index,
                                                        TypedObjectPrediction objTypeReprs,
                                                        MDefinition* value,
                                                        TypedObjectPrediction elemTypeReprs,
                                                        uint32_t elemSize);
    AbortReasonOr<Ok> initializeArrayElement(MDefinition* obj, size_t index, MDefinition* value,
                                             bool addResumePointAndIncrementInitializedLength);

    // jsop_getelem() helpers.
    AbortReasonOr<Ok> getElemTryDense(bool* emitted, MDefinition* obj, MDefinition* index);
    AbortReasonOr<Ok> getElemTryGetProp(bool* emitted, MDefinition* obj, MDefinition* index);
    AbortReasonOr<Ok> getElemTryTypedArray(bool* emitted, MDefinition* obj, MDefinition* index);
    AbortReasonOr<Ok> getElemTryTypedObject(bool* emitted, MDefinition* obj, MDefinition* index);
    AbortReasonOr<Ok> getElemTryString(bool* emitted, MDefinition* obj, MDefinition* index);
    AbortReasonOr<Ok> getElemTryArguments(bool* emitted, MDefinition* obj, MDefinition* index);
    AbortReasonOr<Ok> getElemTryArgumentsInlinedConstant(bool* emitted, MDefinition* obj,
                                                         MDefinition* index);
    AbortReasonOr<Ok> getElemTryArgumentsInlinedIndex(bool* emitted, MDefinition* obj,
                                                      MDefinition* index);
    AbortReasonOr<Ok> getElemAddCache(MDefinition* obj, MDefinition* index);
    AbortReasonOr<Ok> getElemTryScalarElemOfTypedObject(bool* emitted,
                                                        MDefinition* obj,
                                                        MDefinition* index,
                                                        TypedObjectPrediction objTypeReprs,
                                                        TypedObjectPrediction elemTypeReprs,
                                                        uint32_t elemSize);
    AbortReasonOr<Ok> getElemTryReferenceElemOfTypedObject(bool* emitted,
                                                           MDefinition* obj,
                                                           MDefinition* index,
                                                           TypedObjectPrediction objPrediction,
                                                           TypedObjectPrediction elemPrediction);
    AbortReasonOr<Ok> getElemTryComplexElemOfTypedObject(bool* emitted,
                                                         MDefinition* obj,
                                                         MDefinition* index,
                                                         TypedObjectPrediction objTypeReprs,
                                                         TypedObjectPrediction elemTypeReprs,
                                                         uint32_t elemSize);
    TemporaryTypeSet* computeHeapType(const TemporaryTypeSet* objTypes, const jsid id);

    enum BoundsChecking { DoBoundsCheck, SkipBoundsCheck };

    MInstruction* addArrayBufferByteLength(MDefinition* obj);

    // Add instructions to compute a typed array's length and data.  Also
    // optionally convert |*index| into a bounds-checked definition, if
    // requested.
    //
    // If you only need the array's length, use addTypedArrayLength below.
    void addTypedArrayLengthAndData(MDefinition* obj,
                                    BoundsChecking checking,
                                    MDefinition** index,
                                    MInstruction** length, MInstruction** elements);

    // Add an instruction to compute a typed array's length to the current
    // block.  If you also need the typed array's data, use the above method
    // instead.
    MInstruction* addTypedArrayLength(MDefinition* obj) {
        MInstruction* length;
        addTypedArrayLengthAndData(obj, SkipBoundsCheck, nullptr, &length, nullptr);
        return length;
    }

    AbortReasonOr<Ok> improveThisTypesForCall();

    MDefinition* getCallee();
    MDefinition* getAliasedVar(EnvironmentCoordinate ec);
    AbortReasonOr<MDefinition*> addLexicalCheck(MDefinition* input);

    MDefinition* convertToBoolean(MDefinition* input);

    AbortReasonOr<Ok> tryFoldInstanceOf(bool* emitted, MDefinition* lhs, JSObject* protoObject);
    AbortReasonOr<bool> hasOnProtoChain(TypeSet::ObjectKey* key, JSObject* protoObject, bool* onProto);

    AbortReasonOr<Ok> jsop_add(MDefinition* left, MDefinition* right);
    AbortReasonOr<Ok> jsop_bitnot();
    AbortReasonOr<Ok> jsop_bitop(JSOp op);
    AbortReasonOr<Ok> jsop_binary_arith(JSOp op);
    AbortReasonOr<Ok> jsop_binary_arith(JSOp op, MDefinition* left, MDefinition* right);
    AbortReasonOr<Ok> jsop_pow();
    AbortReasonOr<Ok> jsop_pos();
    AbortReasonOr<Ok> jsop_neg();
    AbortReasonOr<Ok> jsop_tostring();
    AbortReasonOr<Ok> jsop_setarg(uint32_t arg);
    AbortReasonOr<Ok> jsop_defvar(uint32_t index);
    AbortReasonOr<Ok> jsop_deflexical(uint32_t index);
    AbortReasonOr<Ok> jsop_deffun();
    AbortReasonOr<Ok> jsop_notearg();
    AbortReasonOr<Ok> jsop_throwsetconst();
    AbortReasonOr<Ok> jsop_checklexical();
    AbortReasonOr<Ok> jsop_checkaliasedlexical(EnvironmentCoordinate ec);
    AbortReasonOr<Ok> jsop_funcall(uint32_t argc);
    AbortReasonOr<Ok> jsop_funapply(uint32_t argc);
    AbortReasonOr<Ok> jsop_funapplyarguments(uint32_t argc);
    AbortReasonOr<Ok> jsop_funapplyarray(uint32_t argc);
    AbortReasonOr<Ok> jsop_spreadcall();
    AbortReasonOr<Ok> jsop_call(uint32_t argc, bool constructing, bool ignoresReturnValue);
    AbortReasonOr<Ok> jsop_eval(uint32_t argc);
    AbortReasonOr<Ok> jsop_label();
    AbortReasonOr<Ok> jsop_andor(JSOp op);
    AbortReasonOr<Ok> jsop_dup2();
    AbortReasonOr<Ok> jsop_loophead(jsbytecode* pc);
    AbortReasonOr<Ok> jsop_compare(JSOp op);
    AbortReasonOr<Ok> jsop_compare(JSOp op, MDefinition* left, MDefinition* right);
    AbortReasonOr<Ok> getStaticName(bool* emitted, JSObject* staticObject, PropertyName* name,
                                    MDefinition* lexicalCheck = nullptr);
    AbortReasonOr<Ok> loadStaticSlot(JSObject* staticObject, BarrierKind barrier,
                                     TemporaryTypeSet* types, uint32_t slot);
    AbortReasonOr<Ok> setStaticName(JSObject* staticObject, PropertyName* name);
    AbortReasonOr<Ok> jsop_getgname(PropertyName* name);
    AbortReasonOr<Ok> jsop_getname(PropertyName* name);
    AbortReasonOr<Ok> jsop_intrinsic(PropertyName* name);
    AbortReasonOr<Ok> jsop_getimport(PropertyName* name);
    AbortReasonOr<Ok> jsop_bindname(PropertyName* name);
    AbortReasonOr<Ok> jsop_bindvar();
    AbortReasonOr<Ok> jsop_getelem();
    AbortReasonOr<Ok> jsop_getelem_dense(MDefinition* obj, MDefinition* index);
    AbortReasonOr<Ok> jsop_getelem_typed(MDefinition* obj, MDefinition* index,
                                         ScalarTypeDescr::Type arrayType);
    AbortReasonOr<Ok> jsop_setelem();
    AbortReasonOr<Ok> initOrSetElemDense(TemporaryTypeSet::DoubleConversion conversion,
                                         MDefinition* object, MDefinition* index,
                                         MDefinition* value, bool writeHole, bool* emitted);
    AbortReasonOr<Ok> jsop_setelem_typed(ScalarTypeDescr::Type arrayType,
                                         MDefinition* object, MDefinition* index,
                                         MDefinition* value);
    AbortReasonOr<Ok> jsop_length();
    bool jsop_length_fastPath();
    AbortReasonOr<Ok> jsop_arguments();
    AbortReasonOr<Ok> jsop_arguments_getelem();
    AbortReasonOr<Ok> jsop_runonce();
    AbortReasonOr<Ok> jsop_rest();
    AbortReasonOr<Ok> jsop_not();
    AbortReasonOr<Ok> jsop_superbase();
    AbortReasonOr<Ok> jsop_getprop_super(PropertyName* name);
    AbortReasonOr<Ok> jsop_getelem_super();
    AbortReasonOr<Ok> jsop_getprop(PropertyName* name);
    AbortReasonOr<Ok> jsop_setprop(PropertyName* name);
    AbortReasonOr<Ok> jsop_delprop(PropertyName* name);
    AbortReasonOr<Ok> jsop_delelem();
    AbortReasonOr<Ok> jsop_newarray(uint32_t length);
    AbortReasonOr<Ok> jsop_newarray(JSObject* templateObject, uint32_t length);
    AbortReasonOr<Ok> jsop_newarray_copyonwrite();
    AbortReasonOr<Ok> jsop_newobject();
    AbortReasonOr<Ok> jsop_initelem();
    AbortReasonOr<Ok> jsop_initelem_inc();
    AbortReasonOr<Ok> jsop_initelem_array();
    AbortReasonOr<Ok> jsop_initelem_getter_setter();
    AbortReasonOr<Ok> jsop_mutateproto();
    AbortReasonOr<Ok> jsop_initprop(PropertyName* name);
    AbortReasonOr<Ok> jsop_initprop_getter_setter(PropertyName* name);
    AbortReasonOr<Ok> jsop_regexp(RegExpObject* reobj);
    AbortReasonOr<Ok> jsop_object(JSObject* obj);
    AbortReasonOr<Ok> jsop_classconstructor();
    AbortReasonOr<Ok> jsop_lambda(JSFunction* fun);
    AbortReasonOr<Ok> jsop_lambda_arrow(JSFunction* fun);
    AbortReasonOr<Ok> jsop_setfunname(uint8_t prefixKind);
    AbortReasonOr<Ok> jsop_pushlexicalenv(uint32_t index);
    AbortReasonOr<Ok> jsop_copylexicalenv(bool copySlots);
    AbortReasonOr<Ok> jsop_functionthis();
    AbortReasonOr<Ok> jsop_globalthis();
    AbortReasonOr<Ok> jsop_typeof();
    AbortReasonOr<Ok> jsop_toasync();
    AbortReasonOr<Ok> jsop_toasyncgen();
    AbortReasonOr<Ok> jsop_toasynciter();
    AbortReasonOr<Ok> jsop_toid();
    AbortReasonOr<Ok> jsop_iter();
    AbortReasonOr<Ok> jsop_itermore();
    AbortReasonOr<Ok> jsop_isnoiter();
    AbortReasonOr<Ok> jsop_iterend();
    AbortReasonOr<Ok> jsop_iternext();
    AbortReasonOr<Ok> jsop_in();
    AbortReasonOr<Ok> jsop_hasown();
    AbortReasonOr<Ok> jsop_instanceof();
    AbortReasonOr<Ok> jsop_getaliasedvar(EnvironmentCoordinate ec);
    AbortReasonOr<Ok> jsop_setaliasedvar(EnvironmentCoordinate ec);
    AbortReasonOr<Ok> jsop_debugger();
    AbortReasonOr<Ok> jsop_newtarget();
    AbortReasonOr<Ok> jsop_checkisobj(uint8_t kind);
    AbortReasonOr<Ok> jsop_checkiscallable(uint8_t kind);
    AbortReasonOr<Ok> jsop_checkobjcoercible();
    AbortReasonOr<Ok> jsop_pushcallobj();
    AbortReasonOr<Ok> jsop_implicitthis(PropertyName* name);

    /* Inlining. */

    enum InliningStatus
    {
        InliningStatus_NotInlined,
        InliningStatus_WarmUpCountTooLow,
        InliningStatus_Inlined
    };
    using InliningResult = AbortReasonOr<InliningStatus>;

    enum InliningDecision
    {
        InliningDecision_Error,
        InliningDecision_Inline,
        InliningDecision_DontInline,
        InliningDecision_WarmUpCountTooLow
    };

    static InliningDecision DontInline(JSScript* targetScript, const char* reason);

    // Helper function for canInlineTarget
    bool hasCommonInliningPath(const JSScript* scriptToInline);

    // Oracles.
    InliningDecision canInlineTarget(JSFunction* target, CallInfo& callInfo);
    InliningDecision makeInliningDecision(JSObject* target, CallInfo& callInfo);
    AbortReasonOr<Ok> selectInliningTargets(const InliningTargets& targets, CallInfo& callInfo,
                                            BoolVector& choiceSet, uint32_t* numInlineable);

    // Native inlining helpers.
    // The typeset for the return value of our function.  These are
    // the types it's been observed returning in the past.
    TemporaryTypeSet* getInlineReturnTypeSet();
    // The known MIR type of getInlineReturnTypeSet.
    MIRType getInlineReturnType();

    // Array natives.
    InliningResult inlineArray(CallInfo& callInfo);
    InliningResult inlineArrayIsArray(CallInfo& callInfo);
    InliningResult inlineArrayPopShift(CallInfo& callInfo, MArrayPopShift::Mode mode);
    InliningResult inlineArrayPush(CallInfo& callInfo);
    InliningResult inlineArraySlice(CallInfo& callInfo);
    InliningResult inlineArrayJoin(CallInfo& callInfo);

    // Boolean natives.
    InliningResult inlineBoolean(CallInfo& callInfo);

    // Iterator intrinsics.
    InliningResult inlineNewIterator(CallInfo& callInfo, MNewIterator::Type type);

    // Math natives.
    InliningResult inlineMathAbs(CallInfo& callInfo);
    InliningResult inlineMathFloor(CallInfo& callInfo);
    InliningResult inlineMathCeil(CallInfo& callInfo);
    InliningResult inlineMathClz32(CallInfo& callInfo);
    InliningResult inlineMathRound(CallInfo& callInfo);
    InliningResult inlineMathSqrt(CallInfo& callInfo);
    InliningResult inlineMathAtan2(CallInfo& callInfo);
    InliningResult inlineMathHypot(CallInfo& callInfo);
    InliningResult inlineMathMinMax(CallInfo& callInfo, bool max);
    InliningResult inlineMathPow(CallInfo& callInfo);
    InliningResult inlineMathRandom(CallInfo& callInfo);
    InliningResult inlineMathImul(CallInfo& callInfo);
    InliningResult inlineMathFRound(CallInfo& callInfo);
    InliningResult inlineMathFunction(CallInfo& callInfo, MMathFunction::Function function);

    // String natives.
    InliningResult inlineStringObject(CallInfo& callInfo);
    InliningResult inlineStrCharCodeAt(CallInfo& callInfo);
    InliningResult inlineConstantCharCodeAt(CallInfo& callInfo);
    InliningResult inlineStrFromCharCode(CallInfo& callInfo);
    InliningResult inlineStrFromCodePoint(CallInfo& callInfo);
    InliningResult inlineStrCharAt(CallInfo& callInfo);
    InliningResult inlineStringConvertCase(CallInfo& callInfo, MStringConvertCase::Mode mode);

    // String intrinsics.
    InliningResult inlineStringReplaceString(CallInfo& callInfo);
    InliningResult inlineConstantStringSplitString(CallInfo& callInfo);
    InliningResult inlineStringSplitString(CallInfo& callInfo);

    // Reflect natives.
    InliningResult inlineReflectGetPrototypeOf(CallInfo& callInfo);

    // RegExp intrinsics.
    InliningResult inlineRegExpMatcher(CallInfo& callInfo);
    InliningResult inlineRegExpSearcher(CallInfo& callInfo);
    InliningResult inlineRegExpTester(CallInfo& callInfo);
    InliningResult inlineIsRegExpObject(CallInfo& callInfo);
    InliningResult inlineRegExpPrototypeOptimizable(CallInfo& callInfo);
    InliningResult inlineRegExpInstanceOptimizable(CallInfo& callInfo);
    InliningResult inlineGetFirstDollarIndex(CallInfo& callInfo);

    // Object natives and intrinsics.
    InliningResult inlineObject(CallInfo& callInfo);
    InliningResult inlineObjectCreate(CallInfo& callInfo);
    InliningResult inlineObjectIs(CallInfo& callInfo);
    InliningResult inlineObjectToString(CallInfo& callInfo);
    InliningResult inlineDefineDataProperty(CallInfo& callInfo);

    // Atomics natives.
    InliningResult inlineAtomicsCompareExchange(CallInfo& callInfo);
    InliningResult inlineAtomicsExchange(CallInfo& callInfo);
    InliningResult inlineAtomicsLoad(CallInfo& callInfo);
    InliningResult inlineAtomicsStore(CallInfo& callInfo);
    InliningResult inlineAtomicsBinop(CallInfo& callInfo, InlinableNative target);
    InliningResult inlineAtomicsIsLockFree(CallInfo& callInfo);

    // Slot intrinsics.
    InliningResult inlineUnsafeSetReservedSlot(CallInfo& callInfo);
    InliningResult inlineUnsafeGetReservedSlot(CallInfo& callInfo,
                                               MIRType knownValueType);

    // Map and Set intrinsics.
    InliningResult inlineGetNextEntryForIterator(CallInfo& callInfo,
                                                 MGetNextEntryForIterator::Mode mode);

    // ArrayBuffer intrinsics.
    InliningResult inlineArrayBufferByteLength(CallInfo& callInfo);
    InliningResult inlinePossiblyWrappedArrayBufferByteLength(CallInfo& callInfo);

    // TypedArray intrinsics.
    enum WrappingBehavior { AllowWrappedTypedArrays, RejectWrappedTypedArrays };
    InliningResult inlineTypedArray(CallInfo& callInfo, Native native);
    InliningResult inlineIsTypedArrayHelper(CallInfo& callInfo, WrappingBehavior wrappingBehavior);
    InliningResult inlineIsTypedArray(CallInfo& callInfo);
    InliningResult inlineIsPossiblyWrappedTypedArray(CallInfo& callInfo);
    InliningResult inlineTypedArrayLength(CallInfo& callInfo);
    InliningResult inlinePossiblyWrappedTypedArrayLength(CallInfo& callInfo);
    InliningResult inlineSetDisjointTypedElements(CallInfo& callInfo);

    // TypedObject intrinsics and natives.
    InliningResult inlineObjectIsTypeDescr(CallInfo& callInfo);
    InliningResult inlineSetTypedObjectOffset(CallInfo& callInfo);
    InliningResult inlineConstructTypedObject(CallInfo& callInfo, TypeDescr* target);

    // SIMD intrinsics and natives.
    InliningResult inlineConstructSimdObject(CallInfo& callInfo, SimdTypeDescr* target);

    // SIMD helpers.
    bool canInlineSimd(CallInfo& callInfo, JSNative native, unsigned numArgs,
                       InlineTypedObject** templateObj);
    MDefinition* unboxSimd(MDefinition* ins, SimdType type);
    InliningResult boxSimd(CallInfo& callInfo, MDefinition* ins, InlineTypedObject* templateObj);
    MDefinition* convertToBooleanSimdLane(MDefinition* scalar);

    InliningResult inlineSimd(CallInfo& callInfo, JSFunction* target, SimdType type);

    InliningResult inlineSimdBinaryArith(CallInfo& callInfo, JSNative native,
                                         MSimdBinaryArith::Operation op, SimdType type);
    InliningResult inlineSimdBinaryBitwise(CallInfo& callInfo, JSNative native,
                                           MSimdBinaryBitwise::Operation op, SimdType type);
    InliningResult inlineSimdBinarySaturating(CallInfo& callInfo, JSNative native,
                                              MSimdBinarySaturating::Operation op, SimdType type);
    InliningResult inlineSimdShift(CallInfo& callInfo, JSNative native, MSimdShift::Operation op,
                                   SimdType type);
    InliningResult inlineSimdComp(CallInfo& callInfo, JSNative native,
                                  MSimdBinaryComp::Operation op, SimdType type);
    InliningResult inlineSimdUnary(CallInfo& callInfo, JSNative native,
                                   MSimdUnaryArith::Operation op, SimdType type);
    InliningResult inlineSimdExtractLane(CallInfo& callInfo, JSNative native, SimdType type);
    InliningResult inlineSimdReplaceLane(CallInfo& callInfo, JSNative native, SimdType type);
    InliningResult inlineSimdSplat(CallInfo& callInfo, JSNative native, SimdType type);
    InliningResult inlineSimdShuffle(CallInfo& callInfo, JSNative native, SimdType type,
                                     unsigned numVectors);
    InliningResult inlineSimdCheck(CallInfo& callInfo, JSNative native, SimdType type);
    InliningResult inlineSimdConvert(CallInfo& callInfo, JSNative native, bool isCast,
                                     SimdType from, SimdType to);
    InliningResult inlineSimdSelect(CallInfo& callInfo, JSNative native, SimdType type);

    bool prepareForSimdLoadStore(CallInfo& callInfo, Scalar::Type simdType,
                                 MInstruction** elements, MDefinition** index,
                                 Scalar::Type* arrayType);
    InliningResult inlineSimdLoad(CallInfo& callInfo, JSNative native, SimdType type,
                                  unsigned numElems);
    InliningResult inlineSimdStore(CallInfo& callInfo, JSNative native, SimdType type,
                                   unsigned numElems);

    InliningResult inlineSimdAnyAllTrue(CallInfo& callInfo, bool IsAllTrue, JSNative native,
                                        SimdType type);

    // Utility intrinsics.
    InliningResult inlineIsCallable(CallInfo& callInfo);
    InliningResult inlineIsConstructor(CallInfo& callInfo);
    InliningResult inlineIsObject(CallInfo& callInfo);
    InliningResult inlineToObject(CallInfo& callInfo);
    InliningResult inlineIsWrappedArrayConstructor(CallInfo& callInfo);
    InliningResult inlineToInteger(CallInfo& callInfo);
    InliningResult inlineToString(CallInfo& callInfo);
    InliningResult inlineDump(CallInfo& callInfo);
    InliningResult inlineHasClass(CallInfo& callInfo, const Class* clasp,
                                  const Class* clasp2 = nullptr,
                                  const Class* clasp3 = nullptr,
                                  const Class* clasp4 = nullptr);
    InliningResult inlineGuardToClass(CallInfo& callInfo, const Class* clasp);
    InliningResult inlineIsConstructing(CallInfo& callInfo);
    InliningResult inlineSubstringKernel(CallInfo& callInfo);
    InliningResult inlineObjectHasPrototype(CallInfo& callInfo);
    InliningResult inlineFinishBoundFunctionInit(CallInfo& callInfo);
    InliningResult inlineIsPackedArray(CallInfo& callInfo);

    // Testing functions.
    InliningResult inlineBailout(CallInfo& callInfo);
    InliningResult inlineAssertFloat32(CallInfo& callInfo);
    InliningResult inlineAssertRecoveredOnBailout(CallInfo& callInfo);

    // Bind function.
    InliningResult inlineBoundFunction(CallInfo& callInfo, JSFunction* target);

    // Main inlining functions
    InliningResult inlineNativeCall(CallInfo& callInfo, JSFunction* target);
    InliningResult inlineNativeGetter(CallInfo& callInfo, JSFunction* target);
    InliningResult inlineNonFunctionCall(CallInfo& callInfo, JSObject* target);
    InliningResult inlineScriptedCall(CallInfo& callInfo, JSFunction* target);
    InliningResult inlineSingleCall(CallInfo& callInfo, JSObject* target);

    // Call functions
    InliningResult inlineCallsite(const InliningTargets& targets, CallInfo& callInfo);
    AbortReasonOr<Ok> inlineCalls(CallInfo& callInfo, const InliningTargets& targets,
                                  BoolVector& choiceSet, MGetPropertyCache* maybeCache);

    // Inlining helpers.
    AbortReasonOr<Ok> inlineGenericFallback(const Maybe<CallTargets>& targets,
                                            CallInfo& callInfo,
                                            MBasicBlock* dispatchBlock);
    AbortReasonOr<Ok> inlineObjectGroupFallback(const Maybe<CallTargets>& targets,
                                                CallInfo& callInfo, MBasicBlock* dispatchBlock,
                                                MObjectGroupDispatch* dispatch,
                                                MGetPropertyCache* cache,
                                                MBasicBlock** fallbackTarget);

    enum AtomicCheckResult {
        DontCheckAtomicResult,
        DoCheckAtomicResult
    };

    bool atomicsMeetsPreconditions(CallInfo& callInfo, Scalar::Type* arrayElementType,
                                   bool* requiresDynamicCheck,
                                   AtomicCheckResult checkResult=DoCheckAtomicResult);
    void atomicsCheckBounds(CallInfo& callInfo, MInstruction** elements, MDefinition** index);

    bool testNeedsArgumentCheck(JSFunction* target, CallInfo& callInfo);

    AbortReasonOr<MCall*> makeCallHelper(const Maybe<CallTargets>& targets, CallInfo& callInfo);
    AbortReasonOr<Ok> makeCall(const Maybe<CallTargets>& targets, CallInfo& callInfo);
    AbortReasonOr<Ok> makeCall(JSFunction* target, CallInfo& callInfo);

    MDefinition* patchInlinedReturn(CallInfo& callInfo, MBasicBlock* exit, MBasicBlock* bottom);
    MDefinition* patchInlinedReturns(CallInfo& callInfo, MIRGraphReturns& returns,
                                     MBasicBlock* bottom);
    MDefinition* specializeInlinedReturn(MDefinition* rdef, MBasicBlock* exit);

    NativeObject* commonPrototypeWithGetterSetter(TemporaryTypeSet* types, PropertyName* name,
                                                  bool isGetter, JSFunction* getterOrSetter,
                                                  bool* guardGlobal);
    void freezePropertiesForCommonPrototype(TemporaryTypeSet* types, PropertyName* name,
                                            JSObject* foundProto, bool allowEmptyTypesForGlobal = false);
    /*
     * Callers must pass a non-null globalGuard if they pass a non-null globalShape.
     */
    bool testCommonGetterSetter(TemporaryTypeSet* types, PropertyName* name,
                                bool isGetter, JSFunction* getterOrSetter,
                                MDefinition** guard, Shape* globalShape = nullptr,
                                MDefinition** globalGuard = nullptr);
    AbortReasonOr<bool> testShouldDOMCall(TypeSet* inTypes,
                                          JSFunction* func, JSJitInfo::OpType opType);

    MDefinition*
    addShapeGuardsForGetterSetter(MDefinition* obj, JSObject* holder, Shape* holderShape,
                                  const BaselineInspector::ReceiverVector& receivers,
                                  const BaselineInspector::ObjectGroupVector& convertUnboxedGroups,
                                  bool isOwnProperty);

    AbortReasonOr<Ok> annotateGetPropertyCache(MDefinition* obj, PropertyName* name,
                                               MGetPropertyCache* getPropCache,
                                               TemporaryTypeSet* objTypes,
                                               TemporaryTypeSet* pushedTypes);

    MGetPropertyCache* getInlineableGetPropertyCache(CallInfo& callInfo);

    JSObject* testGlobalLexicalBinding(PropertyName* name);

    JSObject* testSingletonProperty(JSObject* obj, jsid id);
    JSObject* testSingletonPropertyTypes(MDefinition* obj, jsid id);

    AbortReasonOr<bool> testNotDefinedProperty(MDefinition* obj, jsid id, bool ownProperty = false);

    uint32_t getDefiniteSlot(TemporaryTypeSet* types, jsid id, uint32_t* pnfixed);
    MDefinition* convertUnboxedObjects(MDefinition* obj);
    MDefinition* convertUnboxedObjects(MDefinition* obj,
                                       const BaselineInspector::ObjectGroupVector& list);
    uint32_t getUnboxedOffset(TemporaryTypeSet* types, jsid id,
                              JSValueType* punboxedType);
    MInstruction* loadUnboxedProperty(MDefinition* obj, size_t offset, JSValueType unboxedType,
                                      BarrierKind barrier, TemporaryTypeSet* types);
    MInstruction* loadUnboxedValue(MDefinition* elements, size_t elementsOffset,
                                   MDefinition* scaledOffset, JSValueType unboxedType,
                                   BarrierKind barrier, TemporaryTypeSet* types);
    MInstruction* storeUnboxedProperty(MDefinition* obj, size_t offset, JSValueType unboxedType,
                                       MDefinition* value);
    MInstruction* storeUnboxedValue(MDefinition* obj,
                                    MDefinition* elements, int32_t elementsOffset,
                                    MDefinition* scaledOffset, JSValueType unboxedType,
                                    MDefinition* value, bool preBarrier = true);
    AbortReasonOr<Ok> checkPreliminaryGroups(MDefinition *obj);
    AbortReasonOr<Ok> freezePropTypeSets(TemporaryTypeSet* types,
                                         JSObject* foundProto, PropertyName* name);
    bool canInlinePropertyOpShapes(const BaselineInspector::ReceiverVector& receivers);

    TemporaryTypeSet* bytecodeTypes(jsbytecode* pc);

    // Use one of the below methods for updating the current block, rather than
    // updating |current| directly. setCurrent() should only be used in cases
    // where the block cannot have phis whose type needs to be computed.

    AbortReasonOr<Ok> setCurrentAndSpecializePhis(MBasicBlock* block) {
        if (block) {
            if (!block->specializePhis(alloc()))
                return abort(AbortReason::Alloc);
        }
        setCurrent(block);
        return Ok();
    }

    void setCurrent(MBasicBlock* block) {
        current = block;
    }

    // A builder is inextricably tied to a particular script.
    JSScript* script_;

    // script->hasIonScript() at the start of the compilation. Used to avoid
    // calling hasIonScript() from background compilation threads.
    bool scriptHasIonScript_;

    // If off thread compilation is successful, the final code generator is
    // attached here. Code has been generated, but not linked (there is not yet
    // an IonScript). This is heap allocated, and must be explicitly destroyed,
    // performed by FinishOffThreadBuilder().
    CodeGenerator* backgroundCodegen_;

    // Some aborts are actionable (e.g., using an unsupported bytecode). When
    // optimization tracking is enabled, the location and message of the abort
    // are recorded here so they may be propagated to the script's
    // corresponding JitcodeGlobalEntry::BaselineEntry.
    JSScript* actionableAbortScript_;
    jsbytecode* actionableAbortPc_;
    const char* actionableAbortMessage_;

    MRootList* rootList_;

  public:
    void setRootList(MRootList& rootList) {
        MOZ_ASSERT(!rootList_);
        rootList_ = &rootList;
    }
    void clearForBackEnd();
    JSObject* checkNurseryObject(JSObject* obj);

    JSScript* script() const { return script_; }
    bool scriptHasIonScript() const { return scriptHasIonScript_; }

    CodeGenerator* backgroundCodegen() const { return backgroundCodegen_; }
    void setBackgroundCodegen(CodeGenerator* codegen) { backgroundCodegen_ = codegen; }

    CompilerConstraintList* constraints() {
        return constraints_;
    }

    bool isInlineBuilder() const {
        return callerBuilder_ != nullptr;
    }

    const JSAtomState& names() { return compartment->runtime()->names(); }

    bool hadActionableAbort() const {
        MOZ_ASSERT(!actionableAbortScript_ ||
                   (actionableAbortPc_ && actionableAbortMessage_));
        return actionableAbortScript_ != nullptr;
    }

    TraceLoggerThread *traceLogger() {
        return TraceLoggerForCurrentThread();
    }

    void actionableAbortLocationAndMessage(JSScript** abortScript, jsbytecode** abortPc,
                                           const char** abortMessage)
    {
        MOZ_ASSERT(hadActionableAbort());
        *abortScript = actionableAbortScript_;
        *abortPc = actionableAbortPc_;
        *abortMessage = actionableAbortMessage_;
    }

    void trace(JSTracer* trc);

  private:
    AbortReasonOr<Ok> init();

    JSContext* analysisContext;
    BaselineFrameInspector* baselineFrame_;

    // Constraints for recording dependencies on type information.
    CompilerConstraintList* constraints_;

    TemporaryTypeSet* thisTypes;
    TemporaryTypeSet* argTypes;
    TemporaryTypeSet* typeArray;
    uint32_t typeArrayHint;
    uint32_t* bytecodeTypeMap;

    EnvironmentCoordinateNameCache envCoordinateNameCache;

    jsbytecode* pc;
    MBasicBlock* current;
    uint32_t loopDepth_;
    Vector<MBasicBlock*, 0, JitAllocPolicy> blockWorklist;
    const CFGBlock* cfgCurrent;
    const ControlFlowGraph* cfg;

    Vector<BytecodeSite*, 0, JitAllocPolicy> trackedOptimizationSites_;

    BytecodeSite* bytecodeSite(jsbytecode* pc) {
        MOZ_ASSERT(info().inlineScriptTree()->script()->containsPC(pc));
        // See comment in maybeTrackedOptimizationSite.
        if (isOptimizationTrackingEnabled()) {
            if (BytecodeSite* site = maybeTrackedOptimizationSite(pc))
                return site;
        }
        return new(alloc()) BytecodeSite(info().inlineScriptTree(), pc);
    }

    BytecodeSite* maybeTrackedOptimizationSite(jsbytecode* pc);

    MDefinition* lexicalCheck_;

    void setLexicalCheck(MDefinition* lexical) {
        MOZ_ASSERT(!lexicalCheck_);
        lexicalCheck_ = lexical;
    }
    MDefinition* takeLexicalCheck() {
        MDefinition* lexical = lexicalCheck_;
        lexicalCheck_ = nullptr;
        return lexical;
    }

    /* Information used for inline-call builders. */
    MResumePoint* callerResumePoint_;
    jsbytecode* callerPC() {
        return callerResumePoint_ ? callerResumePoint_->pc() : nullptr;
    }
    IonBuilder* callerBuilder_;

    IonBuilder* outermostBuilder();

    struct LoopHeader {
        jsbytecode* pc;
        MBasicBlock* header;

        LoopHeader(jsbytecode* pc, MBasicBlock* header)
          : pc(pc), header(header)
        {}
    };

    Vector<MDefinition*, 2, JitAllocPolicy> iterators_;
    Vector<LoopHeader, 0, JitAllocPolicy> loopHeaders_;
    Vector<MBasicBlock*, 0, JitAllocPolicy> loopHeaderStack_;
#ifdef DEBUG
    Vector<const CFGBlock*, 0, JitAllocPolicy> cfgLoopHeaderStack_;
#endif

    BaselineInspector* inspector;

    size_t inliningDepth_;

    // Total bytecode length of all inlined scripts. Only tracked for the
    // outermost builder.
    size_t inlinedBytecodeLength_;

    // Cutoff to disable compilation if excessive time is spent reanalyzing
    // loop bodies to compute a fixpoint of the types for loop variables.
    static const size_t MAX_LOOP_RESTARTS = 40;
    size_t numLoopRestarts_;

    // True if script->failedBoundsCheck is set for the current script or
    // an outer script.
    bool failedBoundsCheck_;

    // True if script->failedShapeGuard is set for the current script or
    // an outer script.
    bool failedShapeGuard_;

    // True if script->failedLexicalCheck_ is set for the current script or
    // an outer script.
    bool failedLexicalCheck_;

#ifdef DEBUG
    // If this script uses the lazy arguments object.
    bool hasLazyArguments_;
#endif

    // If this is an inline builder, the call info for the builder.
    const CallInfo* inlineCallInfo_;

    // When compiling a call with multiple targets, we are first creating a
    // MGetPropertyCache.  This MGetPropertyCache is following the bytecode, and
    // is used to recover the JSFunction.  In some cases, the Type of the object
    // which own the property is enough for dispatching to the right function.
    // In such cases we do not have read the property, except when the type
    // object is unknown.
    //
    // As an optimization, we can dispatch a call based on the object group,
    // without doing the MGetPropertyCache.  This is what is achieved by
    // |IonBuilder::inlineCalls|.  As we might not know all the functions, we
    // are adding a fallback path, where this MGetPropertyCache would be moved
    // into.
    //
    // In order to build the fallback path, we have to capture a resume point
    // ahead, for the potential fallback path.  This resume point is captured
    // while building MGetPropertyCache.  It is capturing the state of Baseline
    // before the execution of the MGetPropertyCache, such as we can safely do
    // it in the fallback path.
    //
    // This field is used to discard the resume point if it is not used for
    // building a fallback path.

    // Discard the prior resume point while setting a new MGetPropertyCache.
    void replaceMaybeFallbackFunctionGetter(MGetPropertyCache* cache);

    // Discard the MGetPropertyCache if it is handled by WrapMGetPropertyCache.
    void keepFallbackFunctionGetter(MGetPropertyCache* cache) {
        if (cache == maybeFallbackFunctionGetter_)
            maybeFallbackFunctionGetter_ = nullptr;
    }

    MGetPropertyCache* maybeFallbackFunctionGetter_;

    bool needsPostBarrier(MDefinition* value);

    // Used in tracking outcomes of optimization strategies for devtools.
    void startTrackingOptimizations();

    // The track* methods below are called often. Do not combine them with the
    // unchecked variants, despite the unchecked variants having no other
    // callers.
    void trackTypeInfo(JS::TrackedTypeSite site, MIRType mirType,
                       TemporaryTypeSet* typeSet)
    {
        if (MOZ_UNLIKELY(current->trackedSite()->hasOptimizations()))
            trackTypeInfoUnchecked(site, mirType, typeSet);
    }
    void trackTypeInfo(JS::TrackedTypeSite site, JSObject* obj) {
        if (MOZ_UNLIKELY(current->trackedSite()->hasOptimizations()))
            trackTypeInfoUnchecked(site, obj);
    }
    void trackTypeInfo(CallInfo& callInfo) {
        if (MOZ_UNLIKELY(current->trackedSite()->hasOptimizations()))
            trackTypeInfoUnchecked(callInfo);
    }
    void trackOptimizationAttempt(JS::TrackedStrategy strategy) {
        if (MOZ_UNLIKELY(current->trackedSite()->hasOptimizations()))
            trackOptimizationAttemptUnchecked(strategy);
    }
    void amendOptimizationAttempt(uint32_t index) {
        if (MOZ_UNLIKELY(current->trackedSite()->hasOptimizations()))
            amendOptimizationAttemptUnchecked(index);
    }
    void trackOptimizationOutcome(JS::TrackedOutcome outcome) {
        if (MOZ_UNLIKELY(current->trackedSite()->hasOptimizations()))
            trackOptimizationOutcomeUnchecked(outcome);
    }
    void trackOptimizationSuccess() {
        if (MOZ_UNLIKELY(current->trackedSite()->hasOptimizations()))
            trackOptimizationSuccessUnchecked();
    }
    void trackInlineSuccess(InliningStatus status = InliningStatus_Inlined) {
        if (MOZ_UNLIKELY(current->trackedSite()->hasOptimizations()))
            trackInlineSuccessUnchecked(status);
    }

    bool forceInlineCaches() {
        return MOZ_UNLIKELY(JitOptions.forceInlineCaches);
    }

    // Out-of-line variants that don't check if optimization tracking is
    // enabled.
    void trackTypeInfoUnchecked(JS::TrackedTypeSite site, MIRType mirType,
                                TemporaryTypeSet* typeSet);
    void trackTypeInfoUnchecked(JS::TrackedTypeSite site, JSObject* obj);
    void trackTypeInfoUnchecked(CallInfo& callInfo);
    void trackOptimizationAttemptUnchecked(JS::TrackedStrategy strategy);
    void amendOptimizationAttemptUnchecked(uint32_t index);
    void trackOptimizationOutcomeUnchecked(JS::TrackedOutcome outcome);
    void trackOptimizationSuccessUnchecked();
    void trackInlineSuccessUnchecked(InliningStatus status);
};

class CallInfo
{
    MDefinition* fun_;
    MDefinition* thisArg_;
    MDefinition* newTargetArg_;
    MDefinitionVector args_;
    // If non-empty, this corresponds to the stack prior any implicit inlining
    // such as before JSOP_FUNAPPLY.
    MDefinitionVector priorArgs_;

    bool constructing_:1;

    // True if the caller does not use the return value.
    bool ignoresReturnValue_:1;

    bool setter_:1;
    bool apply_:1;

  public:
    CallInfo(TempAllocator& alloc, jsbytecode* pc, bool constructing, bool ignoresReturnValue)
      : fun_(nullptr),
        thisArg_(nullptr),
        newTargetArg_(nullptr),
        args_(alloc),
        priorArgs_(alloc),
        constructing_(constructing),
        ignoresReturnValue_(ignoresReturnValue),
        setter_(false),
        apply_(JSOp(*pc) == JSOP_FUNAPPLY)
    { }

    MOZ_MUST_USE bool init(CallInfo& callInfo) {
        MOZ_ASSERT(constructing_ == callInfo.constructing());

        fun_ = callInfo.fun();
        thisArg_ = callInfo.thisArg();
        ignoresReturnValue_ = callInfo.ignoresReturnValue();

        if (constructing())
            newTargetArg_ = callInfo.getNewTarget();

        if (!args_.appendAll(callInfo.argv()))
            return false;

        return true;
    }

    MOZ_MUST_USE bool init(MBasicBlock* current, uint32_t argc) {
        MOZ_ASSERT(args_.empty());

        // Get the arguments in the right order
        if (!args_.reserve(argc))
            return false;

        if (constructing())
            setNewTarget(current->pop());

        for (int32_t i = argc; i > 0; i--)
            args_.infallibleAppend(current->peek(-i));
        current->popn(argc);

        // Get |this| and |fun|
        setThis(current->pop());
        setFun(current->pop());

        return true;
    }

    // Before doing any pop to the stack, capture whatever flows into the
    // instruction, such that we can restore it later.
    AbortReasonOr<Ok> savePriorCallStack(MIRGenerator* mir, MBasicBlock* current, size_t peekDepth);

    void popPriorCallStack(MBasicBlock* current) {
        if (priorArgs_.empty())
            popCallStack(current);
        else
            current->popn(priorArgs_.length());
    }

    AbortReasonOr<Ok> pushPriorCallStack(MIRGenerator* mir, MBasicBlock* current) {
        if (priorArgs_.empty())
            return pushCallStack(mir, current);
        for (MDefinition* def : priorArgs_)
            current->push(def);
        return Ok();
    }

    void popCallStack(MBasicBlock* current) {
        current->popn(numFormals());
    }

    AbortReasonOr<Ok> pushCallStack(MIRGenerator* mir, MBasicBlock* current) {
        // Ensure sufficient space in the slots: needed for inlining from FUNAPPLY.
        if (apply_) {
            uint32_t depth = current->stackDepth() + numFormals();
            if (depth > current->nslots()) {
                if (!current->increaseSlots(depth - current->nslots()))
                    return mir->abort(AbortReason::Alloc);
            }
        }

        current->push(fun());
        current->push(thisArg());

        for (uint32_t i = 0; i < argc(); i++)
            current->push(getArg(i));

        if (constructing())
            current->push(getNewTarget());

        return Ok();
    }

    uint32_t argc() const {
        return args_.length();
    }
    uint32_t numFormals() const {
        return argc() + 2 + constructing();
    }

    MOZ_MUST_USE bool setArgs(const MDefinitionVector& args) {
        MOZ_ASSERT(args_.empty());
        return args_.appendAll(args);
    }

    MDefinitionVector& argv() {
        return args_;
    }

    const MDefinitionVector& argv() const {
        return args_;
    }

    MDefinition* getArg(uint32_t i) const {
        MOZ_ASSERT(i < argc());
        return args_[i];
    }

    MDefinition* getArgWithDefault(uint32_t i, MDefinition* defaultValue) const {
        if (i < argc())
            return args_[i];

        return defaultValue;
    }

    void setArg(uint32_t i, MDefinition* def) {
        MOZ_ASSERT(i < argc());
        args_[i] = def;
    }

    MDefinition* thisArg() const {
        MOZ_ASSERT(thisArg_);
        return thisArg_;
    }

    void setThis(MDefinition* thisArg) {
        thisArg_ = thisArg;
    }

    bool constructing() const {
        return constructing_;
    }

    bool ignoresReturnValue() const {
        return ignoresReturnValue_;
    }

    void setNewTarget(MDefinition* newTarget) {
        MOZ_ASSERT(constructing());
        newTargetArg_ = newTarget;
    }
    MDefinition* getNewTarget() const {
        MOZ_ASSERT(newTargetArg_);
        return newTargetArg_;
    }

    bool isSetter() const {
        return setter_;
    }
    void markAsSetter() {
        setter_ = true;
    }

    MDefinition* fun() const {
        MOZ_ASSERT(fun_);
        return fun_;
    }

    void setFun(MDefinition* fun) {
        fun_ = fun;
    }

    void setImplicitlyUsedUnchecked() {
        fun_->setImplicitlyUsedUnchecked();
        thisArg_->setImplicitlyUsedUnchecked();
        if (newTargetArg_)
            newTargetArg_->setImplicitlyUsedUnchecked();
        for (uint32_t i = 0; i < argc(); i++)
            getArg(i)->setImplicitlyUsedUnchecked();
    }
};

} // namespace jit
} // namespace js

#endif /* jit_IonBuilder_h */
