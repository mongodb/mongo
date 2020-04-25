/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/db/commands.h"

namespace mongo {

/* for diagnostic / testing purposes. Enabled via command line. */
class CmdWhatsMySNI : public BasicCommand {
public:
    CmdWhatsMySNI() : BasicCommand("whatsmysni") {}

    std::string help() const override {
        return "internal testing command. Returns an object containing the connection's "
               "advertised SNI name, if any, and false if none is being advertised";
    }

    bool run(OperationContext* opCtx,
             const std::string& ns,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto sniName = opCtx->getClient()->getSniNameForSession();
        // if no SNI name is advertised, output is false
        if (!sniName) {
            result.append("sni", false);
        } else {
            result.append("sni", *sniName);
        }

        result.append("ok", 1);

        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {}
};

MONGO_REGISTER_TEST_COMMAND(CmdWhatsMySNI)
}  // namespace mongo
