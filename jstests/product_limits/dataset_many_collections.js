/**
 * Tests the limits of the product for the DatasetManyCollections dataset.
 */

import {DatasetManyCollections} from "jstests/product_limits/libs/datasets.js";

new DatasetManyCollections().runDataset();
