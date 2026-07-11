// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/change_streams/control_events.h"

#include "mongo/db/namespace_spec_gen.h"

#include <string_view>

#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;
static constexpr std::string_view kClusterTimeField = "clusterTime"sv;
static constexpr std::string_view kIdField = "_id"sv;
static constexpr std::string_view kNamespaceField = "ns"sv;
static constexpr std::string_view kOperationTypeField = "operationType"sv;
static constexpr std::string_view kFullDocumentField = "fullDocument"sv;
static constexpr std::string_view kCommittedAtField = "committedAt"sv;
static constexpr std::string_view kDonorField = "donor"sv;
static constexpr std::string_view kRecipientField = "recipient"sv;
static constexpr std::string_view kAllCollectionChunksMigratedFromDonorField =
    "allCollectionChunksMigratedFromDonor"sv;
static constexpr std::string_view kFromField = "from"sv;
static constexpr std::string_view kToField = "to"sv;
static constexpr std::string_view kOperationDescriptionField = "operationDescription"sv;

Value assertFieldType(const Document& document, std::string_view fieldName, BSONType expectedType) {
    auto val = document[fieldName];
    uassert(ErrorCodes::BadValue,
            str::stream() << "failed to convert change event into control event: expected \""
                          << fieldName << "\" field to have type " << typeName(expectedType)
                          << ", instead found type " << typeName(val.getType()) << ": "
                          << val.toString() << ", full object: " << document.toString(),
            val.getType() == expectedType);
    return val;
}
}  // namespace

MoveChunkControlEvent MoveChunkControlEvent::createFromDocument(const Document& event) {
    auto opDescription =
        assertFieldType(event, kOperationDescriptionField, BSONType::object).getDocument();
    Timestamp clusterTime =
        assertFieldType(event, kClusterTimeField, BSONType::timestamp).getTimestamp();
    ShardId fromShard = assertFieldType(opDescription, kDonorField, BSONType::string).getString();
    ShardId toShard = assertFieldType(opDescription, kRecipientField, BSONType::string).getString();
    bool allCollectionChunksMigratedFromDonor =
        assertFieldType(
            opDescription, kAllCollectionChunksMigratedFromDonorField, BSONType::boolean)
            .getBool();
    return MoveChunkControlEvent{
        clusterTime, fromShard, toShard, allCollectionChunksMigratedFromDonor};
}

MovePrimaryControlEvent MovePrimaryControlEvent::createFromDocument(const Document& event) {
    auto opDescription =
        assertFieldType(event, kOperationDescriptionField, BSONType::object).getDocument();
    Timestamp clusterTime =
        assertFieldType(event, kClusterTimeField, BSONType::timestamp).getTimestamp();
    ShardId fromShard = assertFieldType(opDescription, kFromField, BSONType::string).getString();
    ShardId toShard = assertFieldType(opDescription, kToField, BSONType::string).getString();
    return MovePrimaryControlEvent{clusterTime, fromShard, toShard};
}

NamespacePlacementChangedControlEvent NamespacePlacementChangedControlEvent::createFromDocument(
    const Document& event) {
    Timestamp clusterTime =
        assertFieldType(event, kClusterTimeField, BSONType::timestamp).getTimestamp();
    auto nsField = assertFieldType(event, kNamespaceField, BSONType::object).getDocument();
    auto nssSpec = NamespaceSpec::parse(nsField.toBson(),
                                        IDLParserContext("NamespacePlacementChangedControlEvent"));

    NamespaceString nss = [&]() {
        if (nssSpec.getDb()) {
            if (nssSpec.getColl()) {
                return NamespaceStringUtil::deserialize(*nssSpec.getDb(), *nssSpec.getColl());
            } else {
                return NamespaceString(*nssSpec.getDb());
            }
        }

        tassert(11161100,
                fmt::format("Invalid NamespacePlacementChangedControlEvent. Collection name is "
                            "provided, but no database name is present {}",
                            event.toString()),
                !nssSpec.getColl());
        return NamespaceString::kEmpty;
    }();

    return NamespacePlacementChangedControlEvent{clusterTime, nss};
}

DatabaseCreatedControlEvent DatabaseCreatedControlEvent::createFromDocument(const Document& event) {
    Timestamp clusterTime =
        assertFieldType(event, kClusterTimeField, BSONType::timestamp).getTimestamp();
    auto fullDocumentField =
        assertFieldType(event, kFullDocumentField, BSONType::object).getDocument();
    auto dbNameString = assertFieldType(fullDocumentField, kIdField, BSONType::string).getString();
    DatabaseName createdDatabaseName = DatabaseNameUtil::deserialize(
        boost::none /* tenantId */, dbNameString, SerializationContext::stateCatalog());
    return DatabaseCreatedControlEvent{clusterTime, createdDatabaseName};
}

ControlEvent parseControlEvent(const Document& changeEvent) {
    auto opType = assertFieldType(changeEvent, kOperationTypeField, BSONType::string).getString();
    if (opType == MoveChunkControlEvent::opType) {
        return MoveChunkControlEvent::createFromDocument(changeEvent);
    } else if (opType == MovePrimaryControlEvent::opType) {
        return MovePrimaryControlEvent::createFromDocument(changeEvent);
    } else if (opType == NamespacePlacementChangedControlEvent::opType) {
        return NamespacePlacementChangedControlEvent::createFromDocument(changeEvent);
    } else if (opType == DatabaseCreatedControlEvent::opType) {
        return DatabaseCreatedControlEvent::createFromDocument(changeEvent);
    }

    uasserted(10790500,
              str::stream() << "Change event " << changeEvent.toBson()
                            << " is not a control event");
}
}  // namespace mongo
