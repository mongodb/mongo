/**
 * Tests the limits of the product in various dimensions by generating degenerate queries and
 * running them.
 *
 * @tags: [
 *   # Pipeline length is limited to 200 in Atlas
 *   simulate_atlas_proxy_incompatible,
 * ]
 */

import {DATASETS} from "jstests/product_limits/libs/datasets.js";

for (const dataset of DATASETS) {
    let ds = new dataset;
    ds.runDataset();
}
