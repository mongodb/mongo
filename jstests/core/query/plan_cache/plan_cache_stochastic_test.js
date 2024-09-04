/**
 * Stochastically tests that plan caching is not causing incorrect results. Like a fuzzer but
 * simpler and self-contained. This operates by generating random documents and aggregation queries.
 * It runs each agg query twice as originally created, so that if caching is enabled it will become
 * cached and active (if cache-eligible), then tweaks a parameter value in it and runs
 * it again, comparing the first to the final result.
 *
 * This does not test whether caching occurs when expected; it only tests whether caching, if and
 * when it happens, produces the same result as non-cached execution. Thus it does not need to be
 * updated if the caching criteria change and is agnostic to whatever cache algorithm is active or
 * whether there is any caching being done at all.
 *
 * To reproduce a failure, see the info between the "BEGIN FAILURE REPRO" and "END FAILURE REPRO"
 * comments below.
 *
 * @tags: [
 *   # Sharded collections will produce result differences from nondeterministic document processing
 *   # order that the sorting of results in this script will not rectify, such as the order in
 *   # which $lookup results appear in its "as" output array.
 *   assumes_unsharded_collection,
 *   assumes_standalone_mongod,
 *   assumes_balancer_off,
 *   assumes_no_implicit_index_creation,
 * ]
 */

import {LcgRandom} from "jstests/libs/lcg_random.js";

////////////////////////////////////////////////////////////////////////////////////////////////////
// GLOBAL CONSTANTS
////////////////////////////////////////////////////////////////////////////////////////////////////

// Supported accumulators for $group stage.
const kAccumulators = ["$avg", "$max", "$min", "$sum"];

// Possible boolean literals.
const kBoolLiterals = [false, true];

// Special proxy field name for array entries, which are unnamed, indicating they can be any
// type.
const kFieldNameAny = "any";

// Possible field names. Fields beginning with prefixes:
//   "key" - Integer literals of restricted cardinality to be used as join keys for
//           AggStageLookup, upon which indexes will be defined; this avoids super-slow $lookups
//           producing huge result sets.
//   "lit" - Generic literals.
//   "obj", "arr" - Objects and arrays, respectively, unless the max depth has been reached, in
//           which case they will be generic literals (like "lit" fields).
//
// To prevent unbounded depth and potentially infinite-sized documents from a "nuclear chain
// reaction" effect where populating the fields of documents and arrays recursively creates ever
// more subdocuments and subarrays than it terminates, "obj" and "arr" fields will instead be
// populated by generic literals when 'kValueMaxDepth' is reached.
//
// The use of prefixes to drive value types is to prevent a multi-key-part $sort from being
// attempted on more than one array field, as this is not supported in MQL and throws the BadValue
// error "cannot sort with keys that are parallel arrays".
//
// The target mix from this set of field names is 70% generic literals, 10% key literals, 10%
// objects, and 10% arrays.
const kKeyFieldNames = ["key00", "key01"];
const kLitFieldNames = [
    "lit02",
    "lit03",
    "lit04",
    "lit05",
    "lit06",
    "lit07",
    "lit08",
    "lit09",
    "lit10",
    "lit11",
    "lit12",
    "lit13",
    "lit14",
    "lit15"
];
const kObjFieldNames = ["obj16", "obj17"];
const kArrFieldNames = ["arr18", "arr19"];
const kNonKeyFieldNames = kLitFieldNames.concat(kObjFieldNames).concat(kArrFieldNames);
const kFieldNames = kKeyFieldNames.concat(kNonKeyFieldNames);

// Precomputed field name references of the form "$"" + 'kFieldNames[i]'.
let kFieldNameRefs = [];
for (let name of kFieldNames) {
    kFieldNameRefs.push("$" + name);
}

// Possible sort direction literals.
const kSortLiterals = [-1, 1];

// Possible value literals. We don't want too many of these so that randomly generated queries will
// have a good chance of matching some documents. Does not include the JS pseudovalue <undefined> as
// it causes havoc. This must also not include any array literals as they could trigger unsupported
// $sort stages with multiple array key parts.
const kValueLiterals =
    [-7, -6, 0, 6, 7, -9.99, -8.88, 8.88, 9.99, "Quoth", "the", "raven", "nevermore", null, NaN];

// Maximum depth for objects (including documents) and arrays.
const kValueMaxDepth = 4;

////////////////////////////////////////////////////////////////////////////////////////////////////
// INFRASTRUCTURE HELPER FUNCTIONS
////////////////////////////////////////////////////////////////////////////////////////////////////

// Returns a random document with the given 'id' set as the value of the "_id" field. Uses the
// 'rngDocs' random number generator by assigning that to 'rngCurr', then reassigning it back to the
// 'rngMain' generator before returning so everything else will use 'rngMain'.
function createDoc(id) {
    rngCurr = rngDocs;  // switch to the docs random number generator

    let result = {"_id": id};
    // All docs must have the "key" fields, else $lookup produces matches between all docs that are
    // missing the match fields as they are both treated as null, producing huge cartesian joins.
    for (let keyName of kKeyFieldNames) {
        result[keyName] = getKeyFieldValue();
    }

    const numFields = rngCurr.getRandomInt(1, 5);
    for (let field = 0; field < numFields; ++field) {
        // Picks a non-duplicate field name.
        let fieldName = getFieldName();
        while (result.hasOwnProperty(fieldName)) {
            fieldName = getFieldName();
        }
        result[fieldName] = getValueRaw(0, fieldName);
    }

    rngCurr = rngMain;  // switch back to the main random number generator
    return result;
}  // function createDoc

