import {range} from "jstests/product_limits/libs/util.js";
import {PipelineWorkload} from "jstests/product_limits/libs/workload.js";

export class WorkloadInclusionProjectManyFields extends PipelineWorkload {
    // One large inclusion $project stage.
    pipeline() {
        let project = {};

        for (let i = 0; i < this.scale(); i++) {
            project[`f${i}`] = 1;
        }
        project["_id"] = 0;

        return [{$project: project}];
    }

    result() {
        let row = {};
        for (let i = 0; i < this.scale(); i++) {
            row[`f${i}`] = i;
        }
        return [row];
    }
}

export class WorkloadExclusionProjectManyFields extends PipelineWorkload {
    // One large exclusion $project stage.
    pipeline() {
        let project = {};

        for (let i = 0; i < this.scale(); i++) {
            project[`f${i}`] = 0;
        }
        project["_id"] = 0;

        return [{$project: project}];
    }

    result() {
        // We projected away everything.
        return [{}];
    }
}

export class WorkloadExpressionProjectManyFields extends PipelineWorkload {
    // One large expression $project stage.
    pipeline() {
        let project = {};

        for (let i = 0; i < this.scale(); i++) {
            project[`f${i}`] = "a";
        }
        project["_id"] = 0;

        return [{$project: project}];
    }

    result() {
        let row = {};
        for (let i = 0; i < this.scale(); i++) {
            row[`f${i}`] = "a";
        }
        return [row];
    }
}

export class WorkloadProjectManyStages extends PipelineWorkload {
    // Pipeline length must be no longer than 200 stages.
    scale() {
        return 190;
    }

    // Many single field $project stage.
    pipeline() {
        let stages = [{$project: {_id: 0, f0: "a"}}];

        for (let i = 0; i < this.scale(); i++) {
            // Repeat inclusion-exclusion-expression $project stages.
            const val = [1, 0, "a"][i % 3];
            stages.push({$project: {f0: val}});
        }

        return stages;
    }

    result() {
        // Result depends on which stage (inclusion/exclusion/expression) ends the pipeline.
        return [[{f0: "a"}, {}, {f0: "a"}][(this.scale() - 1) % 3]];
    }
}

export class WorkloadAddFieldsManyFields extends PipelineWorkload {
    // One large $addFields stage.
    pipeline() {
        let addFields = {};

        for (let i = 0; i < this.scale(); i++) {
            addFields[`f${i}`] = "a";
        }

        return [{$unset: "_id"}, {$addFields: addFields}];
    }

    result() {
        let row = {};
        for (let i = 0; i < this.scale(); i++) {
            row[`f${i}`] = "a";
        }
        return [row];
    }
}

export class WorkloadAddFieldsManyStages extends PipelineWorkload {
    // Pipeline length must be no longer than 200 stages.
    scale() {
        return 190;
    }

    // Many single field $addFields stage.
    pipeline() {
        let stages = [{$project: {_id: 0, f0: 1}}];

        for (let i = 0; i < this.scale(); i++) {
            // Adds one to the field for each stage.
            stages.push({$addFields: {f0: {$add: ["$f0", 1]}}});
        }

        return stages;
    }

    result() {
        // Result depends on how may stages there are.
        return [{f0: this.scale()}];
    }
}

export class WorkloadProjectNestedFields extends PipelineWorkload {
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
            {$count: "cnt"},
        ];
    }
    result() {
        return [{"cnt": 1}];
    }
}

export class WorkloadReplaceRootManyFields extends PipelineWorkload {
    // One large $replaceRoot stage.
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

export class WorkloadReplaceRootNestedFields extends PipelineWorkload {
    // BSONDepth::kDefaultMaxAllowableDepth = 200
    scale() {
        return 190;
    }
    pipeline() {
        let project = range(this.scale());

        return [
            {$project: {[project.join(".")]: {$const: {}}}},
            {$unset: "_id"},
            {$replaceRoot: {newRoot: `$${project.join(".")}`}},
        ];
    }
    result() {
        return [{}];
    }
}
