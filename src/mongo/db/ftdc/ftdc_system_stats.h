/**
 * Copyright (C) 2016 MongoDB Inc.
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

#include <string>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/ftdc/controller.h"
#include "mongo/db/ftdc/controller.h"

namespace mongo {

class FTDCController;

/**
 * Base class for system metrics collectors. Sets collector name to a common name all system metrics
 * collectors to use.
 */
class SystemMetricsCollector : public FTDCCollectorInterface {
public:
    std::string name() const final;

protected:
    /**
     * Convert any errors we see into BSON for the user to see in the final FTDC document. It is
     * acceptable for the collector to fail, but we do not want to shutdown the FTDC loop because
     * of it. We assume that the BSONBuilder is not corrupt on non-OK Status but nothing else with
     * regards to the final document output.
     */
    static void processStatusErrors(Status s, BSONObjBuilder* builder);
};


/**
 * Install a system metrics collector if it exists as a periodic collector.
 */
void installSystemMetricsCollector(FTDCController* controller);

}  // namespace mongo
