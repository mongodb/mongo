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

#define PEGLIB_USE_STD_ANY 0
#include <third_party/peglib/peglib.h>

#include <stack>

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/ix_scan.h"
#include "mongo/db/exec/sbe/stages/spool.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/query/sbe_stage_builder.h"

namespace mongo {
class PlanYieldPolicy;
namespace sbe {
struct ParsedQueryTree {
    std::string identifier;
    size_t indexKey;
    std::string rename;
    std::vector<std::string> identifiers;
    std::vector<size_t> indexKeys;
    std::vector<std::string> renames;

    std::unique_ptr<PlanStage> stage;
    std::unique_ptr<EExpression> expr;

    // Sorted map for projects. Since by default the map will be sorted by the string, we will get
    // deterministic order as we insert symbol string keys and expression values.
    std::map<std::string, std::unique_ptr<EExpression>> projects;
};

using AstQuery = peg::AstBase<ParsedQueryTree>;

class Parser {
public:
    Parser(RuntimeEnvironment* env);
    std::unique_ptr<PlanStage> parse(OperationContext* opCtx,
                                     StringData defaultDb,
                                     StringData line,
                                     PlanYieldPolicy* yieldPolicy = nullptr);

    std::pair<boost::optional<value::SlotId>, boost::optional<value::SlotId>> getTopLevelSlots()
        const {
        return {_resultSlot, _recordIdSlot};
    }

private:
    using SymbolTable = std::map<std::string, value::SlotId>;
    using SpoolBufferLookupTable = std::map<std::string, SpoolId>;
    peg::parser _parser;
    OperationContext* _opCtx{nullptr};
    PlanYieldPolicy* _yieldPolicy{nullptr};
    std::string _defaultDb;
    SymbolTable _symbolsLookupTable;
    SpoolBufferLookupTable _spoolBuffersLookupTable;
    value::SlotIdGenerator _slotIdGenerator;
    value::SpoolIdGenerator _spoolIdGenerator;
    FrameId _frameId{0};
    RuntimeEnvironment* _env;
    struct FrameSymbolTable {
        FrameId id;
        SymbolTable table;
    };
    struct FrameSymbol {
        FrameId id;
        value::SlotId slotId;
    };
    std::vector<std::unique_ptr<FrameSymbolTable>> _frameLookupTable;
    boost::optional<value::SlotId> _resultSlot;
    boost::optional<value::SlotId> _recordIdSlot;
    std::stack<PlanNodeId> planNodeIdStack;

    FrameSymbolTable* newFrameSymbolTable() {
        auto table = std::make_unique<FrameSymbolTable>();
        table->id = ++_frameId;

        _frameLookupTable.emplace_back(std::move(table));

        return _frameLookupTable.back().get();
    }
    FrameSymbolTable* currentFrameSymbolTable() {
        return _frameLookupTable.back().get();
    }
    void popFrameSymbolTable() {
        _frameLookupTable.pop_back();
    }

    boost::optional<FrameSymbol> lookupSymbol(const std::string& name) {
        for (size_t idx = _frameLookupTable.size(); idx-- > 0;) {
            if (auto it = _frameLookupTable[idx]->table.find(name);
                it != _frameLookupTable[idx]->table.end()) {
                return FrameSymbol{_frameLookupTable[idx]->id, it->second};
            }
        }

        return boost::none;
    }
    boost::optional<value::SlotId> lookupSlot(const std::string& name) {
        if (name.empty() || name == DebugPrinter::kNoneKeyword) {
            return boost::none;
        } else if (_symbolsLookupTable.find(name) == _symbolsLookupTable.end()) {
            _symbolsLookupTable[name] = _slotIdGenerator.generate();

            if (name == "$$RESULT") {
                _resultSlot = _symbolsLookupTable[name];
            } else if (name == "$$RID") {
                _recordIdSlot = _symbolsLookupTable[name];
            }
        }
        return _symbolsLookupTable[name];
    }

    value::SlotId lookupSlotStrict(const std::string& name) {
        auto slot = lookupSlot(name);
        uassert(4885906, str::stream() << "Unable lookup SlotId for [" << name << "]", slot);
        return *slot;
    }

