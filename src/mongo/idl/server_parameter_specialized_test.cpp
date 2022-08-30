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

#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/idl/server_parameter_specialized_test_gen.h"
#include "mongo/unittest/assert_that.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace test {

template <typename T = ServerParameter>
T* getServerParameter(StringData name) {
    return ServerParameterSet::getNodeParameterSet()->get<T>(name);
}

template <typename Validator>
void ASSERT_APPENDED_VALUE(ServerParameter* sp, Validator validator) {
    BSONObjBuilder b;
    sp->append(nullptr, &b, sp->name(), boost::none);
    auto obj = b.obj();

    ASSERT_EQ(obj.nFields(), 1);
    auto elem = obj[sp->name()];
    ASSERT_FALSE(elem.eoo());
    validator(elem);
}

void ASSERT_APPENDED_INT(ServerParameter* sp, long exp) {
    ASSERT_APPENDED_VALUE(sp, [&exp](const BSONElement& elem) {
        if (elem.type() == NumberInt) {
            ASSERT_EQ(elem.Int(), exp);
        } else {
            ASSERT_EQ(elem.type(), NumberLong);
            ASSERT_EQ(elem.Long(), exp);
        }
    });
}

void ASSERT_APPENDED_STRING(ServerParameter* sp, StringData exp) {
    ASSERT_APPENDED_VALUE(sp, [&exp](const BSONElement& elem) {
        ASSERT_EQ(elem.type(), String);
        ASSERT_EQ(elem.String(), exp);
    });
}

void ASSERT_APPENDED_OBJECT(ServerParameter* sp, const BSONObj& exp) {
    ASSERT_APPENDED_VALUE(sp, [&exp](const BSONElement& elem) {
        ASSERT_EQ(elem.type(), Object);

        UnorderedFieldsBSONObjComparator comparator;
        ASSERT(comparator.evaluate(elem.Obj() == exp));
    });
}

// specializedDummy

void SpecializedDummyServerParameter::append(OperationContext*,
                                             BSONObjBuilder* b,
                                             StringData name,
                                             const boost::optional<TenantId>&) {
    *b << name << "Dummy Value";
}

Status SpecializedDummyServerParameter::setFromString(StringData value,
                                                      const boost::optional<TenantId>&) {
    return Status::OK();
}

TEST(SpecializedServerParameter, dummy) {
    auto* dsp = getServerParameter("specializedDummy");
    ASSERT_APPENDED_STRING(dsp, "Dummy Value");
    ASSERT_OK(dsp->setFromString("new value", boost::none));
    ASSERT_NOT_OK(dsp->set(BSON("" << BSON_ARRAY("bar")).firstElement(), boost::none));
    ASSERT_OK(dsp->set(BSON(""
                            << "bar")
                           .firstElement(),
                       boost::none));
}

// specializedWithCtor

namespace {
std::string gSCSP("Initial Value");
}  // namespace

SpecializedConstructorServerParameter::SpecializedConstructorServerParameter(
    StringData name, ServerParameterType spt)
    : ServerParameter(name, spt) {
    gSCSP = "Value from ctor";
}

void SpecializedConstructorServerParameter::append(OperationContext*,
                                                   BSONObjBuilder* b,
                                                   StringData name,
                                                   const boost::optional<TenantId>&) {
    *b << name << gSCSP;
}

Status SpecializedConstructorServerParameter::setFromString(StringData value,
                                                            const boost::optional<TenantId>&) {
    gSCSP = value.toString();
    return Status::OK();
}

TEST(SpecializedServerParameter, withCtor) {
    auto* csp = getServerParameter("specializedWithCtor");
    ASSERT_APPENDED_STRING(csp, "Value from ctor");
    ASSERT_OK(csp->setFromString("Updated Value", boost::none));
    ASSERT_EQ(gSCSP, "Updated Value");
    ASSERT_APPENDED_STRING(csp, "Updated Value");
}

// specializedWithValue

void SpecializedWithValueServerParameter::append(OperationContext*,
                                                 BSONObjBuilder* b,
                                                 StringData name,
                                                 const boost::optional<TenantId>&) {
    *b << name << _data;
}

Status SpecializedWithValueServerParameter::setFromString(StringData value,
                                                          const boost::optional<TenantId>&) {
    return NumberParser{}(value, &_data);
}

