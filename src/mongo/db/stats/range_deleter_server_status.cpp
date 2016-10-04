/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/range_deleter_service.h"

namespace mongo {

/**
 * Server status section for RangeDeleter.
 *
 * Sample format:
 *
 * rangeDeleter: {
 *   lastDeleteStats: [
 *     {
 *       deleteDocs: NumberLong(5);
 *       queueStart: ISODate("2014-06-11T22:45:30.221Z"),
 *       queueEnd: ISODate("2014-06-11T22:45:30.221Z"),
 *       deleteStart: ISODate("2014-06-11T22:45:30.221Z"),
 *       deleteEnd: ISODate("2014-06-11T22:45:30.221Z"),
 *       waitForReplStart: ISODate("2014-06-11T22:45:30.221Z"),
 *       waitForReplEnd: ISODate("2014-06-11T22:45:30.221Z")
 *     }
 *   ]
 * }
 */
class RangeDeleterServerStatusSection : public ServerStatusSection {
public:
    RangeDeleterServerStatusSection() : ServerStatusSection("rangeDeleter") {}
    bool includeByDefault() const {
        return false;
    }

    BSONObj generateSection(OperationContext* txn, const BSONElement& configElement) const {
        RangeDeleter* deleter = getDeleter();
        if (!deleter) {
            return BSONObj();
        }

        BSONObjBuilder result;

        OwnedPointerVector<DeleteJobStats> statsList;
        deleter->getStatsHistory(&statsList.mutableVector());
        BSONArrayBuilder oldStatsBuilder;
        for (OwnedPointerVector<DeleteJobStats>::const_iterator it = statsList.begin();
             it != statsList.end();
             ++it) {
            BSONObjBuilder entryBuilder;
            entryBuilder.append("deletedDocs", (*it)->deletedDocCount);

            if ((*it)->queueEndTS > Date_t()) {
                entryBuilder.append("queueStart", (*it)->queueStartTS);
                entryBuilder.append("queueEnd", (*it)->queueEndTS);
            }

            if ((*it)->deleteEndTS > Date_t()) {
                entryBuilder.append("deleteStart", (*it)->deleteStartTS);
                entryBuilder.append("deleteEnd", (*it)->deleteEndTS);

                if ((*it)->waitForReplEndTS > Date_t()) {
                    entryBuilder.append("waitForReplStart", (*it)->waitForReplStartTS);
                    entryBuilder.append("waitForReplEnd", (*it)->waitForReplEndTS);
                }
            }

            oldStatsBuilder.append(entryBuilder.obj());
        }
        result.append("lastDeleteStats", oldStatsBuilder.arr());

        return result.obj();
    }

} rangeDeleterServerStatusSection;
}
