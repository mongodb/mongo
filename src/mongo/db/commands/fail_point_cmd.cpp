/*
 *    Copyright (C) 2012 10gen Inc.
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

#include <vector>

#include "mongo/base/init.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/util/fail_point_service.h"

namespace mongo {

using std::string;
using std::stringstream;

/**
 * Command for modifying installed fail points.
 *
 * Format
 * {
 *    configureFailPoint: <string>, // name of the fail point.
 *    mode: <string|Object>, // the new mode to set. Can have one of the
 *        following format:
 *
 *        1. 'off' - disable fail point.
 *        2. 'alwaysOn' - fail point is always active.
 *        3. { activationProbability: <n> } - n should be a double between 0 and 1,
 *           representing the probability that the fail point will fire.  0 means never,
 *           1 means (nearly) always.
 *        4. { times: <n> } - n should be positive and within the range of a 32 bit
 *            signed integer and this is the number of passes on the fail point will
 *            remain activated.
 *
 *    data: <Object> // optional arbitrary object to store.
 * }
 */
class FaultInjectCmd : public Command {
public:
    FaultInjectCmd() : Command("configureFailPoint") {}

    virtual bool slaveOk() const {
        return true;
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual bool adminOnly() const {
        return true;
    }

    // No auth needed because it only works when enabled via command line.
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {}

    virtual void help(stringstream& h) const {
        h << "modifies the settings of a fail point";
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        const string failPointName(cmdObj.firstElement().str());
        FailPointRegistry* registry = getGlobalFailPointRegistry();
        FailPoint* failPoint = registry->getFailPoint(failPointName);

        if (failPoint == NULL) {
            errmsg = failPointName + " not found";
            return false;
        }

        FailPoint::Mode mode = FailPoint::alwaysOn;
        FailPoint::ValType val = 0;

        const BSONElement modeElem(cmdObj["mode"]);
        if (modeElem.eoo()) {
            result.appendElements(failPoint->toBSON());
            return true;
        } else if (modeElem.type() == String) {
            const string modeStr(modeElem.valuestr());

            if (modeStr == "off") {
                mode = FailPoint::off;
            } else if (modeStr == "alwaysOn") {
                mode = FailPoint::alwaysOn;
            } else {
                errmsg = "unknown mode: " + modeStr;
                return false;
            }
        } else if (modeElem.type() == Object) {
            const BSONObj modeObj(modeElem.Obj());

            if (modeObj.hasField("times")) {
                mode = FailPoint::nTimes;
                const int intVal = modeObj["times"].numberInt();

                if (intVal < 0) {
                    errmsg = "times should be positive";
                    return false;
                }

                val = intVal;
            } else if (modeObj.hasField("activationProbability")) {
                mode = FailPoint::random;
                const double activationProbability =
                    modeObj["activationProbability"].numberDouble();
                if (activationProbability < 0 || activationProbability > 1) {
                    errmsg = str::stream()
                        << "activationProbability must be between 0.0 and 1.0; found "
                        << activationProbability;
                    return false;
                }
                val = static_cast<int32_t>(std::numeric_limits<int32_t>::max() *
                                           activationProbability);
            } else {
                errmsg = "invalid mode object";
                return false;
            }
        } else {
            errmsg = "invalid mode format";
            return false;
        }

        BSONObj dataObj;
        if (cmdObj.hasField("data")) {
            dataObj = cmdObj["data"].Obj();
        }

        failPoint->setMode(mode, val, dataObj);
        return true;
    }
};
MONGO_INITIALIZER(RegisterFaultInjectCmd)(InitializerContext* context) {
    if (Command::testCommandsEnabled) {
        // Leaked intentionally: a Command registers itself when constructed.
        new FaultInjectCmd();
    }
    return Status::OK();
}
}