TEST(SpecializedServerParameter, withValue) {
    using cls = SpecializedWithValueServerParameter;
    ASSERT_EQ(cls::kDataDefault, 43);

    auto* wv = getServerParameter<cls>("specializedWithValue");
    ASSERT_EQ(wv->_data, cls::kDataDefault);
    ASSERT_APPENDED_INT(wv, cls::kDataDefault);
    ASSERT_OK(wv->setFromString("102", boost::none));
    ASSERT_APPENDED_INT(wv, 102);
    ASSERT_EQ(wv->_data, 102);
}

// specializedWithStringValue

void SpecializedWithStringValueServerParameter::append(OperationContext*,
                                                       BSONObjBuilder* b,
                                                       StringData name,
                                                       const boost::optional<TenantId>&) {
    *b << name << _data;
}

Status SpecializedWithStringValueServerParameter::setFromString(StringData value,
                                                                const boost::optional<TenantId>&) {
    _data = value.toString();
    return Status::OK();
}

TEST(SpecializedServerParameter, withStringValue) {
    using cls = SpecializedWithStringValueServerParameter;
    ASSERT_EQ(cls::kDataDefault, "Hello World"_sd);

    auto* wsv = getServerParameter<cls>("specializedWithStringValue");
    ASSERT_EQ(wsv->_data, cls::kDataDefault);
    ASSERT_APPENDED_STRING(wsv, cls::kDataDefault);
    ASSERT_OK(wsv->setFromString("Goodbye Land", boost::none));
    ASSERT_APPENDED_STRING(wsv, "Goodbye Land");
    ASSERT_EQ(wsv->_data, "Goodbye Land");
}

// specializedWithAtomicValue

void SpecializedWithAtomicValueServerParameter::append(OperationContext*,
                                                       BSONObjBuilder* b,
                                                       StringData name,
                                                       const boost::optional<TenantId>&) {
    *b << name << _data.load();
}

Status SpecializedWithAtomicValueServerParameter::setFromString(StringData value,
                                                                const boost::optional<TenantId>&) {
    std::uint32_t val;

    auto status = NumberParser{}(value, &val);
    if (!status.isOK()) {
        return status;
    }

    _data.store(val);
    return Status::OK();
}

TEST(SpecializedServerParameter, withAtomicValue) {
    using cls = SpecializedWithAtomicValueServerParameter;
    ASSERT_EQ(cls::kDataDefault, 42);

    auto* wv = getServerParameter<cls>("specializedWithAtomicValue");
    ASSERT_EQ(wv->_data.load(), cls::kDataDefault);
    ASSERT_APPENDED_INT(wv, cls::kDataDefault);
    ASSERT_OK(wv->set(BSON("" << 99).firstElement(), boost::none));
    ASSERT_APPENDED_INT(wv, 99);
    ASSERT_OK(wv->setFromString("101", boost::none));
    ASSERT_APPENDED_INT(wv, 101);
    ASSERT_EQ(wv->_data.load(), 101);
}

// specializedWithMultiValue

void SpecializedMultiValueServerParameter::append(OperationContext*,
                                                  BSONObjBuilder* b,
                                                  StringData name,
                                                  const boost::optional<TenantId>&) {
    *b << name << BSON("value" << _data.value << "flag" << _data.flag);
}

Status SpecializedMultiValueServerParameter::set(const BSONElement& value,
                                                 const boost::optional<TenantId>&) try {
    auto obj = value.Obj();
    _data.value = obj["value"].String();
    _data.flag = obj["flag"].Bool();
    return Status::OK();
} catch (const AssertionException&) {
    return {ErrorCodes::BadValue, "Failed parsing extra data"};
}

Status SpecializedMultiValueServerParameter::setFromString(StringData value,
                                                           const boost::optional<TenantId>&) {
    return set(BSON("" << BSON("value" << value << "flag" << false)).firstElement(), boost::none);
}

