import {PipelineWorkload} from "jstests/product_limits/libs/workload.js";

export class WorkloadConcat extends PipelineWorkload {
    /** $concat */
    pipeline() {
        let concat = [];

        for (let i = 0; i < this.scale(); i++) {
            concat.push({$toString: `$f${i}`});
        }
        return [{$project: {"concat": {$concat: concat}, _id: 0}}];
    }

    result() {
        let concat = "";
        for (let i = 0; i < this.scale(); i++) {
            concat = concat + i;
        }
        return [{concat: concat}];
    }
}

export class WorkloadSwitch extends PipelineWorkload {
    /**
     * $switch with many branches. We explicitly generate conditions that
     * are false in order to cause all branches to be attempted.
     */
    scale() {
        // SERVER-96119 SBE: Stack overflow with many conditions to a $match, index
        return Math.min(1000, super.scale());
    }

    pipeline() {
        let branches = [];

        for (let i = 0; i < this.scale(); i++) {
            branches.push({case: {$ne: [`$f${i}`, i]}, then: i});
        }

        return [
            {$project: {"result": {$switch: {branches: branches, default: "no match"}}}},
            {$unset: "_id"}
        ];
    }

    result() {
        return [{"result": "no match"}];
    }
}

export class WorkloadCond extends PipelineWorkload {
    /**
     * $cond with many levels of nesting
     */
    scale() {
        return Math.min(70, super.scale());  // Exceeded depth limit of 150 when converting js

        // object to BSON. Do you have a cycle?
    }
    pipeline() {
        let cond = "match";

        for (let i = 0; i < this.scale(); i++) {
            cond = {$cond: {if: {$eq: [`$f${i}`, i]}, then: cond, else: "no match"}};
        }

        return [{$project: {"result": cond}}, {$unset: "_id"}];
    }

    result() {
        return [{"result": "match"}];
    }
}

export class WorkloadRange extends PipelineWorkload {
    scale() {
        return 5000000;
    }
    pipeline() {
        return [{$project: {_id: 0, range: {$size: {$range: [0, this.scale()]}}}}];
    }

    result() {
        return [{"range": this.scale()}];
    }
}
