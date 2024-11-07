import {range} from "./jstests/product_limits/libs/util.js";
import {PipelineWorkload} from "./jstests/product_limits/libs/workload.js";

export class WorkloadManyAccumulatorsSameField extends PipelineWorkload {
    /** Many accumulators in a single $group stage */
    pipeline() {
        let accumulators = {};

        for (let i = 0; i < this.scale(); i++) {
            accumulators[`f${i}`] = {$max: {$add: ["$f0", i]}};
        }
        accumulators['_id'] = null;
        return [{$group: accumulators}];
    }

    result() {
        let row = {"_id": null};
        for (let i = 0; i < this.scale(); i++) {
            row[`f${i}`] = this.scale() + i - 1;
        }
        return [row];
    }
}

export class WorkloadManyGroupingFields extends PipelineWorkload {
    /** Many fields in the _id argument of $group */
    pipeline(dataset) {
        let key = dataset.field_list();
        return [{$group: {_id: key, f0: {$max: 0}}}];
    }

    result(dataset) {
        let key = dataset.value_list();
        return [{"_id": key, f0: 0}];
    }
}

export class WorkloadManyAccumulatorsManyFields extends PipelineWorkload {
    /** Many accumulators over distinct fields in a single $group stage */
    pipeline() {
        let accumulators = {};

        for (let i = 0; i < this.scale(); i++) {
            accumulators[`f${i}`] = {$max: `$f${i}`};
        }
        accumulators['_id'] = null;
        return [{$group: accumulators}];
    }

    result() {
        let row = {"_id": null};
        for (let i = 0; i < this.scale(); i++) {
            row[`f${i}`] = i;
        }
        return [row];
    }
}

export class WorkloadBucketManyBoundaries extends PipelineWorkload {
    /** Many boundaries in a single $bucket stage */
    scale() {
        // SERVER-95977 Stack overflow with many boundaries in $bucket
        return Math.min(1000, super.scale());
    }
    pipeline() {
        let boundaries = range(this.scale() + 1);
        return [{
            $bucket: {
                groupBy: "$f0",
                boundaries: boundaries,
                default: "default",
                output: {"count": {$sum: 1}}
            }
        }];
    }

    result(dataset) {
        let result = [];
        for (let i = 0; i < this.scale(); i++) {
            result.push({_id: i, count: 1});
        }
        if (this.scale() < dataset.scale()) {
            // The default bucket will collect all values above the largest boundary
            result.push({_id: "default", count: dataset.scale() - this.scale()});
        }
        return result;
    }
}

export class WorkloadBucketManyOutputs extends PipelineWorkload {
    /** Many outputs in a single $bucket stage */
    pipeline() {
        let outputs = {};

        for (let i = 0; i < this.scale(); i++) {
            outputs[`f${i}`] = {$min: `$f${i}`};
        }
        return [
            {$bucket: {groupBy: "$f0", boundaries: [0, 1], default: "default", output: outputs}}
        ];
    }

    result() {
        let row = {"_id": 0};

        for (let i = 0; i < this.scale(); i++) {
            row[`f${i}`] = i;
        }
        return [row];
    }
}

export class WorkloadBucketAutoManyBuckets extends PipelineWorkload {
    /** Many buckets in a single $bucketAuto stage */
    pipeline() {
        return [
            {$bucketAuto: {groupBy: "$f0", buckets: this.scale(), output: {"count": {$sum: 1}}}}
        ];
    }

    result() {
        let result = [];
        for (let i = 0; i < this.scale(); i++) {
            if (i == this.scale() - 1) {
                result.push({_id: {min: i, max: i}, count: 1});

            } else {
                result.push({_id: {min: i, max: i + 1}, count: 1});
            }
        }
        return result;
    }
}

export class WorkloadBucketAutoManyOutputs extends PipelineWorkload {
    /** Many outputs in a single $bucketAuto stage */
    pipeline() {
        let outputs = {};

        for (let i = 0; i < this.scale(); i++) {
            outputs[`f${i}`] = {$min: `$f${i}`};
        }
        return [{$bucketAuto: {groupBy: "$f0", buckets: 1, output: outputs}}];
    }

