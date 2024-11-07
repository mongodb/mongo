import {range} from "jstests/product_limits/libs/util.js";
import {Workload} from "jstests/product_limits/libs/workload.js";

export class FindWorkload extends Workload {
    runWorkload(dataset, _, db) {
        const coll = db.getCollection(this.collection());
        const find = this.find(dataset);
        printjsononeline(find);

        coll.explain("allPlansExecution").find(dataset, {"_id": 0});

        const startTime = Date.now();
        const actualResult = coll.find(find, {"_id": 0}).toArray();
        const duration = Date.now() - startTime;
        print(`${dataset.constructor.name}.${this.constructor.name} took ${duration} ms.`);

        this.check(dataset, actualResult);
        print("Find execution complete.");
    }
}
export class WorkloadFindOverSingleField extends FindWorkload {
    scale() {
        // SERVER-96119 SBE: Stack overflow with many conditions to a $match, index
        return Math.min(1000, super.scale());
    }

    find() {
        let find = [];
        for (let i = 0; i < this.scale(); i++) {
            find.push({'f0': {$lt: this.scale() + i}});
        }
        return {$and: find};
    }

    result() {
        return range(this.scale()).map((i) => ({f0: i}));
    }
}

export class WorkloadFindOverManyFields extends FindWorkload {
    find() {
        let find = {};
        for (let i = 0; i < this.scale(); i++) {
            find[`f${i}`] = i;
        }
        return find;
    }
}
