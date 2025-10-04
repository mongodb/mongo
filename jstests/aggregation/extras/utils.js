import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

/**
 * Compute the result of evaluating 'expression', and compare it to 'result'. Replaces the contents
 * of 'coll' with a single empty document.
 */
export function testExpression(coll, expression, result) {
    testExpressionWithCollation(coll, expression, result);
}

/**
 * Compute the result of evaluating 'expression', and compare it to 'result', using 'collationSpec'
 * as the collation spec. Replaces the contents of 'coll' with a single empty document.
 */
export function testExpressionWithCollation(coll, expression, result, collationSpec) {
    assert.commandWorked(coll.remove({}));
    assert.commandWorked(coll.insert({}));

    const options = collationSpec !== undefined ? {collation: collationSpec} : undefined;

    const res = coll.aggregate([{$project: {output: expression}}], options).toArray();

    assert.eq(res.length, 1, tojson(res));
    assert.eq(res[0].output, result, tojson(res));
}

export function _getObjectSubtypeOrUndefined(o) {
    function isNumberLong(v) {
        return v instanceof NumberLong;
    }
    function isNumberInt(v) {
        return v instanceof NumberInt;
    }
    function isNumberDecimal(v) {
        return v instanceof NumberDecimal;
    }
    function isObjectId(v) {
        return v instanceof ObjectId;
    }
    function isDate(v) {
        return v instanceof Date;
    }
    function isTimestamp(v) {
        return v instanceof Timestamp;
    }
    function isArray(v) {
        return v instanceof Array;
    }

    const objectSubtypes = [
        {typeName: "NumberLong", isSameSubtype: isNumberLong},
        {typeName: "NumberInt", isSameSubtype: isNumberInt},
        {typeName: "NumberDecimal", isSameSubtype: isNumberDecimal},
        {typeName: "ObjectId", isSameSubtype: isObjectId},
        {typeName: "Date", isSameSubtype: isDate},
        {typeName: "Timestamp", isSameSubtype: isTimestamp},
        {typeName: "Array", isSameSubtype: isArray},
    ];

    for (const subtype of objectSubtypes) {
        if (subtype.isSameSubtype(o)) {
            return subtype;
        }
    }
    return undefined;
}

/**
 * Compare using valueComparator if provided, or the default otherwise. Assumes al and ar have the
 * same type.
 */
export function _uncheckedCompare(al, ar, valueComparator) {
    // bsonBinaryEqual would return false for NumberDecimal("0.1") and NumberDecimal("0.100").
    return valueComparator ? valueComparator(al, ar) : al === ar || bsonWoCompare(al, ar) === 0;
}

/**
 * Returns true if 'al' is the same as 'ar'. If the two are arrays, the arrays can be in any order.
 * Objects (either 'al' and 'ar' themselves, or embedded objects) must have all the same properties.
 * If 'al' and 'ar' are neither object nor arrays, they must compare equal using 'valueComparator',
 * or == if not provided.
 */
