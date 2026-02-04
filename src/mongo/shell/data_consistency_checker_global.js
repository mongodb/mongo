// Populate global variables from modules for backwards compatibility

import {
    CollInfos,
    DataConsistencyChecker,
    filterGetAllCollectionsExcludingViews,
    filterGetAllCollectionsExcludingViewsAndTimeseries,
} from "src/mongo/shell/data_consistency_checker.js";

globalThis.CollInfos = CollInfos;
globalThis.DataConsistencyChecker = DataConsistencyChecker;
globalThis.filterGetAllCollectionsExcludingViews = filterGetAllCollectionsExcludingViews;
globalThis.filterGetAllCollectionsExcludingViewsAndTimeseries = filterGetAllCollectionsExcludingViewsAndTimeseries;
