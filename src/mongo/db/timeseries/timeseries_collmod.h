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

#pragma once

#include "mongo/db/coll_mod_gen.h"

namespace mongo {
namespace timeseries {

/**
 * Returns a CollMod on the underlying buckets collection of the time-series collection.
 * Returns null if 'origCmd' is not for a time-series collection.
 */
std::unique_ptr<CollMod> makeTimeseriesBucketsCollModCommand(OperationContext* opCtx,
                                                             const CollMod& origCmd);

/**
 * Returns a CollMod on the view definition of the time-series collection.
 * Returns null if 'origCmd' is not for a time-series collection or if the view definition need not
 * be changed.
 */
std::unique_ptr<CollMod> makeTimeseriesViewCollModCommand(OperationContext* opCtx,
                                                          const CollMod& origCmd);

/**
 * Performs the collection modification described in "cmd" on the collection "nss". May perform
 * timeseries view translation to multiple collMod if "performViewChange" flag is set.
 */
Status processCollModCommandWithTimeSeriesTranslation(OperationContext* opCtx,
                                                      const NamespaceString& nss,
                                                      const CollMod& cmd,
                                                      bool performViewChange,
                                                      BSONObjBuilder* result);

}  // namespace timeseries
}  // namespace mongo
