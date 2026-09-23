/**
 * This test ensures that $lookup internal-only fields ($_internalFieldMatchPipelineIdx,
 * $_internalFromIsAView) sent by an external client correctly fail to parse, as these are set only
 * by the router on the mongos->shard dispatch path.
 * @tags: [
 *   requires_fcv_91,
 * ]
 */
const local = db.lookup_internal_fields_local;
const foreign = db.lookup_internal_fields_foreign;
local.drop();
foreign.drop();

assert.commandWorked(local.insert({_id: 0, fk: 1}));
assert.commandWorked(foreign.insert({_id: 1, x: 1}));

function runLookup(extraLookupFields) {
    return db.runCommand({
        aggregate: local.getName(),
        pipeline: [
            {
                $lookup: Object.assign(
                    {
                        from: foreign.getName(),
                        localField: "fk",
                        foreignField: "x",
                        // Combined localField/foreignField + pipeline syntax populates
                        // _fieldMatchPipelineIdx, which is what makes the
                        // $_internalFieldMatchPipelineIdx case reach relocateFieldMatchPlaceholder().
                        pipeline: [{$match: {x: {$gte: 0}}}],
                        as: "joined",
                    },
                    extraLookupFields,
                ),
            },
        ],
        cursor: {},
    });
}

// Error code 5491300 is thrown by assertAllowedInternalIfRequired for a field that is only allowed
// from an internal client (same code the search-stage internal-field guards use).
const kInternalOnlyErrorCode = 5491300;

assert.commandFailedWithCode(
    runLookup({$_internalFieldMatchPipelineIdx: NumberLong(999999)}),
    kInternalOnlyErrorCode,
    "external $_internalFieldMatchPipelineIdx must be rejected",
);

assert.commandFailedWithCode(
    runLookup({$_internalFromIsAView: true}),
    kInternalOnlyErrorCode,
    "external $_internalFromIsAView must be rejected",
);

// Sanity: the same $lookup without any internal fields must still succeed.
assert.commandWorked(runLookup({}), "plain $lookup should still parse and run");
