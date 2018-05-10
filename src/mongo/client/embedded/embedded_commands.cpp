/**
*    Copyright (C) 2018 MongoDB Inc.
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
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#include "mongo/platform/basic.h"

#include "mongo/db/commands.h"

namespace mongo {

class CmdTrimMemory : public BasicCommand {
public:
    CmdTrimMemory() : BasicCommand("trimMemory") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return false;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
    }  // No auth in embedded

    std::string help() const override {
        return "Gives the database an opportunity to trim memory usage. Valid parameters are "
               "conservative|moderate|aggressive to describe trim method.";
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& jsobj,
             BSONObjBuilder& result) override {

        // TODO: Implement trimMemory https://jira.mongodb.org/browse/SERVER-34131

        std::string mode = jsobj[getName()].String();
        if (mode == "aggressive") {

        } else if (mode == "moderate") {

        } else if (mode == "conservative") {

        } else {
            uasserted(ErrorCodes::InvalidOptions,
                      "Only conservative|moderate|aggressive are valid options.");
        }
        return true;
    }

} cmdTrimMemory;

class CmdBatteryLevel : public BasicCommand {
public:
    CmdBatteryLevel() : BasicCommand("setBatteryLevel") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return false;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
    }  // No auth in embedded

    std::string help() const override {
        return "Notifies the database of the battery level on host device. Valid parameters are "
               "low|normal to describe the battery level.";
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& jsobj,
             BSONObjBuilder& result) override {

        // TODO: Implement setBatteryLevel https://jira.mongodb.org/browse/SERVER-34132

        std::string mode = jsobj[getName()].String();
        if (mode == "low") {

        } else if (mode == "normal") {

        } else {
            uasserted(ErrorCodes::InvalidOptions, "Only low|normal are valid options.");
        }
        return true;
    }

} cmdBatteryLevel;

}  // namespace mongo
