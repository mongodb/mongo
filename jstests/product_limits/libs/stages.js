import {range} from "jstests/product_limits/libs/util.js";
import {PipelineWorkload} from "jstests/product_limits/libs/workload.js";

export class WorkloadFillManySortFields extends PipelineWorkload {
    /** $fill with many sortBy fields */
    pipeline() {
        let sortByFields = {};

        for (let i = 0; i < this.scale(); i++) {
            sortByFields[`f${i}`] = 1;
        }
        return [{$fill: {sortBy: sortByFields, output: {f0: {"value": 1}}}}, {$unset: "_id"}];
    }

    result() {
        let result = {};
        for (let i = 0; i < this.scale(); i++) {
            result[`f${i}`] = i;
        }
        return [result];
    }
}

export class WorkloadFillManyOutputs extends PipelineWorkload {
    /** $fill with many outputs */
    pipeline() {
        let output = {};

        for (let i = 0; i < this.scale(); i++) {
            output[`f${i}`] = {method: "linear"};
        }
        return [{$fill: {sortBy: {"f0": 1}, output: output}}, {$unset: "_id"}];
    }

    result() {
        let result = {};
        for (let i = 0; i < this.scale(); i++) {
            result[`f${i}`] = i;
        }
        return [result];
    }
}

export class WorkloadMergeManyLet extends PipelineWorkload {
    /** $merge with many let */
    pipeline() {
        let letList = {};

        for (let i = 0; i < this.scale(); i++) {
            letList[`f${i}`] = i;
        }
        return [{
            $merge: {
                into: this.constructor.name,
                whenMatched: [{$addFields: {"foo": "bar"}}],
                let : letList
            }
        }];
    }

    result() {
        return [];
    }
}

export class WorkloadLetManyVars extends PipelineWorkload {
    /* Use many $lets with many variables, each with a complex expression.
     */
    scale() {
        // Object size exceeds limit of 16793600 bytes.
        return 50;
    }

    pipeline() {
        let condList = range(this.scale()).map((i) => ({$eq: [`$f${i}`, i]}));

        let varsList = {};
        for (let i = 0; i < this.scale(); i++) {
            varsList[`v${i}`] = {"$and": condList};
        }
        let inList = range(this.scale()).map((i) => (`$$v${i}`));

        let letList = {};
        for (let i = 0; i < this.scale(); i++) {
            letList[`max${i}`] = {$let: {vars: varsList, in : {$max: inList}}};
        }

        return [{$project: letList}, {$unset: "_id"}];
    }

    result() {
        let result = {};
        for (let i = 0; i < this.scale(); i++) {
            result[`max${i}`] = true;
        }
        return [result];
    }
}

export class WorkloadProjectManyExpressions extends PipelineWorkload {
    /** One large $project stage */
    pipeline() {
        let project = {};

        for (let i = 0; i < this.scale(); i++) {
            project[`f${i}`] = 'a';
        }
        project['_id'] = 0;

        return [{$project: project}];
    }

    result() {
        let row = {};
        for (let i = 0; i < this.scale(); i++) {
            row[`f${i}`] = 'a';
        }
        return [row];
    }
}

export class WorkloadProjectManyFields extends PipelineWorkload {
    /** One large $project stage with many fields */
    pipeline() {
        let project = {};

        for (let i = 0; i < this.scale(); i++) {
            project[`f${i}`] = 0;
        }
        return [{$project: project}, {$unset: "_id"}];
    }

    result() {
        // We projected away everything
        return [{}];
    }
}