// Returns an array of random documents with sequential "_id" values starting from 0.
function createDocs(numDocs) {
    let result = [];
    for (let id = 0; id < numDocs; ++id) {
        result.push(createDoc(id));
    }
    return result;
}  // function createDocs

// Creates non-unique indexes on the field names in the 'fieldNames' array.
function createIndexes(fieldNames) {
    for (let keyName of fieldNames) {
        let keyObj = {};
        keyObj[keyName] = 1;
        assert.commandWorked(db.runCommand({
            createIndexes: coll.getName(),
            indexes: [{key: keyObj, name: `index_${keyName}`, unique: false}]
        }));
    }
}

// Gets a random field name.
function getFieldName() {
    return kFieldNames[rngCurr.getRandomInt(0, kFieldNames.length)];
}

// Gets a random value for a key field. Key field cardinalities are proportional to number of
// documents to keep the number of $lookup matches for a given doc small. If the cardinality were
// too low, pipelines with $lookup would run very slowly and produce huge result sets that take a
// long time to sort and compare.
function getKeyFieldValue() {
    return rngCurr.getRandomInt(0, kNumDocs);
}

// Gets a random non-key field name.
function getNonKeyFieldName() {
    return kNonKeyFieldNames[rngCurr.getRandomInt(0, kNonKeyFieldNames.length)];
}

// Returns a random raw Javascript array. 'depth' is the current depth in the doc being created,
// used to limit the total depth to 'kValueMaxDepth'.
function getArrayRaw(depth) {
    let result = [];
    const numEntries = rngCurr.getRandomInt(0, 10);
    for (let entry = 0; entry < numEntries; ++entry) {
        result.push(getValueRaw(depth + 1, kFieldNameAny));
    }
    return result;
}  // function getArrayRaw

// Returns a random raw JavaScript object. 'depth' is the current depth in the doc being created,
// used to limit the total depth to 'kValueMaxDepth'.
function getObjectRaw(depth) {
    let result = {};
    const numFields = rngCurr.getRandomInt(0, 10);
    for (let field = 0; field < numFields; ++field) {
        // Picks a non-duplicate field name.
        let fieldName = getFieldName();
        while (result.hasOwnProperty(fieldName)) {
            fieldName = getFieldName();
        }
        result[fieldName] = getValueRaw(depth + 1, fieldName);
    }
    return result;
}  // function getObjectRaw

// Returns a value of any supported type, which may be an object or array and contain more nested
// object and arrays. Non-recursive calls should pass 'depth' 0, and recursive calls increment this
// to prevent objects and arrays from potentially having unbounded depths. 'fieldName' should be an
// entry from 'kFieldNames' or the wildcard 'kValueAny', and its prefix is used to constrain the
// type of value returned.
//
// Terminals will be managed by the 'valueLiteralManager' ValueLiteralManager instance (wrapped in
// ValueLiteral instances) and thus be mutatable EXCEPT for "key" field names, whose values are not
// mutatable as they are not specified in queries so don't need to be.
function getValueManaged(depth, fieldName, valueLiteralManager) {
    // For 'kFieldNameAny', maintain the type probability distribution of 'kFieldNames'.
    if (fieldName === kFieldNameAny) {
        fieldName = getFieldName();
    }
    if (fieldName.startsWith("lit")) {
        return valueLiteralManager.getLiteralManaged();
    } else if (fieldName.startsWith("key")) {
        return getKeyFieldValue();  // NOT managed
    }

    // Prevents objects and arrays from growing to arbitrary depths.
    if (depth >= kValueMaxDepth) {
        return valueLiteralManager.getLiteralManaged();
    }
    if (fieldName.startsWith("obj")) {
        return new ValueObject(depth, valueLiteralManager);
    } else if (fieldName.startsWith("arr")) {
        return new ValueArray(depth, valueLiteralManager);
    }
    assert(false, `Unknown field type ${fieldName.substring(0, 3)}`);
}  // function getValueManaged

// Returns a value of any supported type, which may be an object or array and contain more nested
// object and arrays. Non-recursive calls should pass 'depth' 0, and recursive calls increment this
// to prevent objects and arrays from potentially having unbounded depths. 'fieldName' should be an
// entry from 'kFieldNames' or the wildcard 'kValueAny', and its prefix is used to constrain the
// type of value returned.
//
// Terminals will be raw, unmanaged (not mutatable).
function getValueRaw(depth, fieldName) {
    if (depth >= kValueMaxDepth) {
        return getValueLiteralRaw();
    }

    // For 'kFieldNameAny', maintain the type probability distribution of 'kFieldNames'.
    if (fieldName === kFieldNameAny) {
        fieldName = getFieldName();
    }
    if (fieldName.startsWith("lit")) {
        return getValueLiteralRaw();
    } else if (fieldName.startsWith("obj")) {
        return getObjectRaw(depth);
    }
    return getArrayRaw(depth);  // fieldName.startsWith("arr")
}  // function getValueRaw

