/**
 * Tests the limits of the product in various dimensions by generating degenerate queries and
 * running them.
 */

import {DATASETS} from "jstests/product_limits/libs/datasets.js";

for (const dataset of DATASETS) {
    let ds = new dataset();
    ds.runDataset();
}
