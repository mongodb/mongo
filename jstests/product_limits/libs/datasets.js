import {ShardingTest} from "jstests/libs/shardingtest.js";
import * as ArrayWorkloads from "jstests/product_limits/libs/array.js";
import * as FindWorkloads from "jstests/product_limits/libs/find.js";
import * as GroupingWorkloads from "jstests/product_limits/libs/grouping.js";
import * as LongPipelineWorkloads from "jstests/product_limits/libs/long_pipelines.js";
import * as MatchWorkloads from "jstests/product_limits/libs/match.js";
import * as OperatorWorkloads from "jstests/product_limits/libs/operators.js";
import * as StageWorkloads from "jstests/product_limits/libs/stages.js";
import * as TextSearchWorkloads from "jstests/product_limits/libs/text_search.js";
import {DEFAULT_SCALE, range} from "jstests/product_limits/libs/util.js";

export class Dataset {
    scale() {
        return DEFAULT_SCALE;
    }

    collection() {
        return "coll0";
    }

    db() {
        return db;
    }

    runDataset() {
        const db = this.db();
        const session = db.getMongo().startSession();
        const sessionDb = session.getDatabase(this.constructor.name);

        print(`Populating dataset ${this.constructor.name} ...`);
        this.populate(sessionDb);
        print("Population complete.");
        for (const workload of this.workloads()) {
            const wl = new workload;
            print(`Running workload ${this.constructor.name}.${wl.constructor.name}`);
            wl.runWorkload(this, session, sessionDb);
        }

        this.stop();
    }

    workloads() {
        assert(false, `No workloads() specified for dataset ${this.constructor.name}.`);
    }

    stop() {
        // Nothing to do for the default case
    }

    data() {
        assert(false, `No data() specified for dataset ${this.constructor.name}.`);
    }
}

export class DatasetOneField extends Dataset {
    workloads() {
        return [
            ArrayWorkloads.WorkloadAddToSet,
            FindWorkloads.WorkloadFindOverSingleField,
            GroupingWorkloads.WorkloadBucketAutoManyBuckets,
            GroupingWorkloads.WorkloadBucketManyBoundaries,
            GroupingWorkloads.WorkloadManyAccumulatorsSameField,
            GroupingWorkloads.WorkloadSetWindowFieldsManyPartitions,
            GroupingWorkloads.WorkloadTopK,
            LongPipelineWorkloads.WorkloadAddFields,
            LongPipelineWorkloads.WorkloadFacetManyStages,
            MatchWorkloads.WorkloadAndOverSingleField,
            MatchWorkloads.WorkloadAndPlusOrOverSingleField,
            MatchWorkloads.WorkloadIn,
            MatchWorkloads.WorkloadManyIns,
            MatchWorkloads.WorkloadNin,
            MatchWorkloads.WorkloadOrOverSingleField,
            MatchWorkloads.WorkloadOrPlusAndOverSingleField,
            StageWorkloads.WorkloadLongFieldName,
            StageWorkloads.WorkloadManyDocuments,
            StageWorkloads.WorkloadReplaceRoot,
        ];
    }

    populate(db) {
        db.createCollection(this.collection());
        const coll = db.getCollection(this.collection());
        for (let i = 0; i < this.scale(); i++) {
            assert.commandWorked(coll.insert({f0: i}));
        }
    }

    data() {
        return range(this.scale()).map((i) => ({f0: i}));
    }
}
export class DatasetOneStringField extends Dataset {
    workloads() {
        return [
            MatchWorkloads.WorkloadRegex,
            MatchWorkloads.WorkloadRegexInIn,
        ];
    }

    populate(db) {
        db.createCollection(this.collection());
        const coll = db.getCollection(this.collection());
        for (let i = 0; i < this.scale(); i++) {
            assert.commandWorked(coll.insert({f0: `${i}`}));
        }
    }

    data() {
        return range(this.scale()).map((i) => ({f0: `${i}`}));
    }
}
export class DatasetOneDocumentOneField extends Dataset {
    workloads() {
        return [OperatorWorkloads.WorkloadRange, StageWorkloads.WorkloadNestedProject];
    }