TEST(SpecializedServerParameter, multiValue) {
    auto* edsp = getServerParameter("specializedWithMultiValue");
    ASSERT_APPENDED_OBJECT(edsp,
                           BSON("value"
                                << "start value"
                                << "flag" << true));
    ASSERT_OK(edsp->setFromString("second value", boost::none));
    ASSERT_APPENDED_OBJECT(edsp,
                           BSON("value"
                                << "second value"
                                << "flag" << false));
    ASSERT_OK(edsp->set(BSON("" << BSON("value"
                                        << "third value"
                                        << "flag" << true))
                            .firstElement(),
                        boost::none));
    ASSERT_APPENDED_OBJECT(edsp,
                           BSON("value"
                                << "third value"
                                << "flag" << true));
}

// specializedWithCtorAndValue

SpecializedWithCtorAndValueServerParameter::SpecializedWithCtorAndValueServerParameter(
    StringData name, ServerParameterType spt)
    : ServerParameter(name, spt) {}

void SpecializedWithCtorAndValueServerParameter::append(OperationContext*,
                                                        BSONObjBuilder* b,
                                                        StringData name,
                                                        const boost::optional<TenantId>&) {
    *b << name << _data;
}

Status SpecializedWithCtorAndValueServerParameter::setFromString(StringData value,
                                                                 const boost::optional<TenantId>&) {
    return NumberParser{}(value, &_data);
}

TEST(SpecializedServerParameter, withCtorAndValue) {
    using cls = SpecializedWithCtorAndValueServerParameter;
    auto* cvsp = getServerParameter<cls>("specializedWithCtorAndValue");
    ASSERT_APPENDED_INT(cvsp, cls::kDataDefault);
    ASSERT_OK(cvsp->setFromString(std::to_string(cls::kDataDefault + 1), boost::none));
    ASSERT_EQ(cvsp->_data, cls::kDataDefault + 1);
    ASSERT_APPENDED_INT(cvsp, cls::kDataDefault + 1);
}

// specializedWithOptions

namespace {
std::string gSWO = "Initial Value";
}  // namespace

Status SpecializedWithOptions::setFromString(StringData value, const boost::optional<TenantId>&) {
    gSWO = value.toString();
    return Status::OK();
}

TEST(SpecializedServerParameter, withOptions) {
    auto* swo = getServerParameter("specializedWithOptions");
    ASSERT_APPENDED_STRING(swo, "###");
    ASSERT_OK(swo->setFromString("second value", boost::none));
    ASSERT_EQ(gSWO, "second value");
    ASSERT_APPENDED_STRING(swo, "###");

    auto* dswo = getServerParameter("deprecatedWithOptions");
    ASSERT_APPENDED_STRING(dswo, "###");
    ASSERT_OK(dswo->setFromString("third value", boost::none));
    ASSERT_EQ(gSWO, "third value");
    ASSERT_APPENDED_STRING(dswo, "###");
}

// specializedRuntimeOnly

void SpecializedRuntimeOnly::append(OperationContext*,
                                    BSONObjBuilder*,
                                    StringData,
                                    const boost::optional<TenantId>&) {}

Status SpecializedRuntimeOnly::setFromString(StringData value, const boost::optional<TenantId>&) {
    return Status::OK();
}

Status SpecializedRedactedSettable::setFromString(StringData value,
                                                  const boost::optional<TenantId>&) {
    std::cout << "Setting to: " << value << "\n";
    _data = value.toString();
    return Status::OK();
}

TEST(SpecializedServerParameter, SpecializedRedactedSettable) {
    using namespace std::literals;
    using namespace unittest::match;

    auto* sp = getServerParameter("specializedRedactedSettable");
    ASSERT(sp);
    auto down = dynamic_cast<SpecializedRedactedSettable*>(sp);
    ASSERT(down);
    auto& dataMember = down->_data;

    auto store = [&](auto&& name, auto&& value) {
        return sp->set(BSON(name << value).firstElement(), boost::none);
    };
    auto load = [&] {
        BSONObjBuilder bob;
        sp->append(nullptr, &bob, sp->name(), boost::none);
        return bob.obj();
    };

    ASSERT_OK(store("", "hello"));
    ASSERT_THAT(load(), BSONObjHas(BSONElementIs(Eq(sp->name()), Eq(String), Eq("###"))))
        << "value redacted by append";
    ASSERT_THAT(dataMember, Eq("hello")) << "value preseved in _data member";

    ASSERT_THAT(store("", std::vector{"zzzzz"s}),
                StatusIs(Eq(ErrorCodes::BadValue),
                         AllOf(ContainsRegex("[uU]nsupported type"),
                               ContainsRegex("###"),
                               Not(ContainsRegex("zzzzz")))))
        << "value redacted in `set` Status when failing from unsupported element type";
    ASSERT_THAT(dataMember, Eq("hello")) << "Unchanged by failed `set` call";
}