export function anyEq(al, ar, verbose = false, valueComparator, fieldsToSkip = []) {
    // Helper to log 'msg' iff 'verbose' is true.
    const debug = (msg) => (verbose ? print(msg) : null);

    if (al instanceof Object && ar instanceof Object) {
        const alSubtype = _getObjectSubtypeOrUndefined(al);
        if (alSubtype) {
            // One of the supported subtypes, make sure ar is of the same type.
            if (!alSubtype.isSameSubtype(ar)) {
                debug("anyEq: ar is not instanceof " + alSubtype.typeName + " " + tojson(ar));
                return false;
            }

            if (al instanceof Array) {
                if (!arrayEq(al, ar, verbose, valueComparator, fieldsToSkip)) {
                    debug(`anyEq: arrayEq(al, ar): false; al=${tojson(al)}, ar=${tojson(ar)}`);
                    return false;
                }
            } else if (!_uncheckedCompare(al, ar, valueComparator)) {
                debug(`anyEq: (al != ar): false; al=${tojson(al)}, ar=${tojson(ar)}`);
                return false;
            }
        } else {
            const arType = _getObjectSubtypeOrUndefined(ar);
            if (arType) {
                // If al was not of any of the subtypes, but ar is, then types are different.
                debug("anyEq: al is " + typeof al + " but ar is " + arType.typeName);
                return false;
            }

            // Default to comparing object fields.
            if (!documentEq(al, ar, verbose, valueComparator, fieldsToSkip)) {
                debug(`anyEq: documentEq(al, ar): false; al=${tojson(al)}, ar=${tojson(ar)}`);
                return false;
            }
        }
    } else if (!_uncheckedCompare(al, ar, valueComparator)) {
        // One of the operands, or both, is not an object. If one of them is not an object, but the
        // other is, the default compare will return false. If both are not an object, default
        // comparison should work fine. In all cases, if the value comparator is provided, it should
        // be used, even for different types.
        debug(`anyEq: (al != ar): false; al=${tojson(al)}, ar=${tojson(ar)}`);
        return false;
    }

    debug(`anyEq: these are equal: ${tojson(al)} == ${tojson(ar)}`);
    return true;
}

/**
 * Compares two documents for equality using a custom comparator for the values which returns true
 * or false. Returns true or false. Only equal if they have the exact same set of properties, and
 * all the properties' values match according to 'valueComparator'.
 */
export function customDocumentEq({left, right, verbose, valueComparator, fieldsToSkip = []}) {
    return documentEq(left, right, verbose, valueComparator, fieldsToSkip);
}

/**
 * Compare two documents for equality. Returns true or false. Only equal if they have the exact same
 * set of properties, and all the properties' values match except the values with names in the
 * fieldsToSkip array. The fields in fieldsToSkip will be skipped at all levels of the document.
 * The value comparison with the recursive anyEq function allows for comparing nested array values
 * ignoring the elements' order.
 * If the order of the nested arrays elements is significant for the equivalence, the assert.docEq
 * from assert.js should be used instead.
 */
export function documentEq(dl, dr, verbose = false, valueComparator, fieldsToSkip = []) {
    const debug = (msg) => (verbose ? print(msg) : null); // Helper to log 'msg' iff 'verbose' is true.

    // Make sure these are both objects.
    if (!(dl instanceof Object)) {
        debug("documentEq:  dl is not an object " + tojson(dl));
        return false;
    }
    if (!(dr instanceof Object)) {
        debug("documentEq:  dr is not an object " + tojson(dr));
        return false;
    }

    // Start by checking for all of dl's properties in dr.
    for (let propertyName in dl) {
        // Skip inherited properties.
        if (!dl.hasOwnProperty(propertyName)) continue;

        if (fieldsToSkip.includes(propertyName)) continue;

        // The documents aren't equal if they don't both have the property.
        if (!dr.hasOwnProperty(propertyName)) {
            debug("documentEq: dr doesn't have property " + propertyName);
            return false;
        }

        if (!anyEq(dl[propertyName], dr[propertyName], verbose, valueComparator, fieldsToSkip)) {
            return false;
        }
    }

    // Now make sure that dr doesn't have any extras that dl doesn't have.
    for (let propertyName in dr) {
        if (!dr.hasOwnProperty(propertyName)) continue;

        if (fieldsToSkip.includes(propertyName)) continue;

        // If dl doesn't have this they are not equal; if it does, we compared it above and know it
        // to be equal.
        if (!dl.hasOwnProperty(propertyName)) {
            debug("documentEq: dl is missing property " + propertyName);
            return false;
        }
    }

    debug(`documentEq: these are equal: ${tojson(dl)} == ${tojson(dr)}`);
    return true;
}

/**
 * Returns true if both 'al' and 'ar' are arrays of the same length with the same elements according
 * to valueComparator.  Order of the elements within the arrays is not significant.
 *
 * Element comparison uses the anyEq function recursively, which allows for comparing of nested
 * arrays with insignificant order.
 *
 * Use this function if the arguments have nested arrays and the element order is *not* significant
 * when the equivalence is determined. Use assert.sameMembers() in assert.js instead if the
 * arguments have no nested arrays, or the order of the nested arrays is significant for the
 * equivalent assertion.
 */
