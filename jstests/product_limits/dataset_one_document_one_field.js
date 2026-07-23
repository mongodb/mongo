/**
 * Tests the limits of the product for the DatasetOneDocumentOneField dataset.
 */

import {DatasetOneDocumentOneField} from "jstests/product_limits/libs/datasets.js";

new DatasetOneDocumentOneField().runDataset();