    value::SlotVector lookupSlots(const std::vector<std::string>& names) {
        value::SlotVector result;
        std::transform(names.begin(),
                       names.end(),
                       std::back_inserter(result),
                       [this](const auto& name) { return lookupSlotStrict(name); });
        return result;
    }

    template <typename T>
    sbe::value::SlotMap<T> lookupSlots(std::map<std::string, T> indentifiers) {
        sbe::value::SlotMap<T> result;
        for (auto&& [name, value] : indentifiers) {
            result[lookupSlotStrict(name)] = std::move(value);
        }
        return result;
    }

    std::pair<IndexKeysInclusionSet, sbe::value::SlotVector> lookupIndexKeyRenames(
        const std::vector<std::string>& renames, const std::vector<size_t>& indexKeys);

    SpoolId lookupSpoolBuffer(const std::string& name) {
        if (_spoolBuffersLookupTable.find(name) == _spoolBuffersLookupTable.end()) {
            _spoolBuffersLookupTable[name] = _spoolIdGenerator.generate();
        }
        return _spoolBuffersLookupTable[name];
    }

    void walkChildren(AstQuery& ast);
    void walkIdent(AstQuery& ast);
    void walkIdentList(AstQuery& ast);
    void walkIdentWithRename(AstQuery& ast);
    void walkIdentListWithRename(AstQuery& ast);
    void walkIxKeyWithRename(AstQuery& ast);
    void walkIxKeyListWithRename(AstQuery& ast);

    void walkProjectList(AstQuery& ast);
    void walkAssign(AstQuery& ast);
    void walkExpr(AstQuery& ast);
    void walkEqopExpr(AstQuery& ast);
    void walkRelopExpr(AstQuery& ast);
    void walkAddExpr(AstQuery& ast);
    void walkMulExpr(AstQuery& ast);
    void walkPrimaryExpr(AstQuery& ast);
    void walkIfExpr(AstQuery& ast);
    void walkLetExpr(AstQuery& ast);
    void walkFrameProjectList(AstQuery& ast);
    void walkFunCall(AstQuery& ast);
    void walkUnionBranch(AstQuery& ast);
    void walkSortedMergeBranch(AstQuery& ast);
    void walkSortDirList(AstQuery& ast);

    void walkScan(AstQuery& ast);
    void walkParallelScan(AstQuery& ast);
    void walkSeek(AstQuery& ast);
    void walkIndexScan(AstQuery& ast);
    void walkIndexSeek(AstQuery& ast);
    void walkProject(AstQuery& ast);
    void walkFilter(AstQuery& ast);
    void walkCFilter(AstQuery& ast);
    void walkSort(AstQuery& ast);
    void walkUnion(AstQuery& ast);
    void walkUnwind(AstQuery& ast);
    void walkMkObj(AstQuery& ast);
    void walkGroup(AstQuery& ast);
    void walkHashJoin(AstQuery& ast);
    void walkNLJoin(AstQuery& ast);
    void walkLimit(AstQuery& ast);
    void walkSkip(AstQuery& ast);
    void walkCoScan(AstQuery& ast);
    void walkTraverse(AstQuery& ast);
    void walkExchange(AstQuery& ast);
    void walkBranch(AstQuery& ast);
    void walkSimpleProj(AstQuery& ast);
    void walkPFO(AstQuery& ast);
    void walkLazyProducerSpool(AstQuery& ast);
    void walkEagerProducerSpool(AstQuery& ast);
    void walkConsumerSpool(AstQuery& ast);
    void walkStackConsumerSpool(AstQuery& ast);
    void walkPlanNodeId(AstQuery& ast);
    void walkUnique(AstQuery& ast);
    void walkSortedMerge(AstQuery& ast);

    void walk(AstQuery& ast);

    std::unique_ptr<PlanStage> walkPath(AstQuery& ast,
                                        value::SlotId inputSlot,
                                        value::SlotId outputSlot);
    std::unique_ptr<PlanStage> walkPathValue(AstQuery& ast,
                                             value::SlotId inputSlot,
                                             std::unique_ptr<PlanStage> inputStage,
                                             value::SlotVector correlated,
                                             value::SlotId outputSlot);

    CollectionUUID getCollectionUuid(const std::string& collName);

    PlanNodeId getCurrentPlanNodeId();
};
}  // namespace sbe
}  // namespace mongo