export function arrayEq(al, ar, verbose = false, valueComparator, fieldsToSkip = []) {
    const debug = (msg) => (verbose ? print(msg) : null); // Helper to log 'msg' iff 'verbose' is true.

    // Check that these are both arrays.
    if (!(al instanceof Array)) {
        debug("arrayEq: al is not an array: " + tojson(al));
        return false;
    }

    if (!(ar instanceof Array)) {
        debug("arrayEq: ar is not an array: " + tojson(ar));
        return false;
    }

    if (al.length != ar.length) {
        debug(`arrayEq:  array lengths do not match ${tojson(al)}, ${tojson(ar)}`);
        return false;
    }

    // Keep a set of which indexes we've already used to avoid considering [1,1] as equal to [1,2].
    const matchedElementsInRight = new Set();
    for (let leftElem of al) {
        let foundMatch = false;
        for (let i = 0; i < ar.length; ++i) {
            if (!matchedElementsInRight.has(i) && anyEq(leftElem, ar[i], verbose, valueComparator, fieldsToSkip)) {
                matchedElementsInRight.add(i); // Don't use the same value each time.
                foundMatch = true;
                break;
            }
        }
        if (!foundMatch) {
            return false;
        }
    }

    return true;
}

export function arrayDiff(al, ar, verbose = false, valueComparator, fieldsToSkip = []) {
    // Check that these are both arrays.
    if (!(al instanceof Array)) {
        debug("arrayDiff: al is not an array: " + tojson(al));
        return false;
    }

    if (!(ar instanceof Array)) {
        debug("arrayDiff: ar is not an array: " + tojson(ar));
        return false;
    }

    // Keep a set of which indexes we've already used to avoid considering [1,1] as equal to [1,2].
    const matchedIndexesInRight = new Set();
    let unmatchedElementsInLeft = [];
    for (let leftElem of al) {
        let foundMatch = false;
        for (let i = 0; i < ar.length; ++i) {
            if (!matchedIndexesInRight.has(i) && anyEq(leftElem, ar[i], verbose, valueComparator, fieldsToSkip)) {
                matchedIndexesInRight.add(i); // Don't use the same value each time.
                foundMatch = true;
                break;
            }
        }
        if (!foundMatch) {
            unmatchedElementsInLeft.push(leftElem);
        }
    }
    let unmatchedElementsInRight = [];
    for (let i = 0; i < ar.length; ++i) {
        if (!matchedIndexesInRight.has(i)) {
            unmatchedElementsInRight.push(ar[i]);
        }
    }
    return {left: unmatchedElementsInLeft, right: unmatchedElementsInRight};
}

/**
 * Makes a shallow copy of 'a'.
 */
export function arrayShallowCopy(a) {
    assert(a instanceof Array, "arrayShallowCopy: argument is not an array");
    return a.slice(); // Makes a copy.
}

/**
 * Compare two sets of documents (expressed as arrays) to see if they match. The two sets must have
 * the same documents, although the order need not match and values for fields defined in
 * "fieldsToSkip" need not match.
 *
 * Are non-scalar values references?
 */
export function resultsEq(rl, rr, verbose = false, fieldsToSkip = []) {
    const debug = (msg) => (verbose ? print(msg) : null); // Helper to log 'msg' iff 'verbose' is true.

    // Make clones of the arguments so that we don't damage them.
    rl = arrayShallowCopy(rl);
    rr = arrayShallowCopy(rr);

    if (rl.length != rr.length) {
        debug(`resultsEq:  array lengths do not match ${tojson(rl)}, ${tojson(rr)}`);
        return false;
    }

    for (let i = 0; i < rl.length; ++i) {
        let foundIt = false;

        // Find a match in the other array.
        for (let j = 0; j < rr.length; ++j) {
            if (!anyEq(rl[i], rr[j], verbose, null, fieldsToSkip)) continue;

            debug(`resultsEq: search target found (${tojson(rl[i])}) (${tojson(rr[j])})`);

            // Because we made the copies above, we can edit these out of the arrays so we don't
            // check on them anymore.
            // For the inner loop, we're going to be skipping out, so we don't need to be too
            // careful.
            rr.splice(j, 1);
            foundIt = true;
            break;
        }

        if (!foundIt) {
            // If we got here, we didn't find this item.
            debug(`resultsEq: search target missing index ${i} (${tojson(rl[i])})`);
            return false;
        }
    }

    assert(!rr.length);
    return true;
}

