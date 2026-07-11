// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/idl/server_parameter_specialized_test.h"
#include "mongo/idl/server_parameter_specialized_test_gen.h"

#include <string_view>

namespace mongo {
namespace test {
using namespace std::literals::string_view_literals;

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
                                             std::string_view name,
                                             const boost::optional<TenantId>&) {
    *b << name << "Dummy Value";
}

Status SpecializedDummyServerParameter::setFromString(std::string_view value,
                                                      const boost::optional<TenantId>&) {
    return Status::OK();
}

// specializedDummy

void SpecializedDeprecatedServerParameter::append(OperationContext*,
                                                  BSONObjBuilder* b,
                                                  std::string_view name,
                                                  const boost::optional<TenantId>&) {
    *b << name << "Dummy Value";
}

Status SpecializedDeprecatedServerParameter::setFromString(std::string_view value,
                                                           const boost::optional<TenantId>&) {
    return Status::OK();
}

// specializedWithCtor

SpecializedConstructorServerParameter::SpecializedConstructorServerParameter(
    std::string_view name, ServerParameterType spt)
    : ServerParameter(name, spt) {
    gSCSP = "Value from ctor";
}

void SpecializedConstructorServerParameter::append(OperationContext*,
                                                   BSONObjBuilder* b,
                                                   std::string_view name,
                                                   const boost::optional<TenantId>&) {
    *b << name << getGlobalSCSP();
}

Status SpecializedConstructorServerParameter::setFromString(std::string_view value,
                                                            const boost::optional<TenantId>&) {
    gSCSP = std::string{value};
    return Status::OK();
}

// specializedWithValue

void SpecializedWithValueServerParameter::append(OperationContext*,
                                                 BSONObjBuilder* b,
                                                 std::string_view name,
                                                 const boost::optional<TenantId>&) {
    *b << name << _data;
}

Status SpecializedWithValueServerParameter::setFromString(std::string_view value,
                                                          const boost::optional<TenantId>&) {
    return NumberParser{}(value, &_data);
}

// specializedWithStringValue

void SpecializedWithStringValueServerParameter::append(OperationContext*,
                                                       BSONObjBuilder* b,
                                                       std::string_view name,
                                                       const boost::optional<TenantId>&) {
    *b << name << _data;
}

Status SpecializedWithStringValueServerParameter::setFromString(std::string_view value,
                                                                const boost::optional<TenantId>&) {
    _data = std::string{value};
    return Status::OK();
}

// specializedWithAtomicValue

void SpecializedWithAtomicValueServerParameter::append(OperationContext*,
                                                       BSONObjBuilder* b,
                                                       std::string_view name,
                                                       const boost::optional<TenantId>&) {
    *b << name << _data.load();
}

Status SpecializedWithAtomicValueServerParameter::setFromString(std::string_view value,
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
                                                  std::string_view name,
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

Status SpecializedMultiValueServerParameter::setFromString(std::string_view value,
                                                           const boost::optional<TenantId>&) {
    return set(BSON("" << BSON("value" << value << "flag" << false)).firstElement(), boost::none);
}

// specializedWithCtorAndValue

SpecializedWithCtorAndValueServerParameter::SpecializedWithCtorAndValueServerParameter(
    std::string_view name, ServerParameterType spt)
    : ServerParameter(name, spt) {}

void SpecializedWithCtorAndValueServerParameter::append(OperationContext*,
                                                        BSONObjBuilder* b,
                                                        std::string_view name,
                                                        const boost::optional<TenantId>&) {
    *b << name << _data;
}

Status SpecializedWithCtorAndValueServerParameter::setFromString(std::string_view value,
                                                                 const boost::optional<TenantId>&) {
    return NumberParser{}(value, &_data);
}

// specializedWithOptions

Status SpecializedWithOptions::setFromString(std::string_view value,
                                             const boost::optional<TenantId>&) {
    gSWO = std::string{value};
    return Status::OK();
}

// specializedRuntimeOnly

void SpecializedRuntimeOnly::append(OperationContext*,
                                    BSONObjBuilder*,
                                    std::string_view,
                                    const boost::optional<TenantId>&) {}

Status SpecializedRuntimeOnly::setFromString(std::string_view value,
                                             const boost::optional<TenantId>&) {
    return Status::OK();
}

Status SpecializedRedactedSettable::setFromString(std::string_view value,
                                                  const boost::optional<TenantId>&) {
    std::cout << "Setting to: " << value << "\n";
    _data = std::string{value};
    return Status::OK();
}

// specializedWithValidateServerParameter

void SpecializedWithValidateServerParameter::append(OperationContext*,
                                                    BSONObjBuilder*,
                                                    std::string_view,
                                                    const boost::optional<TenantId>&) {}

Status SpecializedWithValidateServerParameter::setFromString(std::string_view str,
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
                                               std::string_view name,
                                               const boost::optional<TenantId>& tenantId) {
    builder->append("_id"sv, name);
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
        auto strValue = obj["strData"sv].String();
        auto intValue = obj["intData"sv].Int();

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
