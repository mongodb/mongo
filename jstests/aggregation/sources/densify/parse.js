/**
 * Test the syntax of $densify.
 * @tags: [
 *   # Needed as $densify is a 51 feature.
 *   requires_fcv_51,
 * ]
 */

import {parseUtil} from "jstests/aggregation/sources/densify/libs/parse_util.js";

const coll = db.densify_parse;
coll.drop();

parseUtil(db, coll, "$densify");
