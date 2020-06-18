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
class MakeObjStage final : public PlanStage {
public:
    MakeObjStage(std::unique_ptr<PlanStage> input,
                 value::SlotId objSlot,
                 boost::optional<value::SlotId> rootSlot,
                 std::vector<std::string> restrictFields,
                 std::vector<std::string> projectFields,
                 value::SlotVector projectVars,
                 bool forceNewObject,
                 bool returnOldObject);

    std::unique_ptr<PlanStage> clone() const final;

    void prepare(CompileCtx& ctx) final;
    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;
    void open(bool reOpen) final;
    PlanState getNext() final;
    void close() final;

    std::unique_ptr<PlanStageStats> getStats() const final;
    const SpecificStats* getSpecificStats() const final;
    std::vector<DebugPrinter::Block> debugPrint() const final;

private:
    void projectField(value::Object* obj, size_t idx);

    bool isFieldRestricted(const std::string_view& sv) const {
        return _restrictAllFields || _restrictFieldsSet.count(sv) != 0;
    }

    const value::SlotId _objSlot;
    const boost::optional<value::SlotId> _rootSlot;
    const std::vector<std::string> _restrictFields;
    const std::vector<std::string> _projectFields;
    const value::SlotVector _projectVars;
    const bool _forceNewObject;
    const bool _returnOldObject;

    absl::flat_hash_set<std::string> _restrictFieldsSet;
    absl::flat_hash_map<std::string, size_t> _projectFieldsMap;

    std::vector<std::pair<std::string, value::SlotAccessor*>> _projects;

    value::OwnedValueAccessor _obj;

    value::SlotAccessor* _root{nullptr};

    bool _compiled{false};
    bool _restrictAllFields{false};
};
}  // namespace mongo::sbe