/**
 * Returns true if both 'al' and 'ar' are arrays of the same length with the same elements.
 * Order of the elements is significant only in the top-level arrays.
 *
 * Element comparison uses the anyEq function recursively, which allows for comparing of nested
 * arrays ignoring the elements' order.
 *
 * Use this function if the arguments have nested arrays and the elements' order is significant at
 * the top-level and insignificant for the nested arrays.
 */
export function orderedArrayEq(al, ar, verbose = false, fieldsToSkip = []) {
    if (al.length != ar.length) {
        if (verbose) print(`orderedArrayEq:  array lengths do not match ${tojson(al)}, ${tojson(ar)}`);
        return false;
    }

    for (let i = 0; i < al.length; ++i) {
        if (!anyEq(al[i], ar[i], verbose, null, fieldsToSkip)) return false;
    }

    return true;
}

/**
 * Assert that the given aggregation fails with a specific code. Error message is optional. Note
 * that 'code' can be an array of possible codes. If target is a database instead of a collection
 * this function will run a collectionless aggregate command.
 */
export function assertErrorCode(target, pipe, code, errmsg, options = {}) {
    if (!Array.isArray(pipe)) {
        pipe = [pipe];
    }

    let cmd = {pipeline: pipe, cursor: {batchSize: 0}};
    for (let opt of Object.keys(options)) {
        cmd[opt] = options[opt];
    }

    let againstDB = target instanceof DB;
    let targetDB = againstDB ? target : target.getDB();
    let ns = againstDB ? 1 : target.getName();
    let cmdWithNS = Object.assign({}, {aggregate: ns}, cmd);
    let resultObj = targetDB.runCommand(cmdWithNS);
    if (resultObj.ok) {
        let followupBatchSize = 0; // default
        let cursor = new DBCommandCursor(targetDB, resultObj, followupBatchSize);
        let assertThrowsMsg = "expected one of the following error codes: " + tojson(code);
        resultObj = assert.throws(() => cursor.itcount(), [], assertThrowsMsg);
    }

    assert.commandFailedWithCode(resultObj, code, errmsg);
}

/**
 * Assert that an aggregation fails with a specific code and the error message contains the given
 * string. Note that 'code' can be an array of possible codes.
 */
export function assertErrCodeAndErrMsgContains(coll, pipe, code, expectedMessage) {
    const response = assert.commandFailedWithCode(
        coll.getDB().runCommand({aggregate: coll.getName(), pipeline: pipe, cursor: {}}),
        code,
    );
    assert.neq(
        -1,
        response.errmsg.indexOf(expectedMessage),
        "Error message did not contain '" + expectedMessage + "', found:\n" + tojson(response),
    );
}

/**
 * Assert that an aggregation ran on admin DB fails with a specific code and the error message
 * contains the given string. Note that 'code' can be an array of possible codes.
 */
export function assertAdminDBErrCodeAndErrMsgContains(coll, pipe, code, expectedMessage) {
    const response = assert.commandFailedWithCode(
        coll.getDB().adminCommand({aggregate: 1, pipeline: pipe, cursor: {}}),
        code,
    );
    assert.neq(
        -1,
        response.errmsg.indexOf(expectedMessage),
        "Error message did not contain '" + expectedMessage + "', found:\n" + tojson(response),
    );
}

/**
 * Assert that an aggregation fails with any code and the error message contains the given
 * string.
 */
