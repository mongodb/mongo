/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/stages/sort.h"
#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/db/sorter/sorter_template_defs.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>
#include <vector>

namespace mongo::sbe {

template <typename KeyRow, typename ValueRow>
class SortStage::SortImpl final : public SortIface {
public:
    explicit SortImpl(SortStage& stage) : _stage(stage) {}

    void prepare(CompileCtx& ctx) override {
        _stage._children[0]->prepare(ctx);

        size_t counter = 0;
        // Process order by fields.
        for (auto& slot : _stage._obs) {
            _inKeyAccessors.emplace_back(_stage._children[0]->getAccessor(ctx, slot));
            auto [it, inserted] = _outAccessors.emplace(
                slot,
                std::make_unique<value::MaterializedRowKeyAccessor<SorterData*>>(_outputDataIt,
                                                                                 counter));
            ++counter;
            uassert(4822812, str::stream() << "duplicate field: " << slot, inserted);
        }

        counter = 0;
        // Process value fields.
        for (auto& slot : _stage._vals) {
            _inValueAccessors.emplace_back(_stage._children[0]->getAccessor(ctx, slot));
            auto [it, inserted] = _outAccessors.emplace(
                slot,
                std::make_unique<value::MaterializedRowValueAccessor<SorterData*>>(_outputDataIt,
                                                                                   counter));
            ++counter;
            uassert(4822813, str::stream() << "duplicate field: " << slot, inserted);
        }

        if (_stage._limitExpr) {
            _limitCode = _stage._limitExpr->compile(ctx);
        }

        _stage._memoryTracker = OperationMemoryUsageTracker::createSimpleMemoryUsageTrackerForSBE(
            _stage._opCtx, _stage._specificStats.maxMemoryUsageBytes);
    }

    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) override {
        if (auto it = _outAccessors.find(slot); it != _outAccessors.end()) {
            return it->second.get();
        }

        return ctx.getAccessor(slot);
    }

    void open(bool reOpen) override {
        auto optTimer(_stage.getOptTimer(_stage._opCtx));

        invariant(_stage._opCtx);
        _stage._commonStats.opens++;
        _stage._children[0]->open(reOpen);

        if (_limitCode) {
            _stage._specificStats.limit = _runLimitCode();
        } else {
            _stage._specificStats.limit = std::numeric_limits<size_t>::max();
        }

        _makeSorter();

        while (_stage._children[0]->getNext() == PlanState::ADVANCED) {
            KeyRow keys{_inKeyAccessors.size()};

            size_t idx = 0;
            for (auto accessor : _inKeyAccessors) {
                auto [tag, val] = accessor->getViewOfValue();
                keys.reset(idx++, false, tag, val);
            }

            // Do not allocate the values here, instead let the sorter decide, since the sorter may
            // decide not to store the values in the case of sort-limit.
            _sorter->emplace(std::move(keys), [&]() {
                ValueRow vals{_inValueAccessors.size()};
                size_t idx = 0;
                for (auto accessor : _inValueAccessors) {
                    auto [tag, val] = accessor->getViewOfValue();
                    vals.reset(idx++, false, tag, val);
                }
                return vals;
            });

            _stage._memoryTracker.value().set(_sorter->stats().memUsage());
        }

        _stage._specificStats.peakTrackedMemBytes =
            _stage._memoryTracker.value().peakTrackedMemoryBytes();
        _stage._specificStats.totalDataSizeBytes += _sorter->stats().bytesSorted();
        _outputIt = _sorter->done();
        _stage._specificStats.keysSorted += _sorter->stats().numSorted();
        if (_sorterFileStats) {
            _stage._specificStats.spillingStats.incrementSpills(_sorter->stats().spilledRanges());
            _stage._specificStats.spillingStats.incrementSpilledRecords(
                _sorter->stats().spilledKeyValuePairs());
            _stage._specificStats.spillingStats.incrementSpilledDataStorageSize(
                _sorterFileStats->bytesSpilled());
            _stage._specificStats.spillingStats.incrementSpilledBytes(
                _sorterFileStats->bytesSpilledUncompressed());
        }

        _stage._children[0]->close();
    }

    PlanState getNext() override {
        auto optTimer(_stage.getOptTimer(_stage._opCtx));
        _stage.checkForInterruptAndYield(_stage._opCtx);

        if (_outputIt && _outputIt->more()) {
            _outputData = _outputIt->next();

            return _stage.trackPlanState(PlanState::ADVANCED);
        } else {
            return _stage.trackPlanState(PlanState::IS_EOF);
        }
    }

    void close() override {
        auto optTimer(_stage.getOptTimer(_stage._opCtx));

        _stage.trackClose();
        _outputIt.reset();
        _sorter.reset();
        _stage._specificStats.peakTrackedMemBytes =
            _stage._memoryTracker.value().peakTrackedMemoryBytes();
        _stage._memoryTracker.value().set(0);
    }

