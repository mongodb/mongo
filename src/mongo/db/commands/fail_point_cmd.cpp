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
 */

#include <vector>

#include "mongo/base/init.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/util/fail_point_service.h"

namespace mongo {
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
     *        3. { period: <n> } - n should be within the range of a 32 bit signed
     *            integer and this would be the approximate period for every activation.
     *            For example, for { period: 120 }, the probability of the fail point to
     *            be activated is 1 in 120. NOT YET SUPPORTED.
     *        4. { times: <n> } - n should be positive and within the range of a 32 bit
     *            signed integer and this is the number of passes on the fail point will
     *            remain activated.
     *
     *    data: <Object> // optional arbitrary object to store.
     * }
     */
    class FaultInjectCmd: public Command {
    public:
        FaultInjectCmd(): Command("configureFailPoint") {}

        virtual bool slaveOk() const {
            return true;
        }

        virtual LockType locktype() const {
            return NONE;
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

        bool run(const string& dbname,
                BSONObj& cmdObj,
                int,
                string& errmsg,
                BSONObjBuilder& result,
                bool fromRepl) {
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
            }
            else if (modeElem.type() == String) {
                const string modeStr(modeElem.valuestr());

                if (modeStr == "off") {
                    mode = FailPoint::off;
                }
                else if (modeStr == "alwaysOn") {
                    mode = FailPoint::alwaysOn;
                }
                else {
                    errmsg = "unknown mode: " + modeStr;
                    return false;
                }
            }
            else if (modeElem.type() == Object) {
                const BSONObj modeObj(modeElem.Obj());

                if (modeObj.hasField("times")) {
                    mode = FailPoint::nTimes;
                    const int intVal = modeObj["times"].numberInt();

                    if (intVal < 0) {
                        errmsg = "times should be positive";
                        return false;
                    }

                    val = intVal;
                }
                else if (modeObj.hasField("period")) {
                    mode = FailPoint::random;

                    // TODO: implement
                    errmsg = "random is not yet supported";
                    return false;
                }
                else {
                    errmsg = "invalid mode object";
                    return false;
                }
            }
            else {
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