export function assertErrMsgContains(coll, pipe, expectedMessage) {
    const response = assert.commandFailed(
        coll.getDB().runCommand({aggregate: coll.getName(), pipeline: pipe, cursor: {}}),
    );
    assert.neq(
        -1,
        response.errmsg.indexOf(expectedMessage),
        "Error message did not contain '" + expectedMessage + "', found:\n" + tojson(response),
    );
}

/**
 * Assert that an aggregation fails with any code and the error message does not contain the given
 * string.
 */
export function assertErrMsgDoesNotContain(coll, pipe, expectedMessage) {
    const response = assert.commandFailed(
        coll.getDB().runCommand({aggregate: coll.getName(), pipeline: pipe, cursor: {}}),
    );
    assert.eq(-1, response.errmsg.indexOf(expectedMessage), "Error message contained '" + expectedMessage + "'");
}

/**
 * Asserts that two arrays are equal - that is, if their sizes are equal and each element in
 * the 'actual' array has a matching element in the 'expected' array, without honoring elements
 * order.
 */
export function assertArrayEq({actual = [], expected = [], fieldsToSkip = [], extraErrorMsg = ""} = {}) {
    assert.eq(arguments.length, 1, "assertArrayEq arguments must be in an object");
    assert(
        arrayEq(actual, expected, false, null, fieldsToSkip),
        `actual=${tojson(actual)}, expected=${tojson(expected)}${extraErrorMsg}`,
    );
}

/**
 * Generates the 'numDocs' number of documents each of 'docSize' size and inserts them into the
 * collecton 'coll'. Each document is generated from the 'template' function, which, by default,
 * returns a document in the form of {_id: i}, where 'i' is the iteration index, starting from 0.
 * The 'template' function is called on each iteration and can take three arguments and return
 * any JSON document which will be used as a document template:
 *   - 'itNum' - the current iteration number in the range [0, numDocs)
 *   - 'docSize' - is the 'docSize' parameter passed to 'generateCollection'
 *   - 'numDocs' - is the 'numDocs' parameter passed to 'generateCollection'
 *
 * After a document is generated from the template, it will be assigned a new field called 'padding'
 * holding a repeating string of 'x' characters, so that the total size of the generated object
 * equals to 'docSize'.
 */
export function generateCollection({
    coll = null,
    numDocs = 0,
    docSize = 0,
    template = (itNum) => {
        return {_id: itNum};
    },
} = {}) {
    assert(coll, "Collection not provided");

    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; ++i) {
        const doc = Object.assign({padding: ""}, template(i, docSize, numDocs));
        const len = docSize - Object.bsonsize(doc);
        assert.lte(0, len, `Document is already bigger than ${docSize} bytes: ${tojson(doc)}`);

        doc.padding = "x".repeat(len);
        assert.eq(
            docSize,
            Object.bsonsize(doc),
            `Generated document's size doesn't match requested document's size: ${tojson(doc)}`,
        );

        bulk.insert(doc);
    }

    const res = bulk.execute();
    assert.commandWorked(res);
    assert.eq(numDocs, res.nInserted);
    assert.eq(numDocs, coll.find().itcount());
}

/**
 * Returns true if 'coll' exists or false otherwise.
 */
export function collectionExists(coll) {
    return Array.contains(coll.getDB().getCollectionNames(), coll.getName());
}

/**
 * Runs and asserts an explain command for an aggregation with the given stage. Returns just the
 * pipeline from the explain results regardless of cluster topology.
 */
export function desugarSingleStageAggregation(db, coll, stage) {
    return getExplainedPipelineFromAggregation(db, coll, [stage]);
}

/**
 * Runs and asserts an explain command for an aggregation with the given pipeline. Returns just the
 * pipeline from the explain results regardless of cluster topology.
 * The fourth parameter `options` is for a few options for unusual scenarios.
 * options.inhibitOptimization defaults to true. This prepends an inhibitOptimization stage to the
 * query and removes it before returning results. This is sub ideal for views. options.hint is an
 * optional hint that will get passed on to the aggregation stage. It defaults to undefined.
 */
