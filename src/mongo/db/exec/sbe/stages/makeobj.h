/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/value.h"

namespace mongo::sbe {

enum class MakeObjFieldBehavior { drop, keep };

enum class MakeObjOutputType { object, bsonObject };

/**
 * Base stage for creating a bsonObject or object.
 *
 * Template argument 'O' indicates which output type to use.
 *
 * Debug string formats:
 *
 *  mkobj objSlot (rootSlot [<list of field names>] drop|keep)?
 *       [projectedField_1 = slot_1, ..., projectedField_n = slot_n]
 *       forceNewObj returnOldObject childStage
 *
 *  mkbson objSlot (rootSlot [<list of field names>] drop|keep)?
 *       [projectedField_1 = slot_1, ..., projectedField_n = slot_n]
 *       forceNewObj returnOldObject childStage
 */
template <MakeObjOutputType O>
class MakeObjStageBase final : public PlanStage {
public:
    using FieldBehavior = MakeObjFieldBehavior;

    /**
     * Constructor. Arguments:
     * -input: Child PlanStage.
     * -objSlot: The output slot.
     *
     * -rootSlot (optional): Slot containing an object which the return object will be based on.
     * -fieldBehavior (optional): This may only be specified when 'rootSlot' is specified. Describes
     * what the behavior should be for each field in 'fields'. Either "drop" or "keep".
     * -fields: List of fields. What the stage does with each field depends on 'fieldBehavior'.
     *
     * -projectFields: List of fields which should be added to the result object using the values
     * from 'projectVars'.
     * -projectVars: See above.
     *
     * -forceNewObject, returnOldObject: Describes what the behavior should be when the resulting
     * object has no fields. May either return 'Nothing', an empty object, or the object in
     * 'rootSlot' unmodified.
     *
     * -planNodeId: Mapping to the corresponding QuerySolutionNode.
     */
    MakeObjStageBase(std::unique_ptr<PlanStage> input,
                     value::SlotId objSlot,
                     boost::optional<value::SlotId> rootSlot,
                     boost::optional<FieldBehavior> fieldBehavior,
                     std::vector<std::string> fields,
                     std::vector<std::string> projectFields,
                     value::SlotVector projectVars,
                     bool forceNewObject,
                     bool returnOldObject,
                     PlanNodeId planNodeId,
                     bool participateInTrialRunTracking = true);

    /**
     * A convenience constructor that takes a set instead of a vector for 'fields' and
     * 'projectedFields'.
     */
    MakeObjStageBase(std::unique_ptr<PlanStage> input,
                     value::SlotId objSlot,
                     boost::optional<value::SlotId> rootSlot,
                     boost::optional<FieldBehavior> fieldBehavior,
                     OrderedPathSet fields,
                     OrderedPathSet projectFields,
                     value::SlotVector projectVars,
                     bool forceNewObject,
                     bool returnOldObject,
                     PlanNodeId planNodeId);

    std::unique_ptr<PlanStage> clone() const final;

    void prepare(CompileCtx& ctx) final;
    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;
    void open(bool reOpen) final;
    PlanState getNext() final;
    void close() final;

    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final;
    const SpecificStats* getSpecificStats() const final;
    std::vector<DebugPrinter::Block> debugPrint() const final;
    size_t estimateCompileTimeSize() const final;

protected:
    void doSaveState(bool relinquishCursor) final;

private:
    void projectField(value::Object* obj, size_t idx);
    void projectField(UniqueBSONObjBuilder* bob, size_t idx);

    bool isFieldProjectedOrRestricted(const StringMapHashedKey& key) const {
        bool foundKey = false;
        bool projected = false;
        bool restricted = false;

        if (!_allFieldsMap.empty()) {
            if (auto it = _allFieldsMap.find(key); it != _allFieldsMap.end()) {
                foundKey = true;
                projected = it->second != std::numeric_limits<size_t>::max();
                restricted = *_fieldBehavior != FieldBehavior::keep;
            }
        }
        if (!foundKey) {
            restricted = *_fieldBehavior == FieldBehavior::keep;
        }

        return projected || restricted;
    }

    void produceObject();

    const value::SlotId _objSlot;
    const boost::optional<value::SlotId> _rootSlot;
    const boost::optional<FieldBehavior> _fieldBehavior;
    const std::vector<std::string> _fields;
    const std::vector<std::string> _projectFields;
    const value::SlotVector _projectVars;
    const bool _forceNewObject;
    const bool _returnOldObject;

    StringMap<size_t> _allFieldsMap;

    std::vector<std::pair<std::string, value::SlotAccessor*>> _projects;

    value::OwnedValueAccessor _obj;

    value::SlotAccessor* _root{nullptr};

    bool _compiled{false};
};

using MakeObjStage = MakeObjStageBase<MakeObjOutputType::object>;
using MakeBsonObjStage = MakeObjStageBase<MakeObjOutputType::bsonObject>;
}  // namespace mongo::sbe