// Returns a raw (unmanaged) value literal (not wrapped in a ValueLiteral).
function getValueLiteralRaw() {
    return kValueLiterals[rngCurr.getRandomInt(0, kValueLiterals.length)];
}

// Logs a detailed message for a manual repro success, i.e. when the repro attempt did get a
// mismatch, strongly implying (but not guaranteeing) that the original failure was reproduced.
function logRepro(pipeline, results1, results2, msg) {
    const fullMsg = msg + `\nPipeline:\n${tojson(pipeline)}` +
        `\nFirst run results (${results1.length} docs):\n${tojson(results1)}` +
        `\nSecond run results (${results2.length} docs):\n${tojson(results2)}`;
    assert(false, fullMsg);
}  // function logFailedRepro

// Logs a detailed failure message for a mismatch between the results of two runs of the same
// pipeline and information about how to reproduce it.
//   pipeline (JS array) - the (original or mutated) pipeline that was run
//   results1, results2 (JS Array) - results from the first and second runs of 'pipeline'
//   msg (string) - caller-provided specific message for this failure
function logFailedTest(pipeline, results1, results2, msg) {
    const fullMsg = msg + `\nTo reproduce, in this test script replace` +
        `\n  "let rngSeed = undefined;" with "let rngSeed = ${rngSeed};"` +
        `\n  "const pipelineOrig = undefined;" with "const pipelineOrig =` +
        `\n${tojson(pipeline.unmutate().toMql())};"` +
        `\n  "const pipelineMutant = undefined;" with "const pipelineMutant =` +
        `\n${tojson(pipeline.remutate().toMql())};"` +
        `\nFirst run results (${results1.length} docs):\n${tojson(results1)}` +
        `\nSecond run results (${results2.length} docs):\n${tojson(results2)}`;
    assert(false, fullMsg);
}  // function logFailedTest

// Recursively converts an array to MQL.
function toMqlArray(arr) {
    let result = [];
    for (let entry of arr) {
        // Only managed values have a toMql() function and must call it to convert to plain JSON.
        if (typeof entry.toMql === 'function') {
            result.push(entry.toMql());
        } else {
            result.push(entry);
        }
    }
    return result;
}  // function toMqlArray

// Recursively converts an object to MQL.
function toMqlObject(obj) {
    let result = {};
    for (let fieldName in obj) {
        // Skip fields inherited from the prototype.
        if (obj.hasOwnProperty(fieldName)) {
            // Only managed values have a toMql() function and must call it to convert to plain
            // JSON.
            if (typeof obj[fieldName].toMql === 'function') {
                result[fieldName] = obj[fieldName].toMql();
            } else {
                result[fieldName] = obj[fieldName];
            }
        }
    }
    return result;
}  // function toMqlObject

////////////////////////////////////////////////////////////////////////////////////////////////////
// CLASS Accumulator
////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * Represents an accumulator object in an AggStageGroup.
 */
class Accumulator {
    // 'fieldNameRefMgr' is always needed as this creates accumulator objects in which the
    // accumulator target (rhs) is a managed (mutatable) FieldValueRef.
    constructor(fieldNameRefMgr) {
        // The accumulator object.
        this._self = {};

        this._self[kAccumulators[rngCurr.getRandomInt(0, kAccumulators.length)]] =
            fieldNameRefMgr.getLiteralManaged();
    }  // constructor

    // Recursively converts the subtree rooted at this stage into an MQL query.
    toMql() {
        return toMqlObject(this._self);
    }
}  // class Accumulator

////////////////////////////////////////////////////////////////////////////////////////////////////
// CLASS AggStageLookup
////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * Represents a $lookup stage in an AggPipeline. Its "localField" and "foreignField" arguments will
 * only refer to key field names (thoes in 'kKeyFieldNames') to limit the $lookup result set size.
 */
class AggStageLookup {
    constructor(aggPipeline) {
        // The $lookup: value object.
        this._self = {
        from: coll.getName(),  // not mutatable
        localField: aggPipeline.getKeyFieldNameManaged(),
        foreignField: aggPipeline.getKeyFieldNameManaged(),
        as: aggPipeline.getArrFieldNameManaged(),
    };
    }  // constructor

    // Recursively converts the subtree rooted at this stage into an MQL query.
    toMql() {
        return {"$lookup": toMqlObject(this._self)};
    }
}  // class AggStageLookup

////////////////////////////////////////////////////////////////////////////////////////////////////
// CLASS AggStageMatch
////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * Represents a $match stage in an AggPipeline.
 */
class AggStageMatch {
    constructor(aggPipeline) {
        // The $match: value object.
        this._self = {};

        const numFields = rngCurr.getRandomInt(1, 5);
        for (let field = 0; field < numFields; ++field) {
            // Picks a non-duplicate field name.
            let fieldName = getFieldName();
            while (this._self.hasOwnProperty(fieldName)) {
                fieldName = getFieldName();
            }
            this._self[fieldName] = aggPipeline.getValueManaged(fieldName);
        }
    }  // constructor

    // Recursively converts the subtree rooted at this stage into an MQL query.
    toMql() {
        return {"$match": toMqlObject(this._self)};
    }
}  // class AggStageMatch

////////////////////////////////////////////////////////////////////////////////////////////////////
// CLASS AggStageGroup
////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * Represents a $group stage in an AggPipeline.
 */
