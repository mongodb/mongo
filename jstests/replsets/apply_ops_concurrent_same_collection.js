/**
 * @tags: [
 * # 6.2 removes support for atomic applyOps
 * requires_fcv_62,
 * ]
 */
import {ApplyOpsConcurrentTest} from "jstests/replsets/libs/apply_ops_concurrent.js";

new ApplyOpsConcurrentTest({ns1: "test.coll", ns2: "test.coll"}).run();