export function getExplainedPipelineFromAggregation(
    db,
    coll,
    pipeline,
    {inhibitOptimization = true, postPlanningResults = false, hint} = {},
) {
    // Prevent stages from being absorbed into the .find() layer
    if (inhibitOptimization) {
        pipeline.unshift({$_internalInhibitOptimization: {}});
    }

    const aggOptions = hint ? {hint: hint} : {};

    const result = coll.explain().aggregate(pipeline, aggOptions);

    assert.commandWorked(result);
    return getExplainPipelineFromAggregationResult(result, {inhibitOptimization, postPlanningResults});
}

export function getExplainPipelineFromAggregationResult(
    result,
    {inhibitOptimization = true, postPlanningResults = false} = {},
) {
    if (Array.isArray(result.stages)) {
        // The first two stages should be the .find() cursor and the inhibit-optimization stage (if
        // enabled); the rest of the stages are what the user's 'stage' expanded to.
        assert(result.stages[0].$cursor, result);
        if (inhibitOptimization) {
            assert(result.stages[1].$_internalInhibitOptimization, result);
            return result.stages.slice(2);
        } else {
            return result.stages.slice(1);
        }
    } else {
        if (result.splitPipeline) {
            let shardsPart = null;
            if (!postPlanningResults) {
                shardsPart = result.splitPipeline.shardsPart;
            } else {
                assert.lt(0, Object.keys(result.shards).length, result);
                // Pick an arbitrary shard to look at.
                const shardName = Object.keys(result.shards)[0];
                const shardResult = result.shards[shardName];
                // The shardsPart is either a pipeline or a find-like plan. If it's a find-like
                // plan, wrap it in a $cursor stage so we can combine it into one big pipeline with
                // mergerPart.
                if (Array.isArray(shardResult.stages)) {
                    shardsPart = shardResult.stages;
                } else {
                    assert(
                        shardResult.queryPlanner,
                        `Expected result.shards[${tojson(shardName)}] to be a pipeline, or find-like plan: ` +
                            tojson(result),
                    );
                    shardsPart = [{$cursor: shardResult}];
                }
            }
            if (inhibitOptimization) {
                assert(result.splitPipeline.shardsPart[0].$_internalInhibitOptimization, result);
                shardsPart = shardsPart.slice(1);
            }
            assert(result.splitPipeline.mergerPart[0].$mergeCursors, result);
            const mergerPart = result.splitPipeline.mergerPart.slice(1);
            return [].concat(shardsPart).concat(mergerPart);
        } else if (result.stages) {
            // Required for aggregation_mongos_passthrough.
            assert(Array.isArray(result.stages), result);
            // The first two stages should be the .find() cursor and the inhibit-optimization stage
            // (if enabled); the rest of the stages are what the user's 'stage' expanded to.
            assert(result.stages[0].$cursor, result);
            if (inhibitOptimization) {
                assert(result.stages[1].$_internalInhibitOptimization, result);
                return result.stages.slice(2);
            } else {
                return result.stages.slice(1);
            }
        } else {
            // Required for aggregation_one_shard_sharded_collections.
            assert.lt(0, Object.keys(result.shards).length, result);
            const shardResult = result.shards[Object.keys(result.shards)[0]];
            assert(Array.isArray(shardResult.stages), result);
            assert(shardResult.stages[0].$cursor, result);
            if (inhibitOptimization) {
                assert(shardResult.stages[1].$_internalInhibitOptimization, result);
                return shardResult.stages.slice(2);
            } else {
                return shardResult.stages.slice(1);
            }
        }
    }
}

/**
 * Returns a string that represents the provided array of documents / Objects.
 * Typically used in debugging or assertion messages.
 */
export function stringifyArray(ar, arName = null) {
    let str = "";
    if (arName != null) {
        assert(typeof arName == "string", "provided arName is not a string");
        str += "'" + arName + "' array: ";
    }
    str += "[";
    if (ar.length != 0) {
        str += "\n";
        ar.forEach((element) => {
            str += "    " + tojson(element) + "\n";
        });
    }
    str += "]\n";
    return str;
}
