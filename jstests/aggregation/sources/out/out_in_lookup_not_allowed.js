// Tests that $out cannot be used within a $lookup pipeline.
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");                     // For assertErrorCode.
    load("jstests/libs/collection_drop_recreate.js");                // For assertDropCollection.
    load("jstests/noPassthrough/libs/server_parameter_helpers.js");  // For setParameterOnAllHosts.
    load("jstests/libs/discover_topology.js");                       // For findDataBearingNodes.
    load("jstests/libs/fixture_helpers.js");                         // For isSharded.

    const ERROR_CODE_OUT_BANNED_IN_LOOKUP = 51047;
    const ERROR_CODE_OUT_LAST_STAGE_ONLY = 40601;
    const coll = db.out_in_lookup_not_allowed;
    coll.drop();

    const from = db.out_in_lookup_not_allowed_from;
    from.drop();

    if (FixtureHelpers.isSharded(from)) {
        setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()),
                               "internalQueryAllowShardedLookup",
                               true);
    }

    let pipeline = [
        {
          $lookup: {
              pipeline: [{$out: "out_collection"}],
              from: from.getName(),
              as: "c",
          }
        },
    ];
    assertErrorCode(coll, pipeline, ERROR_CODE_OUT_BANNED_IN_LOOKUP);

    pipeline = [
        {
          $lookup: {
              pipeline: [{$project: {x: 0}}, {$out: "out_collection"}],
              from: from.getName(),
              as: "c",
          }
        },
    ];

    assertErrorCode(coll, pipeline, ERROR_CODE_OUT_BANNED_IN_LOOKUP);

    pipeline = [
        {
          $lookup: {
              pipeline: [{$out: "out_collection"}, {$match: {x: true}}],
              from: from.getName(),
              as: "c",
          }
        },
    ];

    // Pipeline will fail because $out is not last in the subpipeline.
    // Validation for $out in a $lookup's subpipeline occurs at a later point.
    assertErrorCode(coll, pipeline, ERROR_CODE_OUT_LAST_STAGE_ONLY);

    // Create view which contains $out within $lookup.
    assertDropCollection(coll.getDB(), "view1");

    pipeline = [
        {
          $lookup: {
              pipeline: [{$out: "out_collection"}],
              from: from.getName(),
              as: "c",
          }
        },
    ];

    // Pipeline will fail because $out is not allowed to exist within a $lookup.
    // Validation for $out in a view occurs at a later point.
    const cmdRes =
        coll.getDB().runCommand({create: "view1", viewOn: coll.getName(), pipeline: pipeline});
    assert.commandFailedWithCode(cmdRes, ERROR_CODE_OUT_BANNED_IN_LOOKUP);
}());
