import {PipelineWorkload} from "jstests/product_limits/libs/workload.js";

export class LongPipelineWorkload extends PipelineWorkload {
    /**
     * A pipeline can not have more than 1000 stages,
     * and we usually tack $unset at the end
     */
    scale() {
        return Math.min(super.scale(), 990);
    }
}

export class WorkloadManyCollectionsInUnionWith extends LongPipelineWorkload {
    /** $unionWith of many collections. */
    // A pipeline can not have more than 1000 stages, and we tack $unset at the end
    scale() {
        return Math.min(super.scale(), 999);
    }

    pipeline() {
        let pipeline = [];
        for (let i = 0; i < this.scale(); i++) {
            const collName = `coll${i}`;
            pipeline.push({$unionWith: collName});
        }
        pipeline.push({$unset: "_id"});
        return pipeline;
    }

    result() {
        let result = [{f0: 1}];
        for (let i = 0; i < this.scale(); i++) {
            result.push({f0: 1});
        }
        return result;
    }
}

export class WorkloadManyCollectionsInLookupBushy extends LongPipelineWorkload {
    /** Many $lookup-s where each new collection is joined to the same column. */
    scale() {
        // Too many $lookups result in "errmsg" : "BSONObj size: 53097740 (0x32A350C) is invalid.
        // Size must be between 0 and 16793600(16MB) First element: slots: \"$$RESULT=s7202 env: {
        // }\"",
        return Math.min(500, super.scale());
    }

    pipeline() {
        let pipeline = [];
        let unsetList = ["_id"];
        for (let i = 1; i < this.scale(); i++) {
            pipeline.push({
                $lookup:
                    {from: `coll${i}`, localField: "f0", foreignField: "f0", as: `asField_${i}`}
            });
            // Remove all _id fields
            unsetList.push(`asField_${i}._id`);
        }

        pipeline.push({$unset: unsetList});

        return pipeline;
    }

    result() {
        let result = {f0: 1};
        for (let i = 1; i < this.scale(); i++) {
            result[`asField_${i}`] = [{f0: 1}];
        }
        return [result];
    }
}
export class WorkloadManyMatchStages extends LongPipelineWorkload {
    /** Many $match stages. */
    pipeline() {
        let pipeline = [];

        for (let i = 0; i < this.scale(); i++) {
            pipeline.push({$match: {[`f${i}`]: i}});
        }
        pipeline.push({$unset: "_id"});

        return pipeline;
    }
}

export class WorkloadFacetManyStages extends LongPipelineWorkload {
    /** $facet with many pipeline stages */
    pipeline() {
        let stages = [{$limit: 1}];
        for (let i = 0; i < this.scale(); i++) {
            stages.push({$addFields: {[`f${i}`]: i}});
        }
        stages.push({$project: {_id: 0}});
        return [{$facet: {f0: stages}}];
    }

    result() {
        let result = {};
        for (let i = 0; i < this.scale(); i++) {
            result[`f${i}`] = i;
        }
        return [{"f0": [result]}];
    }
}

export class WorkloadAddFields extends LongPipelineWorkload {
    /** Many individual $addFields stages */
    pipeline() {
        let pipeline = [];

        for (let i = 0; i < this.scale(); i++) {
            pipeline.push({$addFields: {[`f${i}`]: i}});
        }
        pipeline.push({$limit: this.scale()}, {$unset: "_id"});
        return pipeline;
    }

    result() {
        let result = [];
        let row = {};
        for (let i = 0; i < this.scale(); i++) {
            row[`f${i}`] = i;
        }
        for (let i = 0; i < this.scale(); i++) {
            result.push(row);
        }
        return result;
    }
}
