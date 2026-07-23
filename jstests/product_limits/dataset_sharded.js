/**
 * Tests the limits of the product for the DatasetSharded dataset.
 */

import {DatasetSharded} from "jstests/product_limits/libs/datasets.js";

new DatasetSharded().runDataset();
