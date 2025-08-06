/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#pragma once

#ifndef MONGO_SBE_VM_MAKEOBJ_H_WHITELIST
// This file contains a usage of anonymous namespaces which are very bad to have in headers since
// they can lead to ODR violations. Unfortunately when removed, it leads to a significant
// performance loss (see https://jira.mongodb.org/browse/BF-35748). This regression does not go away
// even when doing the obvious fix of merging this header and the two files that include it into a
// single cpp file. For now we will just live with this being in a header. All files that include
// this header must follow the following rules to avoid ODR violations:
//  * This header may only be included in leaf cpp files, not other headers
//  * This must be the last header included
//  * The #define MONGO_SBE_VM_MAKEOBJ_H_WHITELIST must be after all other headers
//
// Using #warning rather than #error to play nicely with opening this header in  clangd.
#warning "vm_makeobj.h is only allowed to be included in whitelisted cpp files. See above comment"
#endif

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/makeobj_spec.h"
#include "mongo/db/exec/sbe/values/util.h"
#include "mongo/db/exec/sbe/vm/makeobj_cursors.h"
#include "mongo/db/exec/sbe/vm/makeobj_writers.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/platform/compiler.h"

#include <limits>

namespace mongo::sbe::vm {
namespace {  // NOLINT(google-build-namespaces) See WHITELIST comment above.
class MakeObjImpl : ByteCode::MakeObjImplBase {
public:
    using BaseT = ByteCode::MakeObjImplBase;
    using BaseT::BaseT;

    template <typename ObjWriterT, typename ArrWriterT>
    MONGO_COMPILER_ALWAYS_INLINE FastTuple<bool, value::TypeTags, value::Value> makeObj() const {
        constexpr int64_t maxInt64 = std::numeric_limits<int64_t>::max();

        auto [specOwned, specTag, specVal] = getSpec();
        auto [objOwned, objTag, objVal] = getInputObject();

        if (specTag != value::TypeTags::makeObjSpec) {
            return {false, value::TypeTags::Nothing, 0};
        }

        auto spec = value::getMakeObjSpecView(specVal);
        auto maxDepth = spec->traversalDepth ? *spec->traversalDepth : maxInt64;

        // If 'obj' is not an object or array, or if 'obj' is an array and 'maxDepth' is 0,
        // then check if there is applicable "NonObjInputBehavior" that should be applied.
        if (spec->nonObjInputBehavior != MakeObjSpec::NonObjInputBehavior::kNewObj &&
            !value::isObject(objTag) && (!value::isArray(objTag) || maxDepth == 0)) {
            if (spec->nonObjInputBehavior == MakeObjSpec::NonObjInputBehavior::kReturnNothing) {
                // If the input is Nothing or not an Object and if 'nonObjInputBehavior' equals
                // 'kReturnNothing', then return Nothing.
                return {false, value::TypeTags::Nothing, 0};
            } else if (spec->nonObjInputBehavior ==
                       MakeObjSpec::NonObjInputBehavior::kReturnInput) {
                // If the input is Nothing or not an Object and if 'nonObjInputBehavior' equals
                // 'kReturnInput', then return the input.
                return extractInputObject();
            }
        }

        // If 'tag' is an array and we have not exceeded the maximum depth yet, traverse the array.
        if (maxDepth > 0 && value::isArray(objTag)) {
            ArrWriterT bab;

            auto newMaxDepth = maxDepth == maxInt64 ? maxDepth : maxDepth - 1;
            traverseAndProduceObj(spec, objTag, objVal, newMaxDepth, bab);

            auto [resultTag, resultVal] = bab.done();
            return {true, resultTag, resultVal};
        } else {
            ObjWriterT bob;

            produceObj(spec, bob, objTag, objVal);

            auto [resultTag, resultVal] = bob.done();
            return {true, resultTag, resultVal};
        }
    }

private:
    /**
     * produceObj() takes a MakeObjSpec ('spec'), a root value ('rootTag' and 'rootVal'),
     * and 0 or more "computed" values as inputs, it builds an output BSON object based on the
     * instructions provided by 'spec' and based on the contents of 'root' and the computed input
     * values, and then it returns the output object. (Note the computed input values are not
     * directly passed in as C++ parameters -- instead the computed input values are passed via
     * the VM's stack.)
     */
    template <typename ObjWriterT>
    void produceObj(const MakeObjSpec* spec,
                    ObjWriterT& bob,
                    value::TypeTags rootTag,
                    value::Value rootVal) const {
        // Invoke produceObj<ObjWriterT, CursorT>() with the appropriate cursor type. For
        // SBE objects, we use ObjectCursor. For all other types, we use BsonObjCursor.
        if (rootTag == value::TypeTags::Object) {
            auto obj = value::getObjectView(rootVal);

            produceObj(spec, bob, ObjectCursor(obj));
        } else {
            const char* obj = rootTag == value::TypeTags::bsonObject
                ? value::bitcastTo<const char*>(rootVal)
                : BSONObj::kEmptyObject.objdata();

            produceObj(spec, bob, BsonObjCursor(obj));
        }
    }