class AggStageGroup {
    constructor(aggPipeline) {
        // The $group: value object.
        this._self = {_id: aggPipeline.getFieldNameRefManaged()};

        const numFields = rngCurr.getRandomInt(0, 4);
        for (let field = 0; field < numFields; ++field) {
            // Picks a non-duplicate field name.
            let fieldName = getFieldName();
            while (this._self.hasOwnProperty(fieldName)) {
                fieldName = getFieldName();
            }
            this._self[fieldName] = aggPipeline.getAccumulator();
        }
    }  // constructor

    // Recursively converts the subtree rooted at this stage into an MQL query.
    toMql() {
        return {"$group": toMqlObject(this._self)};
    }
}  // class AggStageGroup

////////////////////////////////////////////////////////////////////////////////////////////////////
// CLASS AggStageProject
////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * Represents a $project stage in an AggPipeline.
 */
class AggStageProject {
    constructor(aggPipeline) {
        // The $project: value object.
        this._self = {};

        // A single managed boolean literal decides include or exclude for all fields, per the
        // $project stage's requirement that it must not mix includes and excludes except for
        // optionally excluding "_id".
        const include = aggPipeline.getBoolLiteralManaged();
        if (include._literalIdx === 1) {
            // Inclusion case: always includes all the "key##" fields so $lookup will always have
            // the keys it needs to avoid huge results and queries that take minutes to run.
            for (let name of kKeyFieldNames) {
                this._self[name] = include;
            }
        }
        const numFields = rngCurr.getRandomInt(1, 4);
        for (let field = 0; field < numFields; ++field) {
            // Picks a non-duplicate field name. This chooses only non-key fields so if we are in
            // the exclusion case we do not project away any key fields $lookup needs, whereas if we
            // are in the inclusion case we already forced inclusion of all the key fields above.
            let fieldName = getNonKeyFieldName();
            while (this._self.hasOwnProperty(fieldName)) {
                fieldName = getNonKeyFieldName();
            }
            this._self[fieldName] = include;
        }
        // Coin flip to exclude the "_id" field.
        if (rngCurr.getRandomInt(0, 2) === 0) {
            this._self["_id"] = false;
        }
    }  // constructor

    // Recursively converts the subtree rooted at this stage into an MQL query.
    toMql() {
        return {"$project": toMqlObject(this._self)};
    }
}  // class AggStageProject

////////////////////////////////////////////////////////////////////////////////////////////////////
// CLASS AggStageSort
////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * Represents a $sort stage in an AggPipeline.
 */
class AggStageSort {
    constructor(aggPipeline) {
        // The $sort: value object.
        this._self = {};

        let arrayInKey = false;  // does the $sort key already have an array field in it?
        const numFields = rngCurr.getRandomInt(1, 3);
        for (let field = 0; field < numFields; ++field) {
            let fieldName = getFieldName();
            let arrField = fieldName.startsWith("arr");

            // Picks a non-duplicate field name and avoids multiple array fields in the $sort key,
            // which MQL does not support.
            while ((arrayInKey && arrField) || this._self.hasOwnProperty(fieldName)) {
                fieldName = getFieldName();
                arrField = fieldName.startsWith("arr");
            }
            this._self[fieldName] = aggPipeline.getSortLiteralManaged();
            if (arrField) {
                arrayInKey = true;
            }
        }
    }  // constructor

    // Recursively converts the subtree rooted at this stage into an MQL query.
    toMql() {
        return {"$sort": toMqlObject(this._self)};
    }
}  // class AggStageSort

////////////////////////////////////////////////////////////////////////////////////////////////////
// CLASS AggStageUnwind
////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * Represents an $unwind stage in an AggPipeline.
 */
class AggStageUnwind {
    constructor(aggPipeline) {
        // The $unwind: value object.
        this._self = {
            path: aggPipeline.getFieldNameRefManaged(),
            includeArrayIndex: aggPipeline.getFieldNameManaged(),
            preserveNullAndEmptyArrays: aggPipeline.getBoolLiteralManaged()
        };
    }  // constructor

    // Recursively converts the subtree rooted at this stage into an MQL query.
    toMql() {
        return {"$unwind": toMqlObject(this._self)};
    }
}  // class AggStageUnwind

////////////////////////////////////////////////////////////////////////////////////////////////////
// CLASS AggPipeline
////////////////////////////////////////////////////////////////////////////////////////////////////

// Array of the classes representing supported aggregation stage types.
const kStageTypes =
    [AggStageGroup, AggStageLookup, AggStageMatch, AggStageProject, AggStageSort, AggStageUnwind];

/**
 * Represents a complete aggregation pipeline.
 */