    populate(db) {
        db.createCollection(this.collection());
        const coll = db.getCollection(this.collection());
        assert.commandWorked(coll.insert({f0: 0}));
    }
}
export class DatasetOneFieldIndex extends DatasetOneField {
    populate(db) {
        super.populate(db);
        assert.commandWorked(db.getCollection(this.collection()).createIndex({'f0': 1}));
    }
}
export class DatasetOneFieldPartialIndex extends DatasetOneField {
    populate(db) {
        super.populate(db);

        assert.commandWorked(db.getCollection(this.collection()).createIndex({f0: 1}, {
            partialFilterExpression: {f0: {$in: range(this.scale())}}
        }));
    }
}
export class DatasetWideArray extends Dataset {
    workloads() {
        return [
            ArrayWorkloads.WorkloadAll,
            ArrayWorkloads.WorkloadAllElementsTrue,
            ArrayWorkloads.WorkloadAnyElementTrue,
            ArrayWorkloads.WorkloadArrayToObject,
            ArrayWorkloads.WorkloadConcatArrays,
            ArrayWorkloads.WorkloadElemMatchGte,
            ArrayWorkloads.WorkloadFilter,
            ArrayWorkloads.WorkloadIndexOfArray,
            ArrayWorkloads.WorkloadInOverArrayField,
            ArrayWorkloads.WorkloadMap,
            ArrayWorkloads.WorkloadMatchArrayExact,
            ArrayWorkloads.WorkloadMatchArrayIndexPosition,
            ArrayWorkloads.WorkloadMatchArrayManyConditions,
            ArrayWorkloads.WorkloadReduce,
            ArrayWorkloads.WorkloadReverseArray,
            ArrayWorkloads.WorkloadSetDifference,
            ArrayWorkloads.WorkloadSetEquals,
            ArrayWorkloads.WorkloadSetIntersection,
            ArrayWorkloads.WorkloadZipArrayFields,
            StageWorkloads.WorkloadUnwind,
        ];
    }
    populate(db) {
        const collName = this.collection();
        const coll = db.getCollection(collName);
        assert.commandWorked(coll.insertMany(this.data()));
    }

    data() {
        return [{f0: range(this.scale())}];
    }
}
export class DatasetWideArrayIndex extends DatasetWideArray {
    populate(db) {
        super.populate(db);
        assert.commandWorked(db.getCollection(this.collection()).createIndex({'f0': 1}));
    }
}
export class DatasetManyCollections extends Dataset {
    workloads() {
        return [
            LongPipelineWorkloads.WorkloadManyCollectionsInLookupBushy,
            LongPipelineWorkloads.WorkloadManyCollectionsInUnionWith,
        ];
    }

    populate(db) {
        for (let i = 0; i < this.scale(); i++) {
            const collName = `coll${i}`;
            print(`Creating collection ${collName}`);
            db.createCollection(collName);
            const coll = db.getCollection(collName);
            assert.commandWorked(coll.insert({f0: 1}));
        }
    }
}
export class DatasetManyFields extends Dataset {
    workloads() {
        return [
            ArrayWorkloads.WorkloadZipManyArrays,
            FindWorkloads.WorkloadFindOverManyFields,
            GroupingWorkloads.WorkloadBucketAutoManyOutputs,
            GroupingWorkloads.WorkloadBucketManyOutputs,
            GroupingWorkloads.WorkloadDensifyManyFields,
            GroupingWorkloads.WorkloadFillManyPartitionFields,
            GroupingWorkloads.WorkloadManyAccumulatorsManyFields,
            GroupingWorkloads.WorkloadManyGroupingFields,
            GroupingWorkloads.WorkloadSetWindowFieldsManyOutputs,
            GroupingWorkloads.WorkloadSetWindowFieldsManySortBy,
            LongPipelineWorkloads.WorkloadManyMatchStages,
            MatchWorkloads.WorkloadAndOverManyFields,
            MatchWorkloads.WorkloadAndPlusOrOverManyFields,
            MatchWorkloads.WorkloadExists,
            MatchWorkloads.WorkloadMatchOverManyFields,
            MatchWorkloads.WorkloadOrOverManyFields,
            MatchWorkloads.WorkloadOrPlusAndOverManyFields,
            OperatorWorkloads.WorkloadConcat,
            OperatorWorkloads.WorkloadCond,
            OperatorWorkloads.WorkloadSwitch,
            StageWorkloads.WorkloadFacetManyFields,
            StageWorkloads.WorkloadFillManyOutputs,
            StageWorkloads.WorkloadFillManySortFields,
            StageWorkloads.WorkloadGetField,
            StageWorkloads.WorkloadLetManyVars,
            StageWorkloads.WorkloadMergeManyLet,
            StageWorkloads.WorkloadProjectManyExpressions,
            StageWorkloads.WorkloadProjectManyFields,
            StageWorkloads.WorkloadSort,
            StageWorkloads.WorkloadSortByCount,
            StageWorkloads.WorkloadUnset,
        ];
    }

    populate(db) {
        const collName = this.collection();
        let row = {};
        for (let i = 0; i < this.scale(); i++) {
            const fieldName = `f${i}`;
            row[fieldName] = i;
        }
        print(`Creating collection ${collName}`);
        db.createCollection(collName);
        const coll = db.getCollection(collName);
        assert.commandWorked(coll.insert(row));
    }

    data() {
        let result = {};
        for (let i = 0; i < this.scale(); i++) {
            result[`f${i}`] = i;
        }
        return [result];
    }

    field_list() {
        return range(this.scale()).map((i) => `$f${i}`);
    }