TEST(SpecializedServerParameter, withScope) {
    using SPT = ServerParameterType;

    auto* nodeSet = ServerParameterSet::getNodeParameterSet();
    auto* clusterSet = ServerParameterSet::getClusterParameterSet();

    static constexpr auto kSpecializedWithOptions = "specializedWithOptions"_sd;
    auto* nodeSWO = nodeSet->getIfExists(kSpecializedWithOptions);
    ASSERT(nullptr != nodeSWO);
    ASSERT(nullptr == clusterSet->getIfExists(kSpecializedWithOptions));

    auto* clusterSWO =
        makeServerParameter<SpecializedWithOptions>(kSpecializedWithOptions, SPT::kClusterWide);
    ASSERT(clusterSWO != nodeSWO);
    ASSERT(clusterSWO == clusterSet->getIfExists(kSpecializedWithOptions));

    // Duplicate key
    ASSERT_THROWS_CODE(
        makeServerParameter<SpecializedWithOptions>(kSpecializedWithOptions, SPT::kClusterWide),
        DBException,
        23784);

    // Require runtime only.
    static constexpr auto kSpecializedRuntimeOnly = "specializedRuntimeOnly"_sd;
    auto* clusterSRO =
        makeServerParameter<SpecializedRuntimeOnly>(kSpecializedRuntimeOnly, SPT::kClusterWide);
    ASSERT(nullptr != clusterSRO);
    // Pointer now belongs to ServerParameterSet, no need to delete.
}

// specializedWithValidateServerParameter

void SpecializedWithValidateServerParameter::append(OperationContext*,
                                                    BSONObjBuilder*,
                                                    StringData,
                                                    const boost::optional<TenantId>&) {}

Status SpecializedWithValidateServerParameter::setFromString(StringData str,
                                                             const boost::optional<TenantId>&) {
    return NumberParser{}(str, &_data);
}

Status SpecializedWithValidateServerParameter::validate(
    const BSONElement& newValueElement, const boost::optional<TenantId>& tenantId) const {
    try {
        auto val = newValueElement.Int();
        if (val < 0) {
            return Status{ErrorCodes::BadValue,
                          "specializedWithValidate must be a non-negative integer"};
        }
    } catch (const AssertionException&) {
        return {ErrorCodes::BadValue, "Failed parsing specializedWithValidate"};
    }

    return Status::OK();
}

TEST(SpecializedServerParameter, withValidate) {
    auto* nodeSet = ServerParameterSet::getNodeParameterSet();

    constexpr auto kSpecializedWithValidate = "specializedWithValidate"_sd;
    auto* validateSP = nodeSet->getIfExists(kSpecializedWithValidate);
    ASSERT(nullptr != validateSP);

    // Assert that validate works by itself.
    ASSERT_OK(
        validateSP->validate(BSON(kSpecializedWithValidate << 5).firstElement(), boost::none));
    ASSERT_OK(
        validateSP->validate(BSON(kSpecializedWithValidate << 0).firstElement(), boost::none));
    ASSERT_NOT_OK(
        validateSP->validate(BSON(kSpecializedWithValidate << -1).firstElement(), boost::none));

    // Assert that validate works when called within set.
    ASSERT_OK(validateSP->set(BSON(kSpecializedWithValidate << 5).firstElement(), boost::none));
    ASSERT_OK(validateSP->set(BSON(kSpecializedWithValidate << 0).firstElement(), boost::none));
    ASSERT_NOT_OK(
        validateSP->set(BSON(kSpecializedWithValidate << -1).firstElement(), boost::none));
}

// specializedWithClusterServerParameter

void SpecializedClusterServerParameter::append(OperationContext*,
                                               BSONObjBuilder* builder,
                                               StringData name,
                                               const boost::optional<TenantId>& tenantId) {
    builder->append("_id"_sd, name);
    builder->appendElementsUnique(_data.toBSON());
}