class AggPipeline {
    constructor() {
        // Managers for the different types of mutatable literals in this AggPipeline.
        this._literalMgrs = [];
        this._literalMgrs.push(this._arrFieldNameMgr = new LiteralMgr(kArrFieldNames));
        this._literalMgrs.push(this._boolLiteralMgr = new LiteralMgr(kBoolLiterals));
        this._literalMgrs.push(this._fieldNameMgr = new LiteralMgr(kFieldNames));
        this._literalMgrs.push(this._fieldNameRefMgr = new LiteralMgr(kFieldNameRefs));
        this._literalMgrs.push(this._keyFieldNameMgr = new LiteralMgr(kKeyFieldNames));
        this._literalMgrs.push(this._nonKeyFieldNameMgr = new LiteralMgr(kNonKeyFieldNames));
        this._literalMgrs.push(this._sortLiteralMgr = new LiteralMgr(kSortLiterals));
        this._literalMgrs.push(this._valueLiteralMgr = new LiteralMgr(kValueLiterals));

        // The stages of the pipeline.
        this._self = [];

        const numStages = rngCurr.getRandomInt(1, 5);
        for (let stage = 0; stage < numStages; ++stage) {
            this._self.push(new kStageTypes[rngCurr.getRandomInt(0, kStageTypes.length)](this));
        }

        // Each index of '_runningSums' is the sum of managed literals for all literal managers with
        // equal or lower index in 'this._literalMgrs', used to create a uniform mutation
        // distribution.
        const len = this._literalMgrs.length;
        this._runningSums = [this._literalMgrs[0].getNum()];
        for (let i = 1; i < len; ++i) {
            this._runningSums[i] = this._runningSums[i - 1] + this._literalMgrs[i].getNum();
        }
        this._runningSumsTotal = this._runningSums[len - 1];
    }  // constructor

    // Gets a random accumulator object whose RHS is a managed 'kFieldNameRefs' entry'.
    getAccumulator() {
        return new Accumulator(this._fieldNameRefMgr);
    }

    // Gets a mutatable wrapper of a 'kArrFieldNames' entry managed by '_arrFieldNameMgr'.
    getArrFieldNameManaged() {
        return this._arrFieldNameMgr.getLiteralManaged();
    }

    // Gets a mutatable wrapper of a 'kBoolLiterals' entry managed by '_boolLiteralMgr'.
    getBoolLiteralManaged() {
        return this._boolLiteralMgr.getLiteralManaged();
    }

    // Gets a mutatable wrapper of a 'kFieldNames' entry managed by '_fieldNameMgr'.
    getFieldNameManaged() {
        return this._fieldNameMgr.getLiteralManaged();
    }

    // Gets a mutatable wrapper of a 'kFieldNameRefs' entry managed by '_fieldNameRefMgr'.
    getFieldNameRefManaged() {
        return this._fieldNameRefMgr.getLiteralManaged();
    }

    // Gets a mutatable wrapper of a 'kKeyFieldNames' entry managed by '_keyFieldNameMgr'.
    getKeyFieldNameManaged() {
        return this._keyFieldNameMgr.getLiteralManaged();
    }

    // Gets a mutatable wrapper of a 'kSortLiterals' entry managed by '_sortLiteralMgr'.
    getSortLiteralManaged() {
        return this._sortLiteralMgr.getLiteralManaged();
    }

    // Gets a new ValueXyz of any supported type. If a ValueLiteral is returned, it will be managed
    // by '_valueLiteralMgr' so it can be mutated later. 'fieldName' should be an entry from
    // 'kFieldNames' or the wildcard 'kValueAny'.
    getValueManaged(fieldName) {
        return getValueManaged(0, fieldName, this._valueLiteralMgr);
    }

    // Mutates this AggPipeline. This performs exactly one mutation. The mutation is guaranteed to
    // change the query. (There is no guarantee that a subsequent mutation won't change it back to
    // the original.) See also unmutate() and remutate().
    //
    // Ideally we want to mutate a query as little as possible to produce a different result set, as
    // this gives us the greatest chance of matching a cached plan that we should not have matched.
    // Massively mutating a query makes it much more likely that a cached plan will NOT be matched,
    // even if there is some aspect of it that is buggy (e.g. a baked-in value that should have been
    // parameterized or an aspect missing from the cache key).
    mutate() {
        // Choose the type of terminator to mutate proportional to its numbers in the
        // population.
        const len = this._literalMgrs.length;
        const chooseIdx = rngCurr.getRandomInt(0, this._runningSumsTotal);
        for (let i = 0; i < len; ++i) {
            if (chooseIdx < this._runningSums[i]) {
                this._literalMgrs[i].mutate();
                break;
            }
        }
        return this;
    }

    // Remutates all literals in the pipeline, restoring them to their last (most mutated)
    // values.
    remutate() {
        for (let mgr of this._literalMgrs) {
            mgr.remutate();
        }
        return this;
    }

    // Recursively converts the subtree rooted at this stage into an MQL query.
    toMql() {
        let result = [];
        for (let stage of this._self) {
            result.push(stage.toMql());
        }
        return result;
    }

    // Unmutates all literals in the pipeline, restoring them all to their original values.
    unmutate() {
        for (let mgr of this._literalMgrs) {
            mgr.unmutate();
        }
        return this;
    }
}  // class AggPipeline

////////////////////////////////////////////////////////////////////////////////////////////////////
// CLASS Literal
////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * Represents a JavaScript native literal wrapped in a class so a LiteralMgr instance can manage it
 * and make it mutatable in an AggPipeline.
 */
class Literal {
    // 'kLitSource' is an array of the possible JavaScript native literals for this instance.
    constructor(kLitSource) {
        this._kLitSource = kLitSource;

        // An instance is represented as an index '_literalIdx' into array '_kLitSource'.
        this._literalIdx = rngCurr.getRandomInt(0, this._kLitSource.length);
        // Initially undefined data members:
        //   this._origLiteralIdx   - iff mutated, the original '_literalIdx' value
        //   this._mutantLiteralIdx - iff mutated, the newest   '_literalIdx' value
    }

