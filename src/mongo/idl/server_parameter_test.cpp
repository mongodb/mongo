/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/idl/server_parameter.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace test {

namespace {
std::string gCustomSetting;
}  // namespace

void customSettingAppendBSON(OperationContext*, BSONObjBuilder* builder, StringData name) {
    builder->append(name, gCustomSetting);
}

Status customSettingFromBSON(const BSONElement& element) {
    gCustomSetting = element.String();
    return Status::OK();
}

Status customSettingFromString(StringData str) {
    gCustomSetting = str.toString();
    return Status::OK();
}
}  // namespace test

namespace {

TEST(ServerParameter, setAppendBSON) {
    IDLServerParameter param("setAppendBSON"_sd, ServerParameterType::kStartupOnly);

    param.setAppendBSON([](OperationContext*, BSONObjBuilder* builder, StringData name) {
        builder->append(name, 42);
    });
    BSONObjBuilder builder;
    param.append(nullptr, builder, param.name());
    auto obj = builder.obj();
    ASSERT_EQ(obj.nFields(), 1);
    ASSERT_EQ(obj[param.name()].Int(), 42);
}

TEST(ServerParameter, setFromString) {
    IDLServerParameter param("setFromString"_sd, ServerParameterType::kStartupOnly);

    param.setFromString([](StringData) { return Status::OK(); });
    ASSERT_OK(param.setFromString("A value"));

    param.setFromString([](StringData) { return Status(ErrorCodes::BadValue, "Can't set me."); });
    ASSERT_NOT_OK(param.setFromString("A value"));
}

TEST(ServerParameter, setFromBSON) {
    IDLServerParameter param("setFromBSON"_sd, ServerParameterType::kStartupOnly);
    BSONElement elem;

    param.setFromBSON([](const BSONElement&) { return Status::OK(); });
    ASSERT_OK(param.set(elem));

    param.setFromBSON(
        [](const BSONElement&) { return Status(ErrorCodes::BadValue, "Can't set me."); });
    ASSERT_NOT_OK(param.set(elem));
}

TEST(ServerParameter, setFromBSONViaString) {
    IDLServerParameter param("setFromBSONViaString"_sd, ServerParameterType::kStartupOnly);
    auto obj = BSON(""
                    << "value");
    auto elem = obj.firstElement();

    param.setFromString([](StringData) { return Status::OK(); });
    ASSERT_OK(param.set(elem));

    param.setFromString([](StringData) { return Status(ErrorCodes::BadValue, "Can't set me."); });
    ASSERT_NOT_OK(param.set(elem));
}

TEST(ServerParameter, deprecatedAlias) {
    IDLServerParameter param("basename"_sd, ServerParameterType::kStartupOnly);
    IDLServerParameterDeprecatedAlias alias("aliasname"_sd, &param);
    std::string value;
    param.setFromString([&value](StringData str) {
        value = str.toString();
        return Status::OK();
    });
    ASSERT_OK(param.setFromString("alpha"));
    ASSERT_EQ("alpha", value);

    ASSERT_OK(alias.setFromString("bravo"));
    ASSERT_EQ("bravo", value);
}

ServerParameter* getServerParameter(const std::string& name) {
    const auto& spMap = ServerParameterSet::getGlobal()->getMap();
    const auto& spIt = spMap.find(name);
    ASSERT(spIt != spMap.end());

    auto* sp = spIt->second;
    ASSERT(sp);
    return sp;
}

TEST(IDLServerParameter, customSettingTest) {
    auto* cst = getServerParameter("customSettingTest");
    ASSERT_OK(cst->setFromString("New Value"));
    ASSERT_EQ(test::gCustomSetting, "New Value");

    auto* cswobson = getServerParameter("customSettingWithoutFromBSON");
    ASSERT_OK(cswobson->set(BSON(""
                                 << "no bson")
                                .firstElement()));
    ASSERT_EQ(test::gCustomSetting, "no bson");

    auto* depr = getServerParameter("customSettingTestDeprecated");
    ASSERT_OK(depr->setFromString("Value via depr name"));
    ASSERT_EQ(test::gCustomSetting, "Value via depr name");
}

TEST(IDLServerParameter, customSettingWithRedaction) {
    auto* csr = getServerParameter("customSettingWithRedaction");
    ASSERT_OK(csr->setFromString("Secret"));
    ASSERT_EQ(test::gCustomSetting, "Secret");

    BSONObjBuilder b;
    csr->append(nullptr, b, csr->name());
    auto obj = b.obj();
    ASSERT_EQ(obj.nFields(), 1);
    ASSERT_EQ(obj[csr->name()].String(), "###");
}

TEST(IDLServerParameter, customTestOnly) {
    auto* cto = getServerParameter("customTestOnlyParameter");
    ASSERT_OK(cto->setFromString("enabled"));
    ASSERT_EQ(test::gCustomSetting, "enabled");

    {
        BSONObjBuilder b;
        cto->append(nullptr, b, cto->name());
        auto obj = b.obj();
        ASSERT_EQ(obj.nFields(), 1);
        ASSERT_EQ(obj[cto->name()].String(), "enabled");
    }

    ServerParameterSet::getGlobal()->disableTestParameters();
    auto* disabled = getServerParameter("customTestOnlyParameter");
    ASSERT_NE(cto, disabled);
    ASSERT_EQ(cto->name(), disabled->name());

    {
        BSONObjBuilder b;
        disabled->append(nullptr, b, disabled->name());
        auto obj = b.obj();
        ASSERT_EQ(obj.nFields(), 0);
    }

    auto status = disabled->setFromString("disabled");
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status.code(), ErrorCodes::BadValue);
    ASSERT_EQ(
        status.reason(),
        "setParameter: 'customTestOnlyParameter' is only supported with 'enableTestCommands=true'");

    ASSERT_EQ(test::gCustomSetting, "enabled");
}

}  // namespace
}  // namespace mongo
