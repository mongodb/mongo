/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/repl/repl_set_config.h"


namespace mongo {
namespace repl {

static ReplSetConfig makeSplitConfig(const ReplSetConfig& config,
                                     const std::string& recipientSetName,
                                     const std::string& recipientTagName) {
    dassert(!recipientSetName.empty() && recipientSetName != config.getReplSetName());
    uassert(6201800,
            "We can not make a split config on an existing split config.",
            !config.isSplitConfig());

    const auto& tagConfig = config.getTagConfig();
    std::vector<BSONObj> recipientMembers, donorMembers;
    int donorIndex = 0, recipientIndex = 0;
    for (const auto& member : config.members()) {
        bool isRecipient =
            std::any_of(member.tagsBegin(), member.tagsEnd(), [&](const ReplSetTag& tag) {
                return tagConfig.getTagKey(tag) == recipientTagName;
            });
        if (isRecipient) {
            BSONObjBuilder bob(
                member.toBSON().removeField("votes").removeField("priority").removeField("_id"));
            bob.appendNumber("_id", recipientIndex);
            recipientMembers.push_back(bob.obj());
            recipientIndex++;
        } else {
            BSONObjBuilder bob(member.toBSON().removeField("_id"));
            bob.appendNumber("_id", donorIndex);
            donorMembers.push_back(bob.obj());
            donorIndex++;
        }
    }

    uassert(6201801, "No recipient members found for split config.", !recipientMembers.empty());
    uassert(6201802, "No donor members found for split config.", !donorMembers.empty());

    const auto configNoMembersBson = config.toBSON().removeField("members");

    BSONObjBuilder recipientConfigBob(
        configNoMembersBson.removeField("_id").removeField("settings"));
    recipientConfigBob.append("_id", recipientSetName).append("members", recipientMembers);
    if (configNoMembersBson.hasField("settings") &&
        configNoMembersBson.getField("settings").isABSONObj()) {
        BSONObj settings = configNoMembersBson.getField("settings").Obj();
        if (settings.hasField("replicaSetId")) {
            recipientConfigBob.append("settings", settings.removeField("replicaSetId"));
        }
    }

    BSONObjBuilder splitConfigBob(configNoMembersBson);
    splitConfigBob.append("members", donorMembers);
    splitConfigBob.append("recipientConfig", recipientConfigBob.obj());

    return ReplSetConfig::parse(splitConfigBob.obj());
}
}  // namespace repl
}  // namespace mongo
