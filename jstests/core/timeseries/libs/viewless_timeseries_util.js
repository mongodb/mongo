/*
 * TODO SERVER-101609 remove this library once 9.0 becomes lastLTS
 * By then only viewless timeseries will exists so we won't need these functionalities
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

export function areViewlessTimeseriesEnabled(conn) {
    return FeatureFlagUtil.isPresentAndEnabled(db, "CreateViewlessTimeseriesCollections");
}
