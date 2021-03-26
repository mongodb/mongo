/**
 *    Copyright (C) 2021-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/timeseries/timeseries_lookup.h"

#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/views/view_catalog.h"

namespace mongo {

namespace timeseries {

boost::optional<TimeseriesOptions> getTimeseriesOptions(OperationContext* opCtx,
                                                        const NamespaceString& nss) {
    auto viewCatalog = DatabaseHolder::get(opCtx)->getViewCatalog(opCtx, nss.db());
    if (!viewCatalog) {
        return {};
    }

    auto view = viewCatalog->lookupWithoutValidatingDurableViews(opCtx, nss.ns());
    if (!view) {
        return {};
    }

    // Return a copy of the time-series options so that we do not refer to the internal state of
    // 'viewCatalog' after it goes out of scope.
    return view->timeseries();
}

}  // namespace timeseries
}  // namespace mongo