Status SpecializedClusterServerParameter::set(const BSONElement& newValueElement,
                                              const boost::optional<TenantId>& tenantId) {
    Status status = validate(newValueElement, tenantId);
    if (!status.isOK()) {
        return status;
    }

    _data.parse(newValueElement.Obj());
    return Status::OK();
}

Status SpecializedClusterServerParameter::validate(
    const BSONElement& newValueElement, const boost::optional<TenantId>& tenantId) const {
    try {
        auto obj = newValueElement.Obj();
        auto strValue = obj["strData"_sd].String();
        auto intValue = obj["intData"_sd].Int();

        if (strValue.size() == 0 || intValue < 0) {
            return Status{ErrorCodes::BadValue,
                          "Invalid fields provided to specializedCluster parameter"};
        }
    } catch (const AssertionException&) {
        return {ErrorCodes::BadValue, "Failed parsing specializedCluster parameter"};
    }

    return Status::OK();
}

Status SpecializedClusterServerParameter::reset(const boost::optional<TenantId>& tenantId) {
    _data.reset();
    return Status::OK();
}

LogicalTime SpecializedClusterServerParameter::getClusterParameterTime(
    const boost::optional<TenantId>& tenantId) const {
    return _data.getClusterParameterTime();
}

TEST(SpecializedServerParameter, clusterServerParameter) {
    auto* clusterSet = ServerParameterSet::getClusterParameterSet();
    constexpr auto kSpecializedCSPName = "specializedCluster"_sd;

    auto* specializedCsp = clusterSet->getIfExists(kSpecializedCSPName);
    ASSERT(nullptr != specializedCsp);

    // Assert that the parameter can be set.
    BSONObjBuilder builder;
    SpecializedClusterServerParameterData data;
    LogicalTime updateTime = LogicalTime(Timestamp(Date_t::now()));
    data.setClusterParameterTime(updateTime);
    data.setIntData(50);
    data.setStrData("hello");
    data.setId(kSpecializedCSPName);
    data.serialize(&builder);
    ASSERT_OK(specializedCsp->set(builder.asTempObj(), boost::none));

    // Assert that the parameter cannot be set from strings.
    ASSERT_NOT_OK(specializedCsp->setFromString("", boost::none));

    // Assert that the clusterParameterTime can be retrieved.
    ASSERT_EQ(specializedCsp->getClusterParameterTime(boost::none), updateTime);

    // Assert that the parameter can be appended to a builder.
    builder.resetToEmpty();
    specializedCsp->append(nullptr, &builder, kSpecializedCSPName.toString(), boost::none);
    auto obj = builder.asTempObj();
    ASSERT_EQ(obj.nFields(), 4);
    ASSERT_EQ(obj["_id"_sd].String(), kSpecializedCSPName);
    ASSERT_EQ(obj["clusterParameterTime"_sd].timestamp(), updateTime.asTimestamp());
    ASSERT_EQ(obj["strData"_sd].String(), "hello");
    ASSERT_EQ(obj["intData"_sd].Int(), 50);

    // Assert that invalid parameter values fail validation directly and implicitly during set.
    builder.resetToEmpty();
    updateTime = LogicalTime(Timestamp(Date_t::now()));
    data.setClusterParameterTime(updateTime);
    data.setIntData(-1);
    data.setStrData("");
    data.serialize(&builder);
    ASSERT_NOT_OK(specializedCsp->validate(builder.asTempObj(), boost::none));
    ASSERT_NOT_OK(specializedCsp->set(builder.asTempObj(), boost::none));

    // Assert that the parameter can be reset to its defaults.
    builder.resetToEmpty();
    ASSERT_OK(specializedCsp->reset(boost::none));
    specializedCsp->append(nullptr, &builder, kSpecializedCSPName.toString(), boost::none);
    obj = builder.asTempObj();
    ASSERT_EQ(obj.nFields(), 4);
    ASSERT_EQ(obj["_id"_sd].String(), kSpecializedCSPName);
    ASSERT_EQ(obj["clusterParameterTime"_sd].timestamp(), LogicalTime().asTimestamp());
    ASSERT_EQ(obj["strData"_sd].String(), "default");
    ASSERT_EQ(obj["intData"_sd].Int(), 30);
}

}  // namespace test
}  // namespace mongo
