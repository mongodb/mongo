// Test the scenarios from match.js but place the $match inside a $lookup subpipeline
import * as MatchWorkloads from "jstests/product_limits/libs/match.js";

const LOOKUP_AS = "lookupMatch";

/**
 * Rewrites a match.js pipeline so its $match runs inside a $lookup subpipeline.
 */
function wrapMatchInLookupUnwind(workload, stages) {
    const matchIdx = stages.findIndex((stage) => stage.hasOwnProperty("$match"));
    assert.gte(matchIdx, 0, "Expected a $match stage");

    const matchStage = stages[matchIdx];
    const afterMatch = stages.slice(matchIdx + 1);

    const lookup = {
        from: workload.collection(),
        localField: "f0",
        foreignField: "f0",
        pipeline: [matchStage],
        as: LOOKUP_AS,
    };

    const wrapped = [{$lookup: lookup}, {$unwind: `$${LOOKUP_AS}`}];
    const hasCount = afterMatch.some((stage) => stage.hasOwnProperty("$count"));
    if (!hasCount) {
        wrapped.push({$replaceRoot: {newRoot: `$${LOOKUP_AS}`}});
    }
    return wrapped.concat(afterMatch);
}

export class WorkloadAndOverSingleField extends MatchWorkloads.WorkloadAndOverSingleField {
    pipeline(dataset) {
        return wrapMatchInLookupUnwind(this, super.pipeline(dataset));
    }
}

export class WorkloadOrOverSingleField extends MatchWorkloads.WorkloadOrOverSingleField {
    pipeline(dataset) {
        return wrapMatchInLookupUnwind(this, super.pipeline(dataset));
    }
}

export class WorkloadAndPlusOrOverSingleField extends MatchWorkloads.WorkloadAndPlusOrOverSingleField {
    pipeline(dataset) {
        return wrapMatchInLookupUnwind(this, super.pipeline(dataset));
    }
}

export class WorkloadOrPlusAndOverSingleField extends MatchWorkloads.WorkloadOrPlusAndOverSingleField {
    pipeline(dataset) {
        return wrapMatchInLookupUnwind(this, super.pipeline(dataset));
    }
}

export class WorkloadAndOverManyFields extends MatchWorkloads.WorkloadAndOverManyFields {
    pipeline(dataset) {
        return wrapMatchInLookupUnwind(this, super.pipeline(dataset));
    }
}

export class WorkloadOrOverManyFields extends MatchWorkloads.WorkloadOrOverManyFields {
    pipeline(dataset) {
        return wrapMatchInLookupUnwind(this, super.pipeline(dataset));
    }
}

export class WorkloadAndPlusOrOverManyFields extends MatchWorkloads.WorkloadAndPlusOrOverManyFields {
    pipeline(dataset) {
        return wrapMatchInLookupUnwind(this, super.pipeline(dataset));
    }
}

export class WorkloadOrPlusAndOverManyFields extends MatchWorkloads.WorkloadOrPlusAndOverManyFields {
    pipeline(dataset) {
        return wrapMatchInLookupUnwind(this, super.pipeline(dataset));
    }
}

export class WorkloadMatchOverManyFields extends MatchWorkloads.WorkloadMatchOverManyFields {
    pipeline(dataset) {
        return wrapMatchInLookupUnwind(this, super.pipeline(dataset));
    }
}

export class WorkloadMatchLongPath extends MatchWorkloads.WorkloadMatchLongPath {
    pipeline(dataset) {
        return wrapMatchInLookupUnwind(this, super.pipeline(dataset));
    }
}

export class WorkloadIn extends MatchWorkloads.WorkloadIn {
    pipeline(dataset) {
        return wrapMatchInLookupUnwind(this, super.pipeline(dataset));
    }
}

export class WorkloadNin extends MatchWorkloads.WorkloadNin {
    pipeline(dataset) {
        return wrapMatchInLookupUnwind(this, super.pipeline(dataset));
    }
}

export class WorkloadManyIns extends MatchWorkloads.WorkloadManyIns {
    pipeline(dataset) {
        return wrapMatchInLookupUnwind(this, super.pipeline(dataset));
    }
}

export class WorkloadExists extends MatchWorkloads.WorkloadExists {
    pipeline(dataset) {
        return wrapMatchInLookupUnwind(this, super.pipeline(dataset));
    }
}