    // Mutates this Literal, guaranteeing the value changes. This also saves a copy of the very
    // first value this object ever had, allowing it to be unmutated back to its original state, and
    // a copy of the last mutated value it had, allowing it to be remutated back to its most mutated
    // state.
    mutate() {
        let idxNew = rngCurr.getRandomInt(0, this._kLitSource.length);
        while (idxNew == this._literalIdx) {
            idxNew = rngCurr.getRandomInt(0, this._kLitSource.length);
        }
        if (this._origLiteralIdx === undefined) {
            this._origLiteralIdx = this._literalIdx;
        }
        this._literalIdx = idxNew;
        this._mutantLiteralIdx = idxNew;
    }

    // Restores the most mutated value of this Literal, regardless of how many times it was mutated.
    remutate() {
        if (this._mutantLiteralIdx !== undefined) {
            this._literalIdx = this._mutantLiteralIdx;
        }
    }
    // Recursively converts the subtree rooted at this stage into an MQL query.
    toMql() {
        return this._kLitSource[this._literalIdx];
    }

    // Restores the original value of this Literal, regardless of how many times it was mutated.
    unmutate() {
        if (this._origLiteralIdx !== undefined) {
            this._literalIdx = this._origLiteralIdx;
        }
    }
}  // class Literal

////////////////////////////////////////////////////////////////////////////////////////////////////
// CLASS LiteralMgr
////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * Manages all the Literals of a given type in a single AggPipeline.
 */
class LiteralMgr {
    // 'kLitSource' is an array of the possible JavaScript native literals to be managed.
    constructor(kLitSource) {
        this._kLitSource = kLitSource;
        this._literals = [];  // all managed literals
    }

    // Returns the number of Literals currently under management.
    getNum() {
        return this._literals.length;
    }

    // Returns a reference to a new Literal which will be managed in '_literals'.
    getLiteralManaged() {
        const result = new Literal(this._kLitSource);
        this._literals.push(result);
        return result;
    }

    // Remutates all managed Literals, restoring them to their last (most mutated) values.
    remutate() {
        for (let lit of this._literals) {
            lit.remutate();
        }
    }

    // Mutates a random managed Literal.
    mutate() {
        const len = this._literals.length;
        if (len == 0) {
            return;
        }
        this._literals[rngCurr.getRandomInt(0, len)].mutate();
    }

    // Unmutates all managed Literals, restoring them to their original values.
    unmutate() {
        for (let lit of this._literals) {
            lit.unmutate();
        }
    }
}  // class LiteralMgr

////////////////////////////////////////////////////////////////////////////////////////////////////
// CLASS ValueArray
////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * Represents an array value.
 */
class ValueArray {
    // 'depth' is the current depth in the doc being created, used to limit the total depth to
    // 'kValueMaxDepth'. If 'valueLiteralManager' is provided, the literals will be managed
    // (mutatable) by that ValueLiteralManager, else raw.
    constructor(depth, valueLiteralManager) {
        // The array instance.
        this._self = [];

        const numEntries = rngCurr.getRandomInt(0, 10);
        for (let entry = 0; entry < numEntries; ++entry) {
            if (valueLiteralManager) {
                this._self.push(getValueManaged(depth + 1, kFieldNameAny, valueLiteralManager));
            } else {
                this._self.push(getValueRaw(depth + 1, kFieldNameAny));
            }
        }
    }

    // Recursively converts the subtree rooted at this stage into an MQL query.
    toMql() {
        return toMqlArray(this._self);
    }
}  // class ValueArray

////////////////////////////////////////////////////////////////////////////////////////////////////
// CLASS ValueObject
////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * Represents an object value.
 */
class ValueObject {
    // 'depth' is the current depth in the doc being created, used to limit the total depth to
    // 'kValueMaxDepth'. If 'valueLiteralManager' is provided, the literals will be managed
    // (mutatable) by that ValueLiteralManager, else raw.
    constructor(depth, valueLiteralManager) {
        // The object instance.
        this._self = {};

        const numFields = rngCurr.getRandomInt(0, 10);
        for (let field = 0; field < numFields; ++field) {
            // Picks a non-duplicate field name.
            let fieldName = getFieldName();
            while (this._self.hasOwnProperty(fieldName)) {
                fieldName = getFieldName();
            }
            if (valueLiteralManager) {
                this._self[fieldName] = getValueManaged(depth + 1, fieldName, valueLiteralManager);
            } else {
                this._self[fieldName] = getValueRaw(depth + 1, fieldName);
            }
        }
    }

    // Recursively converts the subtree rooted at this stage into an MQL query.
    toMql() {
        return toMqlObject(this._self);
    }
}  // class ValueObject

////////////////////////////////////////////////////////////////////////////////////////////////////
// MAIN HELPER FUNCTIONS
////////////////////////////////////////////////////////////////////////////////////////////////////

// Clears the plan cache for the given collection.
function clearPlanCache(collection) {
    assert.commandWorked(db.runCommand({planCacheClear: collection.getName()}));
}

