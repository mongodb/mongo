/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/global_catalog/type_namespace_placement_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"

#include <boost/optional/optional.hpp>

namespace mongo {

class HistoricalPlacementFetcher {
public:
    virtual ~HistoricalPlacementFetcher() = default;

    /**
     * Fetches HistoricalPlacement information for the given namespace 'nss' at time
     * 'atClusterTime'. 'checkIfPointInTimeIsInFuture' is passed as false by default as
     * HistoricalPlacementFetcher is used primarily by ChangeStreamShardTargeters that handle events
     * for already running change streams that can not return change events with the cluster time in
     * the future.
     */
    virtual HistoricalPlacement fetch(OperationContext* opCtx,
                                      const boost::optional<NamespaceString>& nss,
                                      Timestamp atClusterTime,
                                      bool checkIfPointInTimeIsInFuture = false) = 0;
};

}  // namespace mongo
