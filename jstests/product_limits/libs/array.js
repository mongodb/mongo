import {range} from "jstests/product_limits/libs/util.js";
import {PipelineWorkload} from "jstests/product_limits/libs/workload.js";

export class WorkloadMatchArrayManyConditions extends PipelineWorkload {
    /** $match with many conditions over one array field */
    pipeline() {
        let match = [];

        for (let i = 0; i < this.scale(); i++) {
            match.push({f0: {$eq: i}});
        }

        return [{$match: {$and: match}}, {$unset: "_id"}];
    }
}

export class WorkloadMatchArrayExact extends PipelineWorkload {
    pipeline() {
        return [{$match: {f0: range(this.scale())}}, {$unset: "_id"}];
    }
}

export class WorkloadMatchArrayIndexPosition extends PipelineWorkload {
    pipeline() {
        let match = {};

        for (let i = 0; i < this.scale(); i++) {
            match[`f0.${i}`] = i;
        }

        return [{$match: match}, {$unset: "_id"}];
    }
}

export class WorkloadAllElementsTrue extends PipelineWorkload {
    pipeline() {
        return [{$project: {"allElementsTrue": {$allElementsTrue: "$f0"}}}, {$count: "cnt"}];
    }

    result() {
        return [{"cnt": 1}];
    }
}

export class WorkloadAnyElementTrue extends PipelineWorkload {
    pipeline() {
        return [{$project: {"anyElementTrue": {$anyElementTrue: "$f0"}}}, {$count: "cnt"}];
    }

    result() {
        return [{"cnt": 1}];
    }
}

export class WorkloadArrayToObject extends PipelineWorkload {
    pipeline() {
        return [{$project: {"arrayToObject": {$zip: {inputs: ["$f0", "$f0"]}}}}, {$count: "cnt"}];
    }

    result() {
        return [{"cnt": 1}];
    }
}

export class WorkloadConcatArrays extends PipelineWorkload {
    pipeline() {
        let arrayList = range(this.scale()).map((i) => [`$f$i`]);
        return [{$project: {"size": {$size: [{$concatArrays: arrayList}]}}}, {$unset: "_id"}];
    }

    result() {
        return [{"size": this.scale()}];
    }
}

export class WorkloadFilter extends PipelineWorkload {
    pipeline() {
        return [
            {$project: {"f0": {$filter: {input: "$f0", as: "f0", cond: {$gte: ["$$f0", 0]}}}}},
            {$count: "cnt"}
        ];
    }

    result() {
        return [{"cnt": 1}];
    }
}

export class WorkloadElemMatchGte extends PipelineWorkload {
    /** $elemMatchEq */
    pipeline() {
        return [{$match: {f0: {$elemMatch: {$gte: 0}}}}, {$count: "cnt"}];
    }

    result() {
        return [{"cnt": 1}];
    }
}

export class WorkloadIndexOfArray extends PipelineWorkload {
    pipeline() {
        return [
            {$project: {"indexOfArray": {$indexOfArray: ["$f0", this.scale() - 1]}}},
            {$unset: "_id"}
        ];
    }

    result() {
        return [{indexOfArray: this.scale() - 1}];
    }
}

export class WorkloadReverseArray extends PipelineWorkload {
    pipeline() {
        return [{$project: {"reverseArray": {$reverseArray: "$f0"}}}, {$count: "cnt"}];
    }

    result() {
        return [{cnt: 1}];
    }
}

export class WorkloadSetDifference extends PipelineWorkload {
    pipeline() {
        return [{$project: {"setDifference": {$setDifference: ["$f0", "$f0"]}}}, {$unset: "_id"}];
    }

    result() {
        return [{setDifference: []}];
    }
}

export class WorkloadSetIntersection extends PipelineWorkload {
    pipeline() {
        return [
            {$project: {"setIntersection": {$size: {$setIntersection: ["$f0", "$f0"]}}}},
            {$unset: "_id"}
        ];
    }

    result() {
        return [{setIntersection: this.scale()}];
    }
}

export class WorkloadSetEquals extends PipelineWorkload {
    pipeline() {
        return [{$project: {"setEquals": {$setEquals: ["$f0", "$f0"]}}}, {$unset: "_id"}];
    }

    result() {
        return [{setEquals: true}];
    }
}

export class WorkloadZipArrayFields extends PipelineWorkload {
    pipeline() {
        return [
            {"$project": {"zip": {"$size": {"$zip": {"inputs": ["$f0", "$f0"]}}}}},
            {$unset: "_id"}
        ];
    }

    result() {
        return [{"zip": this.scale()}];
    }
}

export class WorkloadMap extends PipelineWorkload {
    pipeline() {
        return [
            {
                "$project":
                    {"map": {"$size": {"$map": {input: "$f0", as: "f", in : {$add: ["$$f", 1]}}}}}
            },
            {$unset: "_id"}
        ];
    }

    result() {
        return [{"map": this.scale()}];
    }
}

export class WorkloadReduce extends PipelineWorkload {
    pipeline() {
        return [
            {
                "$project": {
                    "reduce": {
                        "$reduce":
                            {input: "$f0", initialValue: 0, in : {$max: ["$$value", "$$this"]}}
                    }
                }
            },
            {$unset: "_id"}
        ];
    }

    result() {
        return [{"reduce": this.scale() - 1}];
    }
}

export class WorkloadZipManyArrays extends PipelineWorkload {
    pipeline() {
        let zipList = [];

        for (let i = 0; i < this.scale(); i++) {
            zipList.push([`$f${i}`]);
        }
        return [{$project: {"zip": {$zip: {inputs: zipList}}}}, {$unset: "_id"}];
    }

    result() {
        return [{"zip": [range(this.scale())]}];
    }
}

export class WorkloadAddToSet extends PipelineWorkload {
    pipeline() {
        return [
            {"$group": {"_id": null, "f": {$addToSet: "$f0"}}},
            {$project: {_id: 0, f: {$sortArray: {input: "$f", sortBy: 1}}}}
        ];
    }

    result() {
        return [{f: range(this.scale())}];
    }
}
export class WorkloadInOverArrayField extends PipelineWorkload {
    pipeline() {
        return [{$match: {f0: {$in: range(this.scale())}}}, {$count: 'cnt'}];
    }

    result() {
        // 1 row in this dataset
        return [{cnt: 1}];
    }
}

export class WorkloadAll extends PipelineWorkload {
    /** $all */
    pipeline() {
        return [{$match: {f0: {$all: range(this.scale())}}}, {$count: "cnt"}];
    }

    result() {
        return [{"cnt": 1}];
    }
}
