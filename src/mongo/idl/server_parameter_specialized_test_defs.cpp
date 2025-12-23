/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/idl/server_parameter_specialized_test.h"
#include "mongo/idl/server_parameter_specialized_test_gen.h"

namespace mongo {
namespace test {

namespace {
std::string gSCSP("Initial Value");
std::string gSWO = "Initial Value";
}  // namespace

const std::string& getGlobalSCSP() {
    return gSCSP;
}
const std::string& getGlobalSWO() {
    return gSWO;
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

// specializedDummy

void SpecializedDeprecatedServerParameter::append(OperationContext*,
                                                  BSONObjBuilder* b,
                                                  StringData name,
                                                  const boost::optional<TenantId>&) {
    *b << name << "Dummy Value";
}

Status SpecializedDeprecatedServerParameter::setFromString(StringData value,
                                                           const boost::optional<TenantId>&) {
    return Status::OK();
}

// specializedWithCtor

SpecializedConstructorServerParameter::SpecializedConstructorServerParameter(
    StringData name, ServerParameterType spt)
    : ServerParameter(name, spt) {
    gSCSP = "Value from ctor";
}

void SpecializedConstructorServerParameter::append(OperationContext*,
                                                   BSONObjBuilder* b,
                                                   StringData name,
                                                   const boost::optional<TenantId>&) {
    *b << name << getGlobalSCSP();
}

Status SpecializedConstructorServerParameter::setFromString(StringData value,
                                                            const boost::optional<TenantId>&) {
    gSCSP = std::string{value};
    return Status::OK();
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

// specializedWithStringValue

void SpecializedWithStringValueServerParameter::append(OperationContext*,
                                                       BSONObjBuilder* b,
                                                       StringData name,
                                                       const boost::optional<TenantId>&) {
    *b << name << _data;
}

Status SpecializedWithStringValueServerParameter::setFromString(StringData value,
                                                                const boost::optional<TenantId>&) {
    _data = std::string{value};
    return Status::OK();
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

// specializedWithOptions

Status SpecializedWithOptions::setFromString(StringData value, const boost::optional<TenantId>&) {
    gSWO = std::string{value};
    return Status::OK();
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
    _data = std::string{value};
    return Status::OK();
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

}  // namespace test
}  // namespace mongo
