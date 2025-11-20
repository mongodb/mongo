/**
 * Tests a query which renames a field and then returns documents where the value of the renamed
 * field is either true or "truthy".
 *
 * This test was originally designed to reproduce SERVER-113319.
 */

import {show} from "jstests/libs/golden_test.js";
import {leafs, unaryDocs} from "jstests/query_golden/libs/example_data.js";

const docs = unaryDocs("a", leafs());
// Also include a document where "a" is missing.
docs.push({});

const coll = db.renamed_field_as_expression_root;
coll.drop();

jsTestLog("Inserting docs:");
show(docs);
coll.insert(docs);

let pipeline = [{$project: {_id: 0, newA: "$a"}}, {$match: {$expr: "$newA"}}];
jsTestLog("Result of query:");
show(coll.aggregate(pipeline));