    template <typename ObjWriterT, typename CursorT>
    void produceObj(const MakeObjSpec* spec, ObjWriterT& bob, CursorT cursor) const {
        using TypeTags = value::TypeTags;
        using Value = value::Value;

        const auto& fields = spec->fields;
        const auto* actions = spec->getActionsData();

        const bool isClosed = spec->fieldsScopeIsClosed();
        const bool recordVisits = !spec->mandatoryFields.empty();
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

        size_t fieldsLeft = spec->numFieldsToSearchFor;

        for (; !cursor.atEnd() && fieldsLeft > 0; cursor.moveNext()) {
            // Get the idx of the current field and the corresponding action.
            const size_t fieldIdx = fields.findPos(cursor.fieldName());
            auto t = fieldIdx != StringListSet::npos ? actions[fieldIdx].type() : defActionType;

            if (t == MakeObjSpec::ActionType::kDrop) {
                fieldsLeft -= static_cast<uint8_t>(!isClosed);
                continue;
            } else if (t == MakeObjSpec::ActionType::kKeep) {
                fieldsLeft -= static_cast<uint8_t>(isClosed);
                cursor.appendTo(bob);
                continue;
            } else if (t == MakeObjSpec::ActionType::kAddArg) {
                fieldsLeft -= static_cast<uint8_t>(!isClosed);
                continue;
            }

            --fieldsLeft;
            if (recordVisits) {
                visited[fieldIdx] = 1;
            }

            auto& action = actions[fieldIdx];

            if (t == MakeObjSpec::ActionType::kSetArg) {
                auto fieldName = cursor.fieldName();

                performSetArgAction(action, fieldName, bob);
            } else if (t == MakeObjSpec::ActionType::kLambdaArg) {
                auto fieldName = cursor.fieldName();
                auto [tag, val] = cursor.value();

                performLambdaArgAction(action, tag, val, fieldName, bob);
            } else if (t == MakeObjSpec::ActionType::kMakeObj) {
                auto fieldName = cursor.fieldName();
                auto [tag, val] = cursor.value();

                performMakeObjAction(action, tag, val, fieldName, bob);
            } else {
                MONGO_UNREACHABLE_TASSERT(7103502);
            }
        }

        // If 'isClosed' is false and 'cursor' has not reached the end of the input object, copy
        // over the remaining fields from the input object to the output object.
        if (!isClosed) {
            for (; !cursor.atEnd(); cursor.moveNext()) {
                cursor.appendTo(bob);
            }
        }

        // If there are remaining fields in 'fields' that need to be processed, take care of
        // processing those remaining fields here.
        if (recordVisits) {
            // Loop over 'fields'.
            const auto& mandatoryFields = spec->mandatoryFields;
            for (size_t fieldIdx : mandatoryFields) {
                // Skip fields that have already been visited.
                if (visited[fieldIdx]) {
                    continue;
                }

                // Get the field name for this field, and then consult 'action' to see what
                // action should be taken.
                StringData fieldName = fields[fieldIdx];
                const auto& action = actions[fieldIdx];

                const auto tag = TypeTags::Nothing;
                const auto val = Value{0u};

                if (action.isSetArg()) {
                    performSetArgAction(action, fieldName, bob);
                } else if (action.isAddArg()) {
                    performAddArgAction(action, fieldName, bob);
                } else if (action.isLambdaArg()) {
                    performLambdaArgAction(action, tag, val, fieldName, bob);
                } else if (action.isMakeObj()) {
                    performMakeObjAction(action, tag, val, fieldName, bob);
                } else {
                    MONGO_UNREACHABLE_TASSERT(7103503);
                }
            }
        }
    }

    template <typename ArrWriterT>
    void traverseAndProduceObj(const MakeObjSpec* spec,
                               value::TypeTags tag,
                               value::Value val,
                               int64_t maxDepth,
                               ArrWriterT& bab) const {
        constexpr int64_t maxInt64 = std::numeric_limits<int64_t>::max();

        // Loop over each element in the array.
        value::arrayForEach(tag, val, [&](value::TypeTags elemTag, value::Value elemVal) {
            if (maxDepth > 0 && value::isArray(elemTag)) {
                // If 'tag' is an array and we have not exceeded the maximum depth yet, traverse
                // the array.
                auto newMaxDepth = maxDepth == maxInt64 ? maxDepth : maxDepth - 1;
                auto nestedBab = bab.startArr();

                traverseAndProduceObj(spec, elemTag, elemVal, newMaxDepth, nestedBab);

                bab.finishArr(std::move(nestedBab));
            } else if (spec->nonObjInputBehavior != MakeObjSpec::NonObjInputBehavior::kNewObj &&
                       !value::isObject(elemTag)) {
                // Otherwise, if 'tag' is not an object and 'nonObjInputBehavior' is not 'kNewObj',
                // then we either append 'tag'/'val' if ('nonObjInputBehavior' is 'kReturnInput')
                // or we skip 'tag'/'val' (if 'nonObjInputBehavior' is 'kReturnNothing').
                if (spec->nonObjInputBehavior == MakeObjSpec::NonObjInputBehavior::kReturnInput) {
                    bab.appendValue(elemTag, elemVal);
                }
            } else {
                // For all other cases, call produceObj().
                auto bob = bab.startObj();

                produceObj(spec, bob, elemTag, elemVal);

                bab.finishObj(std::move(bob));
            }
        });
    }

