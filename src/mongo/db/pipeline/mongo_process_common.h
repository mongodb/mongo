/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/mongo_process_interface.h"

namespace mongo {

/**
 * MongoProcessCommon provides base implementations of any MongoProcessInterface methods whose code
 * is largely identical on mongoD and mongoS.
 */
class MongoProcessCommon : public MongoProcessInterface {
public:
    virtual ~MongoProcessCommon() = default;

    std::vector<BSONObj> getCurrentOps(OperationContext* opCtx,
                                       CurrentOpConnectionsMode connMode,
                                       CurrentOpSessionsMode sessionMode,
                                       CurrentOpUserMode userMode,
                                       CurrentOpTruncateMode) const final;

protected:
    /**
     * Returns a BSONObj representing a report of the operation which is currently being
     * executed by the supplied client. This method is called by the getCurrentOps method of
     * MongoProcessCommon to delegate to the mongoS- or mongoD- specific implementation.
     */
    virtual BSONObj _reportCurrentOpForClient(OperationContext* opCtx,
                                              Client* client,
                                              CurrentOpTruncateMode truncateOps) const = 0;

    /**
     * Iterates through all entries in the local SessionCatalog, and adds an entry to the 'ops'
     * vector for each idle session that has stashed its transaction locks while sleeping.
     */
    virtual void _reportCurrentOpsForIdleSessions(OperationContext* opCtx,
                                                  CurrentOpUserMode userMode,
                                                  std::vector<BSONObj>* ops) const = 0;
};

}  // namespace mongo