// Recursively sorts all entries in arrays and all keys in objects. Used to avoid mismatches due to
// MQL's various non-determinisms in return value orderings, e.g. the orders in which $group and
// $lookup may produce outputs.
function deepSortObjAndArr(ent) {
    if (Array.isArray(ent)) {
        let result = ent.map(deepSortObjAndArr);
        return result.map(i => JSON.stringify(i)).sort().map(i => JSON.parse(i));
    } else if (typeof ent === 'object' && ent !== null) {
        const sortedObj = {};
        Object.keys(ent).sort().forEach(key => {
            sortedObj[key] = deepSortObjAndArr(ent[key]);
        });
        return sortedObj;
    } else if (Number(ent) === ent && ent % 1 !== 0) {
        // Rounds floating point to one decimal place to avoid mismatches from precision loss in
        // accumulators when values are accumulated in different orders.
        return Math.round(ent * 10) / 10;
    }
    return ent;
}

// Performs aggregation of 'pipeline' against 'collection'. 'pipeline' will be of type AggPipeline
// in normal runs but of type Array in manual repro runs. This does a sort of the stringified result
// because sorting by "_id" in the server, while faster than sorting in JS, has problems:
//   1. The server's final sort by "_id" does not produce a stable ordering in SBE when $group
//      produces an array field for its "_id" output, as SBE uses an unstable sorting algorithm and
//      arrays are only sorted based on their first entries in MQL.
//   2. Pipelines may project away the "_id" field, so a server sort by "_id" does nothing.
function doAggregate(collection, pipeline) {
    // Convert AggPipeline to Array if needed.
    if (pipeline.constructor.name !== "Array") {
        pipeline = pipeline.toMql();
    }
    try {
        return deepSortObjAndArr(collection.aggregate(pipeline).toArray());
    } catch (err) {
        if (err.message.includes("cannot sort with keys that are parallel arrays")) {
            // Absorbs known errors from invalid generated MQL.
            return ["ignored error"];
        }
        throw err;
    }
}

// Compares the result sets of two queries and returns true if they are identical, else false. Both
// of the arguments should always be arrays.
function resultsIdentical(results1, results2) {
    // Short circuit for different cardinalities (optimization for checking whether original and
    // mutant queries have different ground-truth results).
    if (results1.length !== results2.length) {
        return false;
    }
    return JSON.stringify(results1) === JSON.stringify(results2);
}

// Runs a manual repro of just the failing pipeline and its mutant that the user set in
// 'pipelineOrig' and 'pipelineMutant' (along with setting 'rngSeed').
function runManualRepro(pipelineOrig, pipelineMutant) {
    // Saves ground-truth results for the original pipeline.
    clearPlanCache(coll);
    const resultsOrig1 = doAggregate(coll, pipelineOrig);

    // Saves ground-truth results for the mutated pipeline.
    clearPlanCache(coll);
    const resultsMutant1 = doAggregate(coll, pipelineMutant);

    // Rerun the mutant. Since it will now have run twice in a row, its plan cache entry should be
    // active if it gets cached at all. It should still retun the mutant's ground-truth results.
    const resultsMutant2 = doAggregate(coll, pipelineMutant);
    if (!resultsIdentical(resultsMutant1, resultsMutant2)) {
        logRepro(
            pipelineMutant,
            resultsMutant1,
            resultsMutant2,
            "Repro got mismatch between mutant pipeline runs. Plan cache should not be involved.");
    }

    // Rerun the original, which may reuse the mutant's cached plan. It should still return the
    // original's ground-truth results.
    const resultsOrig2 = doAggregate(coll, pipelineOrig);
    if (!resultsIdentical(resultsOrig1, resultsOrig2)) {
        logRepro(pipelineOrig,
                 resultsOrig1,
                 resultsOrig2,
                 "Repro got mismatch between original pipeline runs. Plan cache may have a bug.");
    }
}  // function runManualRepro

////////////////////////////////////////////////////////////////////////////////////////////////////
// MAIN
////////////////////////////////////////////////////////////////////////////////////////////////////

// BEGIN FAILURE REPRO -------------------------------------------------------------------------
// To reproduce a specific query failure without rerunning the entire test, replace the values in
// these assignments to 'rngSeed', 'pipelineOrig', and 'pipelineMutant' with those logged in the
// error. The test will generate the same docs but run only that original-mutant query pair.
//
// To rerun the entire test with a particular seed, replace only 'rngSeed's value with the one
// logged in the original test and leave both 'pipelineOrig' and 'pipelineMutant' undefined.
let rngSeed = undefined;
const pipelineOrig = undefined;
const pipelineMutant = undefined;
// END FAILURE REPRO ---------------------------------------------------------------------------

// The collection for this test.
const coll = db.plan_cache_stochastic_test;

// Max mutations on a pipeline trying to get a mutant with different results from the original.
// Empirical testing indicates trying 2 mutations pays for itself but higher than that does not,
// probably due to highly selective pipelines that match nothing, where mutating them still likely
// matches nothing. We also do not want a large number of mutations as they test the plan cache less
// well, as any one mutation could prevent reuse of a cached plan that then masks an incorrect
// parameterization for a different mutation.
const kMaxMutations = 2;

