/**
 * Tests the limits of the product for the DatasetTimeseriesOneField dataset.
 */

import {DatasetTimeseriesOneField} from "jstests/product_limits/libs/datasets.js";

new DatasetTimeseriesOneField().runDataset();
