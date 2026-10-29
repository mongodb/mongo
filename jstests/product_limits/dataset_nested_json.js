/**
 * Tests the limits of the product for the DatasetNestedJSON dataset.
 */

import {DatasetNestedJSON} from "jstests/product_limits/libs/datasets.js";

new DatasetNestedJSON().runDataset();