    result() {
        let row = {"_id": {min: 0, max: 0}};

        for (let i = 0; i < this.scale(); i++) {
            row[`f${i}`] = i;
        }
        return [row];
    }
}

export class WorkloadSetWindowFieldsManyPartitions extends PipelineWorkload {
    pipeline() {
        let partitions = [];

        for (let i = 0; i < this.scale(); i++) {
            partitions.push({$toString: `$f${i}`});
        }
        return [
            {$setWindowFields: {partitionBy: {$concat: partitions}, output: {"f0": {$max: "$f0"}}}},
            {$unset: "_id"}
        ];
    }

    result() {
        let row = {"f0": this.scale() - 1};
        let result = [];
        for (let i = 0; i < this.scale(); i++) {
            result.push(row);
        }
        return result;
    }
}

export class WorkloadSetWindowFieldsManySortBy extends PipelineWorkload {
    scale() {
        // "too many compound keys"
        return 32;
    }
    pipeline() {
        let sortByFields = {};

        for (let i = 0; i < this.scale(); i++) {
            sortByFields[`f${i}`] = 1;
        }

        return [
            {$setWindowFields: {sortBy: sortByFields, output: {"f0": {$max: "$f0"}}}},
            {$unset: "_id"}
        ];
    }

    result(dataset) {
        let row = {};
        for (let i = 0; i < dataset.scale(); i++) {
            row[`f${i}`] = i;
        }
        return [row];
    }
}

export class WorkloadSetWindowFieldsManyOutputs extends PipelineWorkload {
    pipeline() {
        let outputs = {};

        for (let i = 0; i < this.scale(); i++) {
            outputs[`f${i}`] = {$max: `$f${i}`};
        }

        return [{$setWindowFields: {output: outputs}}, {$unset: "_id"}];
    }

    result() {
        let row = {};
        for (let i = 0; i < this.scale(); i++) {
            row[`f${i}`] = i;
        }
        return [row];
    }
}

export class WorkloadDensifyManyFields extends PipelineWorkload {
    /** $densify with many $partitionByFields */
    pipeline() {
        let partitionByFields = [];

        for (let i = 0; i < this.scale(); i++) {
            partitionByFields.push(`f${i}`);
        }
        return [
            {
                $densify: {
                    field: "densify",
                    partitionByFields: partitionByFields,
                    range: {bounds: "full", step: 1}
                }
            },
            {$project: {_id: 0}}
        ];
    }

    result() {
        let result = {};
        for (let i = 0; i < this.scale(); i++) {
            result[`f${i}`] = i;
        }
        return [result];
    }
}
export class WorkloadFillManyPartitionFields extends PipelineWorkload {
    /** $fill with many partitionByFields fields */
    pipeline() {
        let partitionByFields = [];

        for (let i = 0; i < this.scale(); i++) {
            partitionByFields.push(`f${i}`);
        }
        return [
            {$fill: {partitionByFields: partitionByFields, output: {f0: {"value": 1}}}},
            {$unset: "_id"}
        ];
    }

    result() {
        let result = {};
        for (let i = 0; i < this.scale(); i++) {
            result[`f${i}`] = i;
        }
        return [result];
    }
}

export class WorkloadTopK extends PipelineWorkload {
    pipeline() {
        let args = {input: "$f0", n: this.scale()};
        return [
            {
                "$group": {
                    "_id": null,
                    "minN": {$minN: args},
                    "maxN": {$maxN: args},
                    "firstN": {$firstN: args},
                    "lastN": {$lastN: args},
                    "topN": {$topN: {n: this.scale(), output: "$f0", sortBy: {"f0": 1}}},
                    "bottomN": {$bottomN: {n: this.scale(), output: "$f0", sortBy: {"f0": 1}}},
                }
            },
            {$unset: "_id"}
        ];
    }

    result() {
        let vals = range(this.scale());
        let reversed = [...vals];
        reversed.reverse();

        return [{minN: vals, maxN: reversed, firstN: vals, lastN: vals, topN: vals, bottomN: vals}];
    }
}