    value_list() {
        return range(this.scale());
    }
}
export class DatasetManyFieldsMultiFieldIndex extends DatasetManyFields {
    populate(db) {
        super.populate(db);

        let indexColumns = {};

        for (let i = 0; i < 32; i++) {
            indexColumns[`f${i}`] = 1;
        }

        assert.commandWorked(db.getCollection(this.collection()).createIndex(indexColumns));
    }
}
export class DatasetManyFieldsPartialIndex extends DatasetManyFields {
    populate(db) {
        super.populate(db);

        let indexColumns = {};

        for (let i = 0; i < 32; i++) {
            indexColumns[`f${i}`] = 1;
        }

        let indexConds = {};
        for (let i = 0; i < this.scale(); i++) {
            indexConds[`f${i}`] = i;
        }

        assert.commandWorked(db.getCollection(this.collection()).createIndex(indexColumns, {
            partialFilterExpression: indexConds
        }));
    }
}
export class DatasetManyFieldsIndexes extends DatasetManyFields {
    populate(db) {
        super.populate(db);

        for (let i = 0; i < 63; i++) {
            assert.commandWorked(db.getCollection(this.collection()).createIndex({[`f${i}`]: 1}));
        }
    }
}
export class DatasetManyFieldsWildcardIndex extends DatasetManyFields {
    populate(db) {
        super.populate(db);
        assert.commandWorked(db.getCollection(this.collection()).createIndex({'$**': 1}));
    }
}
export class DatasetNestedJSON extends Dataset {
    scale() {
        return 100;
    }
    workloads() {
        return [MatchWorkloads.WorkloadMatchLongPath];
    }
    populate(db) {
        const collName = this.collection();

        let path = [];
        for (let i = 0; i < this.scale(); i++) {
            path.push(`f${i}`);
        }

        // $addFields will generate the entire hierarchy for us.
        let pipeline = [
            {$documents: [{}]},
            {$addFields: {[path.join(".")]: "abc"}},
            {
                $out: {
                    db: this.constructor.name,
                    coll: collName,
                }
            }
        ];

        db.aggregate(pipeline).toArray();
    }
}
export class DatasetLongValue extends Dataset {
    scale() {
        return 10000000;
    }

    workloads() {
        return [MatchWorkloads.WorkloadLongValue];
    }

    populate(db) {
        const collName = this.collection();
        const coll = db.getCollection(collName);
        assert.commandWorked(coll.insert(this.data()));
    }

    data() {
        return [{
            // We need one stand-alone 'x' for the fulltext search workloads below
            f0: 'x'.repeat(this.scale()) + ' x'
        }];
    }
}
export class DatasetLongValueIndex extends DatasetLongValue {
    populate(db) {
        super.populate(db);
        assert.commandWorked(db.getCollection(this.collection()).createIndex({'f0': 1}));
    }
}
export class DatasetLongValueHashed extends DatasetLongValue {
    populate(db) {
        super.populate(db);
        assert.commandWorked(db.getCollection(this.collection()).createIndex({'f0': "hashed"}));
    }
}
export class DatasetLongValueTextIndex extends DatasetLongValue {
    workloads() {
        return [
            TextSearchWorkloads.WorkloadTextSearchLongString,
            TextSearchWorkloads.WorkloadTextSearchManyWords,
            TextSearchWorkloads.WorkloadTextSearchNegation
        ];
    }
    populate(db) {
        super.populate(db);
        assert.commandWorked(db.getCollection(this.collection()).createIndex({'f0': "text"}));
    }
}
export class DatasetSharded extends DatasetManyFields {
    db() {
        this.shardedTest = new ShardingTest({shards: 32, other: {chunkSize: 1}});

        const primaryShard = this.shardedTest.shard0;
        const dbName = this.constructor.name;
        const db = this.shardedTest.s.getDB(dbName);

        assert.commandWorked(this.shardedTest.s.adminCommand(
            {enableSharding: dbName, primaryShard: primaryShard.shardName}));

        let collName = this.collection();

        let shardKey = {};
        for (let i = 0; i < 32; i++) {
            shardKey[`f${i}`] = 1;
        }

        db.createCollection(collName);
        assert.commandWorked(this.shardedTest.s.adminCommand(
            {shardCollection: `${dbName}.${collName}`, key: shardKey}));

        return db;
    }
    stop() {
        this.shardedTest.stop();
    }
}

export const DATASETS = [
    DatasetOneField,
    DatasetOneFieldIndex,
    DatasetOneFieldPartialIndex,
    DatasetOneDocumentOneField,
    DatasetOneStringField,
    DatasetWideArray,
    DatasetWideArrayIndex,
    DatasetManyCollections,
    DatasetManyFields,
    DatasetManyFieldsMultiFieldIndex,
    DatasetManyFieldsIndexes,
    DatasetManyFieldsPartialIndex,
    DatasetManyFieldsWildcardIndex,
    DatasetLongValue,
    DatasetLongValueIndex,
    DatasetLongValueHashed,
    DatasetLongValueTextIndex,
    DatasetSharded,
    DatasetNestedJSON
];
