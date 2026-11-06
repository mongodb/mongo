/**
 * $replaceRoot and $replaceWith must fail when the new root is not a document, even when no later
 * stage reads that new root. Runs the pipelines in classic, and in SBE with and without
 * featureFlagSbeBlueprintRewrites, whose dead slot elimination must not remove the check along with
 * the unread new root.
 */
import {getEngine} from "jstests/libs/query/analyze_plan.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

const pipelines = [
    [{$replaceWith: "$x"}, {$replaceWith: {ok: 1}}],
    [{$replaceRoot: {newRoot: "$x"}}, {$replaceRoot: {newRoot: {ok: 1}}}],
    [{$replaceWith: "$x"}, {$project: {_id: 0, ok: {$literal: 1}}}],
];

for (const [name, engine, setParameter] of [
    ["classic", "classic", {internalQueryFrameworkControl: "forceClassicEngine"}],
    [
        "SBE without blueprint rewrites",
        "sbe",
        {internalQueryFrameworkControl: "trySbeEngine", featureFlagSbeBlueprintRewrites: false},
    ],
    [
        "SBE with blueprint rewrites",
        "sbe",
        {internalQueryFrameworkControl: "trySbeEngine", featureFlagSbeBlueprintRewrites: true},
    ],
]) {
    describe(`unread non-document new root in ${name}`, function () {
        let conn;
        let coll;

        before(function () {
            conn = MongoRunner.runMongod({setParameter});
            coll = conn.getDB(jsTestName())[jsTestName()];
            assert.commandWorked(coll.insert({_id: 0, x: 1}));
        });

        after(function () {
            MongoRunner.stopMongod(conn);
        });

        for (const pipeline of pipelines) {
            it(`fails for ${tojsononeline(pipeline)}`, function () {
                const context = {setParameter, pipeline};
                assert.eq(
                    getEngine(coll.explain().aggregate(pipeline)),
                    engine,
                    "pipeline ran in an unexpected engine",
                    context,
                );
                assert.throwsWithCode(
                    () => coll.aggregate(pipeline).toArray(),
                    [8105800, 40228],
                    [],
                    "pipeline did not fail",
                    context,
                );
            });
        }
    });
}
