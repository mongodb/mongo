import {PipelineWorkload} from "jstests/product_limits/libs/workload.js";

const LOOKUP_AS = "lookupJoin";

export class WorkloadLookupJoinOverManyFields extends PipelineWorkload {
    /** $lookup with correlated $expr join over many fields. */

    scale() {
        // TODO SERVER-127331: $lookup + with many let fields has almost cubic optimization complexity.
        return 100;
    }

    pipeline() {
        const letVars = {};
        const eqExprs = [];

        for (let i = 0; i < this.scale(); i++) {
            const field = `f${i}`;
            letVars[field] = `$${field}`;
            eqExprs.push({$eq: [`$${field}`, `$$${field}`]});
        }

        return [
            {
                $lookup: {
                    from: this.collection(),
                    let: letVars,
                    pipeline: [{$match: {$expr: {$and: eqExprs}}}],
                    as: LOOKUP_AS,
                },
            },
            {$unwind: `$${LOOKUP_AS}`},
            {$replaceRoot: {newRoot: `$${LOOKUP_AS}`}},
            {$unset: "_id"},
        ];
    }
}
