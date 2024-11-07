import {DEFAULT_SCALE} from "jstests/product_limits/libs/util.js";

export class Workload {
    scale() {
        return DEFAULT_SCALE;
    }

    collection() {
        return "coll0";
    }

    check(dataset, actualResult) {
        actualResult.sort();
        let expectedResult = this.result(dataset);
        expectedResult.sort();
        print("Comparison start ...");
        assert.eq(expectedResult, actualResult);
        print("Comparison complete.");
    }

    result(dataset) {
        // By default we assume the workload returns the complete dataset
        return dataset.data();
    }
}
export class PipelineWorkload extends Workload {
    runWorkload(dataset, _, db) {
        const coll = db.getCollection(this.collection());
        const pipeline = this.pipeline(dataset);
        printjsononeline(pipeline);

        if (!pipeline[0].hasOwnProperty("$documents")) {
            try {
                coll.explain("allPlansExecution").aggregate(pipeline);
            } catch (error) {
                /// Large explains() can not legitimately fit in a BSONObject
                printjsononeline(error.codeName);
                assert(error.code === ErrorCodes.BSONObjectTooLarge, error);
            }
        }

        const startTime = Date.now();
        const cursor = pipeline[0].hasOwnProperty("$documents") ? db.aggregate(pipeline)
                                                                : coll.aggregate(pipeline);
        const actualResult = cursor.toArray();
        const duration = Date.now() - startTime;
        print(`${dataset.constructor.name}.${this.constructor.name} took ${duration} ms.`);

        this.check(dataset, actualResult);
        print("Pipeline execution complete.");
    }
}
