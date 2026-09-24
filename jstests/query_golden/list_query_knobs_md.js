/**
 * Golden test asserting on the output of the $listQueryKnobs aggregation stage.
 *
 * Any change to the set of PQS-settable query knobs, or to a knob's wire name, type, or
 * constraints will show up as a diff in the expected output and must be explicitly accepted
 * (see docs/golden_data_test_framework.md). Server parameter names and default values are
 * intentionally not asserted: they can change without breaking PQS compatibility.
 *
 * @tags: [
 *     requires_fcv_90,
 *     do_not_wrap_aggregations_in_facets,
 * ]
 */
import {tojsonMultiLineSortKeys} from "jstests/libs/query_optimization/golden_test.js";
import {code, line, linebreak, section, subSection} from "jstests/libs/query/pretty_md.js";

const adminDB = db.getSiblingDB("admin");

// Only PQS-settable knobs are reported: they are the ones exposable via query settings.
// Projected out:
// - 'name' (the C++ server parameter name) and 'default': neither is part of the PQS wire
//   contract asserted here, and both are allowed to change without breaking compatibility.
// - 'pqsSettable': always true after the $match.
// Sorted by wire name (unique across PQS-settable knobs), so that output is independent of
// registration order and diffs stay incremental as knobs are added, removed, or changed.
const pipeline = [
    {$listQueryKnobs: {}},
    {$match: {pqsSettable: true}},
    {$unset: ["name", "default", "pqsSettable"]},
    {$sort: {wireName: 1}},
];

section("Pipeline");
code(tojsonMultiLineSortKeys(pipeline));
linebreak();
line("PQS-settable query knobs need to be backwards compatible: wire names, types, and");
line("validator ranges. When doing old -> new binary rollouts, persisted query settings should");
line("continue working. For adding new knobs and/or relaxing existing ones, authors should");
line("consider migration work to ensure that all persisted knobs remain valid on downgrades.");
linebreak();
line("The C++ server parameter name, the default value, and pqsSettable are projected out:");
line("they are not part of the PQS wire contract and can change without breaking compatibility.");
linebreak();

section("Output");
const knobs = adminDB.aggregate(pipeline).toArray();
line(`Number of PQS-settable knobs: ${knobs.length}`);
linebreak();
for (const knob of knobs) {
    subSection(knob.wireName);
    code(tojsonMultiLineSortKeys(knob));
    linebreak();
}