    void forceSpill() override {
        if (_outputIt) {
            if (_outputIt->spillable()) {
                auto& spillingStats = _stage._specificStats.spillingStats;
                uint64_t previousSpilledBytes = spillingStats.getSpilledBytes();
                uint64_t previousSpilledDataStorageSize = spillingStats.getSpilledDataStorageSize();

                SorterTracker tracker;
                auto opts = _makeSortOptions();
                opts.Tracker(&tracker);

                _outputIt = _outputIt->spill(opts, typename Sorter<KeyRow, ValueRow>::Settings());
                _stage._memoryTracker.value().set(0);

                spillingStats.incrementSpills(tracker.spilledRanges.loadRelaxed());
                spillingStats.incrementSpilledRecords(tracker.spilledKeyValuePairs.loadRelaxed());
                spillingStats.incrementSpilledBytes(_sorterFileStats->bytesSpilledUncompressed() -
                                                    previousSpilledBytes);
                spillingStats.incrementSpilledDataStorageSize(_sorterFileStats->bytesSpilled() -
                                                              previousSpilledDataStorageSize);
            }
        } else if (_sorter) {
            _sorter->spill();
            _stage._memoryTracker.value().set(_sorter->stats().memUsage());
        }
    }

private:
    using SorterIterator = SortIteratorInterface<KeyRow, ValueRow>;
    using SorterData = std::pair<KeyRow, ValueRow>;

    int64_t _runLimitCode() {
        auto [owned, tag, val] = vm::ByteCode{}.run(_limitCode.get());
        value::ValueGuard guard{owned, tag, val};
        tassert(
            8349205, "Limit code returned unexpected value", tag == value::TypeTags::NumberInt64);
        return value::bitcastTo<size_t>(val);
    }

    SortOptions _makeSortOptions() {
        SortOptions opts;
        opts.MaxMemoryUsageBytes(_stage._specificStats.maxMemoryUsageBytes);
        opts.Limit(_stage._specificStats.limit != std::numeric_limits<size_t>::max()
                       ? _stage._specificStats.limit
                       : 0);
        opts.MoveSortedDataIntoIterator(true);
        if (_stage._allowDiskUse) {
            // TODO SERVER-109634: Use boost::filesystem::path directly when it is supported by
            // SortOptions.
            opts.TempDir((boost::filesystem::path(storageGlobalParams.dbpath) / "_tmp").string());
            if (!_sorterFileStats) {
                _sorterFileStats = std::make_unique<SorterFileStats>(nullptr);
            }
            opts.FileStats(_sorterFileStats.get());
        }
        return opts;
    }

    void _makeSorter() {
        auto opts = _makeSortOptions();

        auto comp = [this](const KeyRow& lhs, const KeyRow& rhs) {
            auto size = lhs.size();
            for (size_t idx = 0; idx < size; ++idx) {
                auto [lhsTag, lhsVal] = lhs.getViewOfValue(idx);
                auto [rhsTag, rhsVal] = rhs.getViewOfValue(idx);
                auto [tag, val] = value::compareValue(lhsTag, lhsVal, rhsTag, rhsVal);
                uassert(7086700, "Invalid comparison result", tag == value::TypeTags::NumberInt32);
                auto result = value::bitcastTo<int32_t>(val);
                if (result) {
                    return _stage._dirs[idx] == value::SortDirection::Descending ? -result : result;
                }
            }

            return 0;
        };

        _sorter = Sorter<KeyRow, ValueRow>::make(opts, comp, {});
        _outputIt.reset();
    }

    SortStage& _stage;
    std::vector<value::SlotAccessor*> _inKeyAccessors;
    std::vector<value::SlotAccessor*> _inValueAccessors;
    value::SlotMap<std::unique_ptr<value::SlotAccessor>> _outAccessors;
    std::unique_ptr<SorterFileStats> _sorterFileStats;
    std::unique_ptr<SorterIterator> _outputIt;
    SorterData _outputData{};
    SorterData* _outputDataIt{&_outputData};
    std::unique_ptr<Sorter<KeyRow, ValueRow>> _sorter;
    std::unique_ptr<vm::CodeFragment> _limitCode;
};

std::unique_ptr<SortStage::SortIface> SortStage::_makeStageImpl() {
    auto pickKeyTypeThen = [&](auto next, auto... args) {
        switch (_obs.size()) {
            case 1:
                return next(args..., std::type_identity<value::FixedSizeRow<1>>{});
            case 2:
                return next(args..., std::type_identity<value::FixedSizeRow<2>>{});
            case 3:
                return next(args..., std::type_identity<value::FixedSizeRow<3>>{});
            default:
                return next(args..., std::type_identity<value::MaterializedRow>{});
        }
    };
    auto pickValueTypeThen = [&](auto next, auto... args) {
        switch (_vals.size()) {
            case 1:
                return next(args..., std::type_identity<value::FixedSizeRow<1>>{});
            default:
                return next(args..., std::type_identity<value::MaterializedRow>{});
        }
    };
    auto makeSortImplWithTypes = [&]<typename... Ts>(std::type_identity<Ts>...) {
        return std::unique_ptr<SortStage::SortIface>{std::make_unique<SortImpl<Ts...>>(*this)};
    };
    return pickKeyTypeThen(pickValueTypeThen, makeSortImplWithTypes);
}

}  // namespace mongo::sbe
