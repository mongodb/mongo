/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <limits>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/makeobj_spec.h"
#include "mongo/db/exec/sbe/values/util.h"
#include "mongo/db/exec/sbe/vm/makeobj_input_fields_cursors.h"
#include "mongo/db/exec/sbe/vm/vm.h"

namespace mongo::sbe::vm {

void ByteCode::traverseAndProduceBsonObj(const TraverseAndProduceBsonObjContext& ctx,
                                         value::TypeTags tag,
                                         value::Value val,
                                         int64_t maxDepth,
                                         UniqueBSONArrayBuilder& bab) {
    constexpr int64_t maxInt64 = std::numeric_limits<int64_t>::max();

    // Loop over each element in the array.
    value::arrayForEach(tag, val, [&](value::TypeTags elemTag, value::Value elemVal) {
        if (maxDepth > 0 && value::isArray(elemTag)) {
            // If 'tag' is an array and we have not exceeded the maximum depth yet, traverse
            // the array.
            auto newMaxDepth = maxDepth == maxInt64 ? maxDepth : maxDepth - 1;

            UniqueBSONArrayBuilder nestedBab(bab.subarrayStart());
            traverseAndProduceBsonObj(ctx, elemTag, elemVal, newMaxDepth, nestedBab);
        } else if (ctx.spec->nonObjInputBehavior != MakeObjSpec::NonObjInputBehavior::kNewObj &&
                   !value::isObject(elemTag)) {
            // Otherwise, if 'tag' is not an object and 'nonObjInputBehavior' is not 'kNewObj',
            // then we either append 'tag'/'val' if ('nonObjInputBehavior' is 'kReturnInput') or we
            // skip 'tag'/'val' (if 'nonObjInputBehavior' is 'kReturnNothing').
            if (ctx.spec->nonObjInputBehavior == MakeObjSpec::NonObjInputBehavior::kReturnInput) {
                bson::appendValueToBsonArr(bab, elemTag, elemVal);
            }
        } else {
            // For all other cases, call produceBsonObject().
            UniqueBSONObjBuilder bob(bab.subobjStart());
            produceBsonObject(ctx.spec, ctx.stackOffsets, ctx.code, bob, elemTag, elemVal);
        }
    });
}

void ByteCode::traverseAndProduceBsonObj(const TraverseAndProduceBsonObjContext& ctx,
                                         value::TypeTags tag,
                                         value::Value val,
                                         StringData fieldName,
                                         UniqueBSONObjBuilder& bob) {
    constexpr int64_t maxInt64 = std::numeric_limits<int64_t>::max();

    auto maxDepth = ctx.spec->traversalDepth ? *ctx.spec->traversalDepth : maxInt64;

    if (value::isArray(tag) && maxDepth > 0) {
        // If 'tag' is an array and we have not exceeded the maximum depth yet, traverse the array.
        auto newMaxDepth = maxDepth == maxInt64 ? maxDepth : maxDepth - 1;

        UniqueBSONArrayBuilder bab(bob.subarrayStart(fieldName));
        traverseAndProduceBsonObj(ctx, tag, val, newMaxDepth, bab);
    } else if (ctx.spec->nonObjInputBehavior != MakeObjSpec::NonObjInputBehavior::kNewObj &&
               !value::isObject(tag)) {
        // Otherwise, if 'tag' is not an object and 'nonObjInputBehavior' is not 'kNewObj',
        // then we either append 'tag'/'val' if ('nonObjInputBehavior' is 'kReturnInput') or we
        // skip 'tag'/'val' (if 'nonObjInputBehavior' is 'kReturnNothing').
        if (ctx.spec->nonObjInputBehavior == MakeObjSpec::NonObjInputBehavior::kReturnInput) {
            bson::appendValueToBsonObj(bob, fieldName, tag, val);
        }
    } else {
        // For all other cases, call produceBsonObject().
        UniqueBSONObjBuilder nestedBob(bob.subobjStart(fieldName));
        produceBsonObject(ctx.spec, ctx.stackOffsets, ctx.code, nestedBob, tag, val);
    }
}

template <typename CursorT>
void ByteCode::produceBsonObject(const MakeObjSpec* spec,
                                 MakeObjStackOffsets stackOffsets,
                                 const CodeFragment* code,
                                 UniqueBSONObjBuilder& bob,
                                 CursorT cursor) {
    using TypeTags = value::TypeTags;
    using InputFields = MakeObjCursorInputFields;

    auto [fieldsStackOff, argsStackOff] = stackOffsets;

    auto& fields = spec->fields;
    auto& actions = spec->actions;
    auto& mandatoryFields = spec->mandatoryFields;
    const size_t numFieldsOfInterest = spec->numFieldsOfInterest;
    const size_t numValueArgs = spec->numValueArgs;
    const size_t numMandatoryLamsAndMakeObjs = spec->mandatoryFields.size() - spec->numValueArgs;
    const bool isClosed = spec->fieldsScopeIsClosed();
    const bool recordVisits = numValueArgs + numMandatoryLamsAndMakeObjs > 0;
    const auto defActionType =
        isClosed ? MakeObjSpec::ActionType::kDrop : MakeObjSpec::ActionType::kKeep;

    // The 'visited' array keeps track of which computed fields have been visited so far so
    // that later we can append the non-visited computed fields to the end of the object.
    //
    // Note that we only use the 'visited' array when 'recordVisits' is true.
    char* visited = nullptr;
    absl::InlinedVector<char, 128> visitedVec;

    if (recordVisits) {
        size_t visitedSize = fields.size();
        visitedVec.resize(visitedSize);
        visited = visitedVec.data();
    }

    size_t fieldsLeft = numFieldsOfInterest;
    size_t valueArgsLeft = numValueArgs;
    size_t mandatoryLamsAndMakeObjsVisited = 0;

    // If 'isClosed' is false, loop over the input object until 'fieldsLeft == 0' is true. If
    // 'isClosed' is true, loop over the input object until 'fieldsLeft == 0' is true or until
    // 'fieldsLeft == 1 && valueArgsLeft == 1' is true.
    for (; !cursor.atEnd() && (fieldsLeft > (isClosed ? 1 : 0) || fieldsLeft != valueArgsLeft);
         cursor.moveNext(fields)) {
        // Get the idx of the current field and the corresponding action type.
        auto fieldIdx = cursor.fieldIdx();
        auto t = fieldIdx != StringListSet::npos ? actions[fieldIdx].type() : defActionType;

        if (t == MakeObjSpec::ActionType::kDrop) {
            fieldsLeft -= static_cast<uint8_t>(!isClosed);
            continue;
        }

        if (t == MakeObjSpec::ActionType::kKeep) {
            fieldsLeft -= static_cast<uint8_t>(isClosed);
            cursor.appendTo(bob);
            continue;
        }

        --fieldsLeft;
        if (recordVisits) {
            visited[fieldIdx] = 1;
        }

        auto& action = actions[fieldIdx];

        if (t == MakeObjSpec::ActionType::kValueArg) {
            // Get the value arg and store it into 'tag'/'val'.
            --valueArgsLeft;
            size_t argIdx = action.getValueArgIdx();
            auto [_, tag, val] = getFromStack(argsStackOff + argIdx);

            // Append the value arg to 'bob'.
            auto fieldName = cursor.fieldName();
            bson::appendValueToBsonObj(bob, fieldName, tag, val);
        } else if (t == MakeObjSpec::ActionType::kLambdaArg) {
            // Increment 'mandatoryLamsAndMakeObjsVisited' if appropriate.
            const auto& lambdaArg = action.getLambdaArg();
            if (!lambdaArg.returnsNothingOnMissingInput) {
                ++mandatoryLamsAndMakeObjsVisited;
            }
            // Get the current value from 'cursor' and store it to 'inputTag'/'inputVal'.
            auto [inputTag, inputVal] = cursor.value();

            // Get the lambda corresponding to 'fieldIdx' and get the offset of the lambda.
            size_t argIdx = lambdaArg.argIdx;
            auto [_, t, v] = getFromStack(argsStackOff + argIdx);
            tassert(7103501, "Expected arg to be LocalLambda", t == TypeTags::LocalLambda);
            int64_t lamPos = value::bitcastTo<int64_t>(v);

            // Push the lambda's input onto the stack and invoke the lambda.
            pushStack(false, inputTag, inputVal);
            runLambdaInternal(code, lamPos);

            // Append the lambda's return value to 'bob', then pop the return value
            // off of the stack and release it.
            auto fieldName = cursor.fieldName();
            auto [__, tag, val] = getFromStack(0);
            bson::appendValueToBsonObj(bob, fieldName, tag, val);
            popAndReleaseStack();
        } else if (t == MakeObjSpec::ActionType::kMakeObj) {
            // Increment 'mandatoryLamsAndMakeObjsVisited' if appropriate.
            auto fieldSpec = action.getMakeObjSpec();
            if (!fieldSpec->returnsNothingOnMissingInput()) {
                ++mandatoryLamsAndMakeObjsVisited;
            }

            // Get the current value from 'cursor' and store it to 'inputTag'/'inputVal', and then
            // call traverseAndProduceBsonObj() to produce the object.
            auto [inputTag, inputVal] = cursor.value();
            auto fieldName = cursor.fieldName();
            TraverseAndProduceBsonObjContext ctx{fieldSpec, {fieldsStackOff, argsStackOff}, code};
            traverseAndProduceBsonObj(ctx, inputTag, inputVal, fieldName, bob);
        } else {
            MONGO_UNREACHABLE_TASSERT(7103502);
        }
    }

    // If 'isClosed' is false and 'cursor' has not reached the end of the input object, copy over
    // the remaining fields from the input object to the output object.
    if (!isClosed) {
        for (; !cursor.atEnd(); cursor.moveNext(fields)) {
            cursor.appendTo(bob);
        }
    }

    // If there are remaining fields in 'fields' that need to be processed, take care of processing
    // those remaining fields here.
    if (recordVisits &&
        (valueArgsLeft > 0 || mandatoryLamsAndMakeObjsVisited < numMandatoryLamsAndMakeObjs)) {
        // Loop over 'fields' (skipping past the simple "keeps"/"drops" at the beginning
        // of the vector).
        for (size_t i = 0; i < mandatoryFields.size(); ++i) {
            // Skip fields that have already been visited.
            size_t fieldIdx = mandatoryFields[i];
            if (visited[fieldIdx]) {
                continue;
            }

            // Get the field name for this field, and then consult 'action' to see what
            // action should be taken.
            StringData fieldName = fields[fieldIdx];
            auto& action = actions[fieldIdx];

            if (action.isValueArg()) {
                // Get the value arg and store it into 'tag'/'val'.
                size_t argIdx = action.getValueArgIdx();
                auto [_, tag, val] = getFromStack(argsStackOff + argIdx);

                // Append the value arg to 'bob'.
                bson::appendValueToBsonObj(bob, fieldName, tag, val);
            } else if (action.isLambdaArg()) {
                // If 'lambdaArg.returnsNothingOnMissingInput' is false, then we need to
                // invoke the lamdba.
                const auto& lambdaArg = action.getLambdaArg();
                if (lambdaArg.returnsNothingOnMissingInput) {
                    continue;
                }

                // Get the lambda corresponding to 'fieldIdx' and the offset of the lambda.
                size_t argIdx = lambdaArg.argIdx;
                auto [_, t, v] = getFromStack(argsStackOff + argIdx);
                tassert(7103506, "Expected arg to be LocalLambda", t == TypeTags::LocalLambda);
                int64_t lamPos = value::bitcastTo<int64_t>(v);

                // Push the lambda's input (Nothing) onto the stack and invoke the lambda.
                pushStack(false, TypeTags::Nothing, 0);
                runLambdaInternal(code, lamPos);

                // Append the lambda's return value to 'bob', then pop the return value
                // off of the stack and release it.
                auto [__, tag, val] = getFromStack(0);
                bson::appendValueToBsonObj(bob, fieldName, tag, val);
                popAndReleaseStack();
            } else if (action.isMakeObj()) {
                // If 'fieldSpec->returnsNothingOnMissingInput()' is false, then we need to
                // produce the object.
                MakeObjSpec* fieldSpec = action.getMakeObjSpec();
                if (fieldSpec->returnsNothingOnMissingInput()) {
                    continue;
                }

                // Call traverseAndProduceBsonObj() to produce the object.
                TraverseAndProduceBsonObjContext ctx{
                    fieldSpec, {fieldsStackOff, argsStackOff}, code};
                traverseAndProduceBsonObj(ctx, TypeTags::Nothing, 0, fieldName, bob);
            } else {
                MONGO_UNREACHABLE_TASSERT(7103503);
            }
        }
    }
}

template void ByteCode::produceBsonObject<BsonObjCursor>(const MakeObjSpec* spec,
                                                         MakeObjStackOffsets stackOffsets,
                                                         const CodeFragment* code,
                                                         UniqueBSONObjBuilder& bob,
                                                         BsonObjCursor cursor);

template void ByteCode::produceBsonObject<ObjectCursor>(const MakeObjSpec* spec,
                                                        MakeObjStackOffsets stackOffsets,
                                                        const CodeFragment* code,
                                                        UniqueBSONObjBuilder& bob,
                                                        ObjectCursor cursor);

template void ByteCode::produceBsonObject<InputFieldsOnlyCursor>(const MakeObjSpec* spec,
                                                                 MakeObjStackOffsets stackOffsets,
                                                                 const CodeFragment* code,
                                                                 UniqueBSONObjBuilder& bob,
                                                                 InputFieldsOnlyCursor cursor);

template void ByteCode::produceBsonObject<BsonObjWithInputFieldsCursor>(
    const MakeObjSpec* spec,
    MakeObjStackOffsets stackOffsets,
    const CodeFragment* code,
    UniqueBSONObjBuilder& bob,
    BsonObjWithInputFieldsCursor cursor);

template void ByteCode::produceBsonObject<ObjWithInputFieldsCursor>(
    const MakeObjSpec* spec,
    MakeObjStackOffsets stackOffsets,
    const CodeFragment* code,
    UniqueBSONObjBuilder& bob,
    ObjWithInputFieldsCursor cursor);

void ByteCode::produceBsonObjectWithInputFields(const MakeObjSpec* spec,
                                                MakeObjStackOffsets stackOffs,
                                                const CodeFragment* code,
                                                UniqueBSONObjBuilder& bob,
                                                value::TypeTags objTag,
                                                value::Value objVal) {
    using TypeTags = value::TypeTags;
    using InputFields = MakeObjCursorInputFields;

    const auto& fields = spec->fields;
    const size_t numInputFields = spec->numInputFields ? *spec->numInputFields : 0;

    auto [fieldsStackOff, _] = stackOffs;
    auto inputFields = InputFields(*this, fieldsStackOff, numInputFields);

    auto bsonObjCursor = objTag == TypeTags::bsonObject
        ? boost::make_optional(BsonObjCursor(fields, value::bitcastTo<const char*>(objVal)))
        : boost::none;
    auto objCursor = objTag == TypeTags::Object
        ? boost::make_optional(ObjectCursor(fields, value::getObjectView(objVal)))
        : boost::none;

    // Invoke the produceBsonObject() lambda with the appropriate cursor type.
    if (objTag == TypeTags::Null) {
        size_t n = numInputFields;
        produceBsonObject(
            spec, stackOffs, code, bob, InputFieldsOnlyCursor(fields, inputFields, n));
    } else if (objTag == TypeTags::bsonObject) {
        produceBsonObject(spec,
                          stackOffs,
                          code,
                          bob,
                          BsonObjWithInputFieldsCursor(fields, inputFields, *bsonObjCursor));
    } else if (objTag == TypeTags::Object) {
        produceBsonObject(
            spec, stackOffs, code, bob, ObjWithInputFieldsCursor(fields, inputFields, *objCursor));
    } else {
        MONGO_UNREACHABLE_TASSERT(8146602);
    }
}

}  // namespace mongo::sbe::vm
