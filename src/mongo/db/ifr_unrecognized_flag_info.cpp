// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/ifr_unrecognized_flag_info.h"

#include "mongo/base/init.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/idl/idl_parser.h"

#include <string_view>

namespace mongo {
namespace {

constexpr std::string_view kFlagsFieldName = "flags";
constexpr std::string_view kSenderVersionFieldName = "senderVersion";
MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(UnrecognizedIFRFlagInfo);

}  // namespace

void UnrecognizedIFRFlagInfo::serialize(BSONObjBuilder* bob) const {
    {
        // Flags are serialized as a single sub-object keyed by flag name, e.g. {flagA: true}.
        BSONObjBuilder flagsObj(bob->subobjStart(kFlagsFieldName));
        for (const auto& [name, value] : _flags) {
            flagsObj.append(name, value);
        }
    }
    {
        // Sender version is serialized as its IDL sub-object (major/minor/patch/extra).
        BSONObjBuilder versionObj(bob->subobjStart(kSenderVersionFieldName));
        _senderVersion.serialize(&versionObj);
    }
}

std::shared_ptr<const ErrorExtraInfo> UnrecognizedIFRFlagInfo::parse(const BSONObj& obj) {
    const auto flagsEl = obj.getField(kFlagsFieldName);
    uassert(13024000,
            fmt::format("UnrecognizedIFRFlagInfo was missing '{}' field", kFlagsFieldName),
            !flagsEl.eoo());
    uassert(13024004,
            fmt::format("UnrecognizedIFRFlagInfo '{}' field must be an object", kFlagsFieldName),
            flagsEl.type() == BSONType::object);

    const auto senderVersionEl = obj.getField(kSenderVersionFieldName);
    uassert(13024001,
            fmt::format("UnrecognizedIFRFlagInfo was missing '{}' field", kSenderVersionFieldName),
            !senderVersionEl.eoo());
    uassert(13024002,
            fmt::format("UnrecognizedIFRFlagInfo '{}' field must be an object",
                        kSenderVersionFieldName),
            senderVersionEl.type() == BSONType::object);

    UnrecognizedIFRFlagInfo::FlagMap flags;
    for (const auto& flagEl : flagsEl.Obj()) {
        uassert(13024003,
                fmt::format("UnrecognizedIFRFlagInfo flag '{}' must have a boolean value",
                            flagEl.fieldNameStringData()),
                flagEl.type() == BSONType::boolean);
        flags[std::string(flagEl.fieldNameStringData())] = flagEl.Bool();
    }

    auto senderVersion = IFRSenderVersion::parse(
        senderVersionEl.Obj(), IDLParserContext("UnrecognizedIFRFlagInfo.senderVersion"));

    return std::make_shared<UnrecognizedIFRFlagInfo>(std::move(flags), std::move(senderVersion));
}

Status makeUnrecognizedIFRFlagStatus(UnrecognizedIFRFlagInfo::FlagMap unknownFlags,
                                     IFRSenderVersion senderVersion) {
    const size_t n = unknownFlags.size();
    std::string reason = n == 1
        ? fmt::format("Received unknown IFR flag '{}' from sender at version {}",
                      unknownFlags.begin()->first,
                      formatSenderVersion(senderVersion))
        : fmt::format("Received {} unknown IFR flags from sender at version {}",
                      n,
                      formatSenderVersion(senderVersion));
    return Status(UnrecognizedIFRFlagInfo(std::move(unknownFlags), std::move(senderVersion)),
                  std::move(reason));
}

}  // namespace mongo