    template <typename ObjWriterT>
    void traverseAndProduceObj(const MakeObjSpec* spec,
                               value::TypeTags tag,
                               value::Value val,
                               StringData fieldName,
                               ObjWriterT& bob) const {
        constexpr int64_t maxInt64 = std::numeric_limits<int64_t>::max();

        auto maxDepth = spec->traversalDepth ? *spec->traversalDepth : maxInt64;

        if (value::isArray(tag) && maxDepth > 0) {
            // If 'tag' is an array and we have not exceeded the maximum depth yet, traverse the
            // array.
            auto newMaxDepth = maxDepth == maxInt64 ? maxDepth : maxDepth - 1;
            auto bab = bob.startArr(fieldName);

            traverseAndProduceObj(spec, tag, val, newMaxDepth, bab);

            bob.finishArr(fieldName, std::move(bab));
        } else if (spec->nonObjInputBehavior != MakeObjSpec::NonObjInputBehavior::kNewObj &&
                   !value::isObject(tag)) {
            // Otherwise, if 'tag' is not an object and 'nonObjInputBehavior' is not 'kNewObj',
            // then we either append 'tag'/'val' if ('nonObjInputBehavior' is 'kReturnInput') or
            // we skip 'tag'/'val' (if 'nonObjInputBehavior' is 'kReturnNothing').
            if (spec->nonObjInputBehavior == MakeObjSpec::NonObjInputBehavior::kReturnInput) {
                bob.appendValue(fieldName, tag, val);
            }
        } else {
            // For all other cases, call produceObj().
            auto nestedBob = bob.startObj(fieldName);

            produceObj(spec, nestedBob, tag, val);

            bob.finishObj(fieldName, std::move(nestedBob));
        }
    }

    template <typename ObjWriterT>
    MONGO_COMPILER_ALWAYS_INLINE void performSetArgAction(const MakeObjSpec::FieldAction& action,
                                                          StringData fieldName,
                                                          ObjWriterT& bob) const {
        size_t argIdx = action.getSetArgIdx();
        auto [_, tag, val] = getArg(argIdx);
        bob.appendValue(fieldName, tag, val);
    }

    template <typename ObjWriterT>
    MONGO_COMPILER_ALWAYS_INLINE void performAddArgAction(const MakeObjSpec::FieldAction& action,
                                                          StringData fieldName,
                                                          ObjWriterT& bob) const {
        size_t argIdx = action.getAddArgIdx();
        auto [_, tag, val] = getArg(argIdx);
        bob.appendValue(fieldName, tag, val);
    }

    template <typename ObjWriterT>
    MONGO_COMPILER_ALWAYS_INLINE void performLambdaArgAction(const MakeObjSpec::FieldAction& action,
                                                             value::TypeTags tag,
                                                             value::Value val,
                                                             StringData fieldName,
                                                             ObjWriterT& bob) const {
        const auto& lambdaArg = action.getLambdaArg();
        size_t argIdx = lambdaArg.argIdx;
        auto [_, lamTag, lamVal] = getArg(argIdx);

        tassert(7103506, "Expected arg to be LocalLambda", lamTag == value::TypeTags::LocalLambda);
        int64_t lamPos = value::bitcastTo<int64_t>(lamVal);

        auto [outputOwned, outputTag, outputVal] = invokeLambda(lamPos, tag, val);
        value::ValueGuard guard(outputOwned, outputTag, outputVal);

        bob.appendValue(fieldName, outputTag, outputVal);
    }

    template <typename ObjWriterT>
    MONGO_COMPILER_ALWAYS_INLINE void performMakeObjAction(const MakeObjSpec::FieldAction& action,
                                                           value::TypeTags tag,
                                                           value::Value val,
                                                           StringData fieldName,
                                                           ObjWriterT& bob) const {
        const MakeObjSpec* spec = action.getMakeObjSpec();
        traverseAndProduceObj(spec, tag, val, fieldName, bob);
    }
};
}  // namespace
}  // namespace mongo::sbe::vm
