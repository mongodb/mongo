/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/query/stage_types.h"

namespace mongo::sbe {
struct CommonStats {
    CommonStats() = delete;

    CommonStats(StringData stageType, PlanNodeId nodeId) : stageType{stageType}, nodeId{nodeId} {}

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }

    const StringData stageType;

    // An identifier for the node, or zero if the idenfier was not provided. Useful for displaying
    // debug output such as explain.
    //
    // These identifiers are not necessarily unique. For example, they can be used by code
    // constructing the SBE plan to construct groups of nodes with the same id, e.g. if a group of
    // PlanStages corresponds to an MQL operation specified by the user.
    const PlanNodeId nodeId;

    // Time elapsed while working inside this stage. When this field is set to boost::none,
    // timing info will not be collected during query execution.
    //
    // The field must be populated when running explain or when running with the profiler on. It
    // must also be populated when multi planning, in order to gather stats stored in the plan
    // cache.
    boost::optional<long long> executionTimeMillis;

    size_t advances{0};
    size_t opens{0};
    size_t closes{0};
    size_t yields{0};
    size_t unyields{0};
    bool isEOF{false};
};

using PlanStageStats = BasePlanStageStats<CommonStats>;

struct ScanStats final : public SpecificStats {
    SpecificStats* clone() const final {
        return new ScanStats(*this);
    }

    uint64_t estimateObjectSizeInBytes() const final {
        return sizeof(*this);
    }

    void accumulate(PlanSummaryStats& stats) const final {
        stats.totalDocsExamined += numReads;
    }

    size_t numReads{0};
};

struct IndexScanStats final : public SpecificStats {
    SpecificStats* clone() const final {
        return new IndexScanStats(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }

    void accumulate(PlanSummaryStats& stats) const final {
        stats.totalKeysExamined += numReads;
    }

    size_t numReads{0};
    size_t seeks{0};
};

struct FilterStats final : public SpecificStats {
    SpecificStats* clone() const final {
        return new FilterStats(*this);
    }

    uint64_t estimateObjectSizeInBytes() const final {
        return sizeof(*this);
    }

    size_t numTested{0};
};

struct LimitSkipStats final : public SpecificStats {
    SpecificStats* clone() const final {
        return new LimitSkipStats(*this);
    }

    uint64_t estimateObjectSizeInBytes() const final {
        return sizeof(*this);
    }

    boost::optional<long long> limit;
    boost::optional<long long> skip;
};

struct UniqueStats : public SpecificStats {
    SpecificStats* clone() const final {
        return new UniqueStats(*this);
    }

    uint64_t estimateObjectSizeInBytes() const final {
        return sizeof(*this);
    }

    size_t dupsTested = 0;
    size_t dupsDropped = 0;
};

struct BranchStats final : public SpecificStats {
    SpecificStats* clone() const final {
        return new BranchStats(*this);
    }

    uint64_t estimateObjectSizeInBytes() const final {
        return sizeof(*this);
    }

    size_t numTested{0};
    size_t thenBranchOpens{0};
    size_t thenBranchCloses{0};
    size_t elseBranchOpens{0};
    size_t elseBranchCloses{0};
};

struct CheckBoundsStats final : public SpecificStats {
    SpecificStats* clone() const final {
        return new CheckBoundsStats(*this);
    }

    uint64_t estimateObjectSizeInBytes() const final {
        return sizeof(*this);
    }

    size_t seeks{0};
};

struct LoopJoinStats final : public SpecificStats {
    SpecificStats* clone() const final {
        return new LoopJoinStats(*this);
    }

    uint64_t estimateObjectSizeInBytes() const final {
        return sizeof(*this);
    }

    size_t innerOpens{0};
    size_t innerCloses{0};
};

struct TraverseStats : public SpecificStats {
    SpecificStats* clone() const final {
        return new TraverseStats(*this);
    }

    uint64_t estimateObjectSizeInBytes() const final {
        return sizeof(*this);
    }

    size_t innerOpens{0};
    size_t innerCloses{0};
};

/**
 * Calculates the total number of physical reads in the given plan stats tree. If a stage can do
 * a physical read (e.g. COLLSCAN or IXSCAN), then its 'numReads' stats is added to the total.
 */
size_t calculateNumberOfReads(const PlanStageStats* root);
}  // namespace mongo::sbe
