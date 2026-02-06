// Duplicate tests from match.js and prepend $limit to the front to make a non-leading $match.
import {range} from "jstests/product_limits/libs/util.js";
import * as MatchWorkloads from "jstests/product_limits/libs/match.js";
import * as GroupingWorkloads from "jstests/product_limits/libs/grouping.js";

export class WorkloadAndOverSingleField extends MatchWorkloads.WorkloadAndOverSingleField {
    pipeline() {
        return [{$sort: {f0: 1}}, {$limit: 20}, {$match: {f0: {$gte: 0, $lt: 20}}}, ...super.pipeline()];
    }

    result() {
        return range(Math.min(20, this.scale())).map((i) => ({f0: i}));
    }
}

export class WorkloadOrOverSingleField extends MatchWorkloads.WorkloadOrOverSingleField {
    pipeline(dataset) {
        return [{$sort: {f0: 1}}, {$limit: 20}, ...super.pipeline(dataset)];
    }
    result() {
        return range(Math.min(20, this.scale())).map((i) => ({f0: i}));
    }
}

export class WorkloadAndPlusOrOverSingleField extends MatchWorkloads.WorkloadAndPlusOrOverSingleField {
    pipeline() {
        return [{$sort: {f0: 1}}, {$limit: 20}, ...super.pipeline()];
    }
    result() {
        return range(Math.min(20, this.scale())).map((i) => ({f0: i}));
    }
}

export class WorkloadOrPlusAndOverSingleField extends MatchWorkloads.WorkloadOrPlusAndOverSingleField {
    pipeline() {
        return [{$sort: {f0: 1}}, {$limit: 20}, ...super.pipeline()];
    }
    result() {
        return range(Math.min(20, this.scale())).map((i) => ({f0: i}));
    }
}

export class WorkloadAndOverManyFields extends MatchWorkloads.WorkloadAndOverManyFields {
    pipeline() {
        return [{$limit: 20}, ...super.pipeline()];
    }
}

export class WorkloadOrOverManyFields extends MatchWorkloads.WorkloadOrOverManyFields {
    pipeline() {
        return [{$limit: 20}, ...super.pipeline()];
    }
}

export class WorkloadAndPlusOrOverManyFields extends MatchWorkloads.WorkloadAndPlusOrOverManyFields {
    pipeline() {
        return [{$limit: 20}, ...super.pipeline()];
    }
}

export class WorkloadOrPlusAndOverManyFields extends MatchWorkloads.WorkloadOrPlusAndOverManyFields {
    pipeline() {
        return [{$limit: 20}, ...super.pipeline()];
    }
}

export class WorkloadMatchOverManyFields extends MatchWorkloads.WorkloadMatchOverManyFields {
    /** $match with individual equality conditions over many fields. */
    pipeline() {
        return [{$limit: 20}, ...super.pipeline()];
    }
}
export class WorkloadMatchLongPath extends MatchWorkloads.WorkloadMatchLongPath {
    pipeline() {
        return [{$limit: 20}, ...super.pipeline()];
    }

    result() {
        return [{"cnt": 1}];
    }
}

export class WorkloadIn extends MatchWorkloads.WorkloadIn {
    /** $in */
    pipeline() {
        return [{$sort: {f0: 1}}, {$limit: 20}, ...super.pipeline()];
    }
    result() {
        return range(Math.min(20, this.scale())).map((i) => ({f0: i}));
    }
}

export class WorkloadNin extends MatchWorkloads.WorkloadNin {
    /** $nin */
    pipeline() {
        return [{$sort: {f0: 1}}, {$limit: 20}, ...super.pipeline()];
    }
    result() {
        return range(Math.min(20, this.scale())).map((i) => ({f0: i}));
    }
}
export class WorkloadManyIns extends MatchWorkloads.WorkloadManyIns {
    /** Many individual $in */
    pipeline() {
        return [{$sort: {f0: 1}}, {$limit: 20}, ...super.pipeline()];
    }
    result() {
        return range(Math.min(20, this.scale())).map((i) => ({f0: i}));
    }
}

export class WorkloadRegexInIn extends MatchWorkloads.WorkloadRegexInIn {
    /** Multiple regexps in an $in */
    pipeline() {
        return [
            {$addFields: {f0Int: {$toInt: "$f0"}}},
            {$sort: {f0Int: 1}},
            {$limit: 20},
            {$project: {f0Int: 0}},
            ...super.pipeline(),
        ];
    }
    result() {
        return range(Math.min(20, this.scale())).map((i) => ({f0: `${i}`}));
    }
}

export class WorkloadRegex extends MatchWorkloads.WorkloadRegex {
    pipeline() {
        return [
            {$addFields: {f0Int: {$toInt: "$f0"}}},
            {$sort: {f0Int: 1}},
            {$limit: 20},
            {$project: {f0Int: 0}},
            ...super.pipeline(),
        ];
    }
    result() {
        return range(Math.min(20, this.scale())).map((i) => ({f0: `${i}`}));
    }
}

export class WorkloadExists extends MatchWorkloads.WorkloadExists {
    pipeline() {
        return [{$limit: 20}, ...super.pipeline()];
    }

    result() {
        return [{"cnt": 1}];
    }
}

export class WorkloadLongValue extends MatchWorkloads.WorkloadLongValue {
    pipeline(dataset) {
        return [{$limit: 20}, ...super.pipeline(dataset)];
    }
}

export class WorkloadManyAccumulatorsManyFields extends GroupingWorkloads.WorkloadManyAccumulatorsManyFields {
    pipeline() {
        return [...super.pipeline(), {$match: {f0: {$lt: this.scale()}}}];
    }
}

export class WorkloadBucketAutoManyBuckets extends GroupingWorkloads.WorkloadBucketAutoManyBuckets {
    pipeline() {
        return [...super.pipeline(), {$match: {count: 1}}];
    }
}

export class WorkloadTopK extends GroupingWorkloads.WorkloadTopK {
    pipeline() {
        return [...super.pipeline(), {$match: {minN: {$gte: 0}}}];
    }
}