export class WorkloadNestedProject extends PipelineWorkload {
    // BSONDepth::kDefaultMaxAllowableDepth = 200
    scale() {
        return 190;
    }
    pipeline() {
        let project = range(this.scale());

        return [
            {$project: {[project.join(".")]: "abc"}},
            {$unset: "_id"},
            {$unset: project.join(".")},
            {$count: "cnt"}
        ];
    }
    result() {
        return [{"cnt": 1}];
    }
}
//
// $replaceRoot
//
export class WorkloadReplaceRoot extends PipelineWorkload {
    /** One large $replaceRoot stage */
    pipeline() {
        let replaceRoot = {};

        for (let i = 0; i < this.scale(); i++) {
            replaceRoot[`f${i}`] = i;
        }
        return [{$replaceRoot: {newRoot: replaceRoot}}, {$count: "cnt"}];
    }

    result() {
        return [{"cnt": this.scale()}];
    }
}

export class WorkloadSort extends PipelineWorkload {
    scale() {
        // "too many compound keys"
        return 32;
    }
    pipeline() {
        let sortKey = {};

        for (let i = 0; i < this.scale(); i++) {
            sortKey[`f${i}`] = 1;
        }
        return [{$sort: sortKey}, {$unset: "_id"}];
    }

    result(dataset) {
        let row = {};
        for (let i = 0; i < dataset.scale(); i++) {
            row[`f${i}`] = i;
        }
        return [row];
    }
}

export class WorkloadSortByCount extends PipelineWorkload {
    sortKey() {
        let sortKey = [];

        for (let i = 0; i < this.scale(); i++) {
            sortKey.push(`f${i}`);
        }
        return sortKey;
    }
    pipeline() {
        return [{$sortByCount: {$concat: this.sortKey()}}];
    }

    result() {
        const concatKey = this.sortKey().join("");

        return [{_id: concatKey, count: 1}];
    }
}

export class WorkloadUnset extends PipelineWorkload {
    pipeline() {
        let unsetList = [];

        for (let i = 0; i < this.scale(); i++) {
            unsetList.push(`f${i}`);
        }
        unsetList.push("_id");
        return [{$unset: unsetList}];
    }

    result() {
        // We projected away everything
        return [{}];
    }
}

export class WorkloadUnwind extends PipelineWorkload {
    pipeline() {
        let unsetList = [];

        for (let i = 0; i < this.scale(); i++) {
            unsetList.push(`f${i}`);
        }
        unsetList.push("_id");
        return [{$unwind: "$f0"}, {$unset: "_id"}];
    }

    result() {
        let result = [];
        for (let i = 0; i < this.scale(); i++) {
            result.push({"f0": i});
        }
        return result;
    }
}

export class WorkloadManyDocuments extends PipelineWorkload {
    pipeline() {
        let documents = [];

        for (let i = 0; i < this.scale(); i++) {
            documents.push({[`f${i}`]: i});
        }
        return [{$documents: documents}];
    }

    result() {
        let result = [];
        for (let i = 0; i < this.scale(); i++) {
            result.push({[`f${i}`]: i});
        }
        return result;
    }
}

export class WorkloadFacetManyFields extends PipelineWorkload {
    /** $facet with many fields */
    pipeline() {
        let fields = {};

        for (let i = 0; i < this.scale(); i++) {
            fields[`f${i}`] = [{$project: {[`f${i}`]: 1, _id: 0}}];
        }
        return [{$facet: fields}];
    }

    result() {
        let result = {};
        for (let i = 0; i < this.scale(); i++) {
            result[`f${i}`] = [{[`f${i}`]: i}];
        }
        return [result];
    }
}

export class WorkloadGetField extends PipelineWorkload {
    pipeline() {
        return [
            {
                $project:
                    {"getField": {$getField: {field: `f${this.scale() - 1}`, input: "$$CURRENT"}}}
            },
            {$unset: "_id"}
        ];
    }

    result() {
        return [{getField: this.scale() - 1}];
    }
}

export class WorkloadLongFieldName extends PipelineWorkload {
    longString() {
        return 'x'.repeat(this.scale());
    }

    pipeline() {
        return [{$count: this.longString()}];
    }

    result() {
        return [{[this.longString()]: this.scale()}];
    }
}
