/**
 * @tags: [
 * # 6.2 removes support for atomic applyOps
 * requires_fcv_62,
 * ]
 */
import {ApplyOpsConcurrentTest} from "jstests/replsets/libs/apply_ops_concurrent.js";

new ApplyOpsConcurrentTest({ns1: "test.coll1", ns2: "test.coll2"}).run();
