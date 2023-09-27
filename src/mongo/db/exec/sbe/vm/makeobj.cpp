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
            produceBsonObject(ctx.spec, elemTag, elemVal, ctx.stackStartOffset, ctx.code, bob);
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
        produceBsonObject(ctx.spec, tag, val, ctx.stackStartOffset, ctx.code, nestedBob);
    }
}

// Iterator for BSON objects.
class BsonFieldCursor {
public:
    BsonFieldCursor(const char* be) : _be(be) {
        _end = _be + ConstDataView(_be).read<LittleEndian<uint32_t>>();
        _be += 4;
        if (_be != _end - 1) {
            _fieldName = bson::fieldNameAndLength(_be);
            _nextBe = bson::advance(_be, _fieldName.size());
        }
    }
    bool atEnd() const {
        return _be == _end - 1;
    }
    void moveNext() {
        _be = _nextBe;
        if (_be != _end - 1) {
            _fieldName = bson::fieldNameAndLength(_be);
            _nextBe = bson::advance(_be, _fieldName.size());
        }
    }
    StringData fieldName() const {
        return _fieldName;
    }
    std::pair<value::TypeTags, value::Value> value() const {
        return bson::convertFrom<true>(bsonElement());
    }
    void appendTo(UniqueBSONObjBuilder& bob) const {
        bob.append(bsonElement());
    }

private:
    BSONElement bsonElement() const {
        auto fieldNameLenWithNull = _fieldName.size() + 1;
        auto totalSize = _nextBe - _be;
        return BSONElement(_be, fieldNameLenWithNull, totalSize, BSONElement::TrustedInitTag{});
    }

    const char* _be{nullptr};
    const char* _end{nullptr};
    const char* _nextBe{nullptr};
    StringData _fieldName;
};

// Iterator for SBE objects.
class FieldCursor {
public:
    FieldCursor(value::Object* objRoot) : _objRoot(objRoot), _idx(0) {}
    bool atEnd() const {
        return _idx >= _objRoot->size();
    }
    void moveNext() {
        ++_idx;
    }
    StringData fieldName() const {
        return _objRoot->field(_idx);
    }
    std::pair<value::TypeTags, value::Value> value() const {
        return _objRoot->getAt(_idx);
    }
    void appendTo(UniqueBSONObjBuilder& bob) const {
        auto [tag, val] = _objRoot->getAt(_idx);
        bson::appendValueToBsonObj(bob, _objRoot->field(_idx), tag, val);
    }

private:
    value::Object* _objRoot{nullptr};
    size_t _idx{0};
};

// "Empty" iterator type whose atEnd() method always returns true.
class EmptyFieldCursor {
public:
    EmptyFieldCursor() = default;
    bool atEnd() const {
        return true;
    }
    void moveNext() {}
    StringData fieldName() const {
        return ""_sd;
    }
    std::pair<value::TypeTags, value::Value> value() const {
        return {value::TypeTags::Nothing, 0};
    }
    void appendTo(UniqueBSONObjBuilder& bob) const {}
};