// Number of documents. Empirical testing shows that with only 10 docs, about 47% of generated
// pipelines have different result sets between the original and mutant, whereas this only improves
// to 52% with 100 docs and 53% with 1,000 docs, but the runtimes of those are 3.5 and 30 times as
// long, respectively. Thus using only 10 docs we can both run a lot more pipelines and make
// debugging failures far easier, as this test does not have a doc minimizer, but we don't really
// need one with only 10 docs.
const kNumDocs = 10;

// Number of generated pipeline pairs (original and mutant).
const kNumPipelines = 5000;

// Random number generators. These should only be accessed through the 'rngCurr' reference.
// 'rngDocs' is used for document generation so the docs can be regenerated after a failed test
// without regenerating, remutating, and rerunning everything else. 'rngMain' is used for all other
// random numbers needed.
if (rngSeed === undefined) {
    print("main: Running a new full test with a new random seed.");
    rngSeed = Date.now();
} else {
    print("main: Rerunning a prior test.");
}
print(`main: rngSeed: ${rngSeed}`);
const rngDocs = new LcgRandom(rngSeed);
const rngMain = new LcgRandom(rngSeed + 1);

// Current random number generator.
let rngCurr = rngMain;

let tested = 0;  // num pipelines with different ground-truth results for original and mutant

print(`main: Dropping collection ${coll.getName()}`);
assert(coll.drop());

print(`main: Creating ${kNumDocs} docs`);
const docs = createDocs(kNumDocs);
print(`main: insertMany ${kNumDocs} docs`);
assert.commandWorked(coll.insertMany(docs));
print(`main: Creating indexes on ${kKeyFieldNames.length} key fields`);
createIndexes(kKeyFieldNames);

if (pipelineOrig) {
    assert(rngSeed && pipelineMutant,
           "main: To rerun a failing query pair, 'rngSeed' and 'pipelineMutant' must be defined.");
    print("main: Running 'pipelineOrig' versus 'pipelineMutant' for repro attempt.");
    runManualRepro(pipelineOrig, pipelineMutant);
} else {
    // Run the full test.
    for (let i = 0; i < kNumPipelines; ++i) {
        print(`main: Running pipeline ${i} of ${kNumPipelines}.`);
        let aggPipeline = new AggPipeline();

        // Saves ground-truth results for the original pipeline.
        clearPlanCache(coll);
        const resultsOrig1 = doAggregate(coll, aggPipeline);
        // Saves ground-truth results for the mutated pipeline. Try several times to get a mutation
        // that produces different results from the original.
        let mutations = 0;
        let found = false;  // did we find a mutation that gave different results from original?
        let resultsMutant1;
        while (!found && mutations < kMaxMutations) {
            aggPipeline.mutate();
            ++mutations;

            clearPlanCache(coll);
            resultsMutant1 = doAggregate(coll, aggPipeline);
            if (!resultsIdentical(resultsOrig1, resultsMutant1)) {
                found = true;
            }
        }

        // The only interesting pipeline mutations for plan cache testing are ones that should
        // return different results from the original, so reuse of an incorrectly parameterized
        // cached plan would be detectable.
        //
        // We will not check whether a query plan got cached or reused from cache as the purpose of
        // this test is to detect whether the plan cache breaks anything, not whether it is being
        // used for a given query shape. Other tests exist to verify that the plan cache is being
        // used or not used as expected. The current test passes for a given twice-run query if a
        // variant of that query run immediately after whose results should be different actually
        // returns the correct, different results. This test does not care whether the results are
        // correct because the query plan was not reused from cache or because it was reused and was
        // correctly parameterized.
        //
        // This also means the test does not need to be updated every time the set of query shapes
        // that get cached changes, and it is agnostic to which plan cache is used (Classic, SBE, or
        // none) and to whatever execution engine or hybrid of execution engines is used.
        if (found) {
            ++tested;

            // Rerun the mutant. Since it will now have run twice in a row, its plan cache entry
            // should be active if it gets cached at all. It should still retun the mutant's
            // ground-truth results.
            const resultsMutant2 = doAggregate(coll, aggPipeline);
            if (!resultsIdentical(resultsMutant1, resultsMutant2)) {
                logFailedTest(
                    aggPipeline,
                    resultsMutant1,
                    resultsMutant2,
                    "Results from two runs of mutant pipeline differ from each other." +
                        " Unexpected - plan cache should not be involved in these two runs.");
            }

            // Rerun the original, which may reuse the mutant's cached plan. It should still return
            // the original's ground-truth results.
            aggPipeline.unmutate();
            const resultsOrig2 = doAggregate(coll, aggPipeline);
            if (!resultsIdentical(resultsOrig1, resultsOrig2)) {
                logFailedTest(
                    aggPipeline,
                    resultsOrig1,
                    resultsOrig2,
                    "Results from two runs of original pipeline differ from each other." +
                        " Possible plan cache bug where improperly parameterized cached mutant plan" +
                        " was reused for second run of original pipeline.");
            }
        }
    }  // for kNumPipelines
    print(`main: ${kNumPipelines} original pipelines were run.`);
    print(`main: ${tested} pipelines (${
        (100.0 * tested / kNumPipelines).toFixed(1)}%) tested the plan cache.`);
    print(`main: ${
        kNumPipelines - tested} pipelines had the same original and mutant ground-truth results.`);
}  // else run full test