void ByteCode::produceBsonObject(const MakeObjSpec* spec,
                                 value::TypeTags rootTag,
                                 value::Value rootVal,
                                 int stackStartOffset,
                                 const CodeFragment* code,
                                 UniqueBSONObjBuilder& bob) {
    using TypeTags = value::TypeTags;

    // Define the processFields() lambda, which will do the actual work of scanning the root
    // object ('rootTag' / 'rootVal') and building the output object.
    auto processFields = [&](auto&& cursor) {
        auto& fields = spec->fields;
        auto& fieldInfos = spec->fieldInfos;
        const size_t numFields = fields.size();
        const size_t numKeepOrDrops = spec->numKeepOrDrops;
        const size_t numValueArgs = spec->numValueArgs;
        const size_t numMandatoryLamsAndMakeObjs =
            spec->numMandatoryLambdas + spec->numMandatoryMakeObjs;
        const bool isClosed = spec->fieldBehavior == MakeObjSpec::FieldBehavior::kClosed;
        const bool recordVisits = numValueArgs + numMandatoryLamsAndMakeObjs > 0;

        // The 'visited' array keeps track of which computed fields have been visited so far so
        // that later we can append the non-visited computed fields to the end of the object.
        //
        // Note that we only use the 'visited' array when 'recordVisits' is true.
        char* visited = nullptr;
        absl::InlinedVector<char, 128> visitedVec;

        if (recordVisits) {
            size_t visitedSize = numFields - numKeepOrDrops;
            visitedVec.resize(visitedSize);
            visited = visitedVec.data();
        }

        size_t fieldsLeft = numFields;
        size_t valueArgsLeft = numValueArgs;
        size_t mandatoryLamsAndMakeObjsVisited = 0;

        // If 'isClosed' is false, loop over the input object until 'fieldsLeft == 0' is true. If
        // 'isClosed' is true, loop over the input object until 'fieldsLeft == 0' is true or until
        // 'fieldsLeft == 1 && valueArgsLeft == 1' is true.
        for (; !cursor.atEnd() && (fieldsLeft > (isClosed ? 1 : 0) || fieldsLeft != valueArgsLeft);
             cursor.moveNext()) {
            // Get the current field name, store it in 'fieldName', and look it up in 'fields'.
            StringData fieldName = cursor.fieldName();
            size_t fieldIdx = fields.findPos(fieldName);

            if (fieldIdx == StringListSet::npos) {
                if (!isClosed) {
                    cursor.appendTo(bob);
                }
            } else if (fieldIdx < numKeepOrDrops) {
                --fieldsLeft;

                if (isClosed) {
                    cursor.appendTo(bob);
                }
            } else {
                --fieldsLeft;

                auto& fieldInfo = fieldInfos[fieldIdx];

                if (recordVisits) {
                    auto visitedIdx = fieldIdx - numKeepOrDrops;
                    visited[visitedIdx] = 1;
                }

                if (fieldInfo.isValueArg()) {
                    // Get the value arg and store it into 'tag'/'val'.
                    --valueArgsLeft;
                    size_t argIdx = fieldInfo.getValueArgIdx();
                    auto [_, tag, val] = getFromStack(stackStartOffset + argIdx);
                    // Append the value arg to 'bob'.
                    bson::appendValueToBsonObj(bob, fieldName, tag, val);
                } else if (fieldInfo.isLambdaArg()) {
                    // Increment 'mandatoryLamsAndMakeObjsVisited' if appropriate.
                    const auto& lambdaArg = fieldInfo.getLambdaArg();
                    if (!lambdaArg.returnsNothingOnMissingInput) {
                        ++mandatoryLamsAndMakeObjsVisited;
                    }
                    // Get the current value from 'cursor' and store it to 'inputTag'/'inputVal'.
                    auto [inputTag, inputVal] = cursor.value();
                    // Get the lambda corresponding to 'fieldIdx'.
                    size_t argIdx = lambdaArg.argIdx;
                    auto [_, t, v] = getFromStack(stackStartOffset + argIdx);
                    // Get the offset of the lambda.
                    tassert(7103501, "Expected arg to be LocalLambda", t == TypeTags::LocalLambda);
                    int64_t lamPos = value::bitcastTo<int64_t>(v);
                    // Push the lambda's input onto the stack and invoke the lambda.
                    pushStack(false, inputTag, inputVal);
                    runLambdaInternal(code, lamPos);
                    // Append the lambda's return value to 'bob', then pop the return value
                    // off of the stack and release it.
                    auto [__, tag, val] = getFromStack(0);
                    bson::appendValueToBsonObj(bob, fieldName, tag, val);
                    popAndReleaseStack();
                } else if (fieldInfo.isMakeObj()) {
                    // Increment 'mandatoryLamsAndMakeObjsVisited' if appropriate.
                    auto fieldSpec = fieldInfo.getMakeObjSpec();
                    if (!fieldSpec->returnsNothingOnMissingInput()) {
                        ++mandatoryLamsAndMakeObjsVisited;
                    }
                    // Get the current value from 'cursor' and store it to 'inputTag'/'inputVal'.
                    auto [inputTag, inputVal] = cursor.value();
                    // Call traverseAndProduceBsonObj() to produce the object.
                    TraverseAndProduceBsonObjContext ctx{fieldSpec, stackStartOffset, code};
                    traverseAndProduceBsonObj(ctx, inputTag, inputVal, fieldName, bob);
                } else {
                    MONGO_UNREACHABLE_TASSERT(7103502);
                }
            }
        }

        // If 'isClosed' is false and 'cursor' has not reached the end of the input object,
        // copy over the remaining fields from the input object to the output object.
        if (!isClosed) {
            for (; !cursor.atEnd(); cursor.moveNext()) {
                cursor.appendTo(bob);
            }
        }

        // If there are remaining fields in 'fields' that need to be processed, take care of
        // processing those remaining fields here.
        if (recordVisits &&
            (valueArgsLeft > 0 || mandatoryLamsAndMakeObjsVisited < numMandatoryLamsAndMakeObjs)) {
            // Loop over 'fields' (skipping past the simple "keeps"/"drops" at the beginning
            // of the vector).
            for (size_t fieldIdx = numKeepOrDrops; fieldIdx < numFields; ++fieldIdx) {
                // Skip fields that have already been visited.
                auto& fieldInfo = fieldInfos[fieldIdx];
                auto visitedIdx = fieldIdx - numKeepOrDrops;
                if (visited[visitedIdx]) {
                    continue;
                }

                // Get the field name for this field and then consult 'fieldInfo' to see what
                // action should be taken.
                auto fieldName = StringData(fields[fieldIdx]);

                if (fieldInfo.isValueArg()) {
                    // Get the value arg and store it into 'tag'/'val'.
                    size_t argIdx = fieldInfo.getValueArgIdx();
                    auto [_, tag, val] = getFromStack(stackStartOffset + argIdx);

                    // Append the value arg to 'bob'.
                    bson::appendValueToBsonObj(bob, fieldName, tag, val);
                } else if (fieldInfo.isLambdaArg()) {
                    // If 'lambdaArg.returnsNothingOnMissingInput' is false, then we need to
                    // invoke the lamdba.
                    const auto& lambdaArg = fieldInfo.getLambdaArg();
                    if (lambdaArg.returnsNothingOnMissingInput) {
                        continue;
                    }
                    // Get the lambda corresponding to 'fieldIdx'.
                    size_t argIdx = lambdaArg.argIdx;
                    auto [_, t, v] = getFromStack(stackStartOffset + argIdx);
                    // Get the offset of the lambda.
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
                } else if (fieldInfo.isMakeObj()) {
                    // If 'fieldSpec->returnsNothingOnMissingInput()' is false, then we need to
                    // produce the object.
                    MakeObjSpec* fieldSpec = fieldInfo.getMakeObjSpec();

                    if (!fieldSpec->returnsNothingOnMissingInput()) {
                        // Call traverseAndProduceBsonObj() to produce the object.
                        TraverseAndProduceBsonObjContext ctx{fieldSpec, stackStartOffset, code};
                        traverseAndProduceBsonObj(ctx, TypeTags::Nothing, 0, fieldName, bob);
                    }
                } else {
                    MONGO_UNREACHABLE_TASSERT(7103503);
                }
            }
        }
    };

    // Invoke the processFields() lambda with the appropriate iterator type.
    switch (rootTag) {
        case TypeTags::bsonObject:
            // For BSON objects, use BsonFieldCursor.
            processFields(BsonFieldCursor(value::bitcastTo<const char*>(rootVal)));
            break;
        case TypeTags::Object:
            // For SBE objects, use FieldCursor.
            processFields(FieldCursor(value::getObjectView(rootVal)));
            break;
        default:
            // For all other types, use EmptyFieldCursor.
            processFields(EmptyFieldCursor());
            break;
    }
}

}  // namespace mongo::sbe::vm
