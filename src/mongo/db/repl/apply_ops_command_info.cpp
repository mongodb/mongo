// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/repl/apply_ops_command_info.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/auth/validated_tenancy_scope_factory.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace repl {

namespace apply_ops_command_info_details {
using namespace std::literals::string_view_literals;

/**
 * Return true iff the applyOpsCmd can be executed in a single WriteUnitOfWork.
 */
bool _parseAreOpsCrudOnly(const BSONObj& applyOpCmd) {
    for (const auto& elem : applyOpCmd.firstElement().Obj()) {
        std::string_view opType = elem.Obj().getStringField("op");

        if (opType == "i"sv) {
            continue;
        } else if (opType == "ci"sv) {
            continue;
        } else if (opType == "d"sv) {
            continue;
        } else if (opType == "cd"sv) {
            continue;
        } else if (opType == "u"sv) {
            continue;
        } else if (opType == "cu"sv) {
            continue;
        } else if (opType == "n"sv) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace apply_ops_command_info_details

// static
ApplyOpsCommandInfo ApplyOpsCommandInfo::parse(const BSONObj& applyOpCmd) {
    try {
        return ApplyOpsCommandInfo(applyOpCmd);
    } catch (DBException& ex) {
        ex.addContext(str::stream() << "Failed to parse applyOps command: " << redact(applyOpCmd));
        throw;
    }
}

bool ApplyOpsCommandInfo::areOpsCrudOnly() const {
    return _areOpsCrudOnly;
}

ApplyOpsCommandInfo::ApplyOpsCommandInfo(const BSONObj& applyOpCmd)
    : _areOpsCrudOnly(apply_ops_command_info_details::_parseAreOpsCrudOnly(applyOpCmd)) {
    const auto tid = repl::OplogEntry::parseTid(applyOpCmd);
    const auto vts = tid
        ? boost::make_optional(auth::ValidatedTenancyScopeFactory::create(
              *tid, auth::ValidatedTenancyScopeFactory::TrustedForInnerOpMsgRequestTag{}))
        : boost::none;
    parseProtected(applyOpCmd,
                   IDLParserContext("applyOps", vts, tid, SerializationContext::stateDefault()));

    uassert(6711600,
            "applyOps command no longer supports the 'preCondition' option",
            !getPreCondition());

    uassert(6711601,
            "applyOps command no longer supports the 'alwaysUpsert' option",
            !getAlwaysUpsert());
}

// static
std::vector<OplogEntry> ApplyOps::extractOperations(const OplogEntry& applyOpsOplogEntry) {
    std::vector<OplogEntry> result;
    extractOperationsTo(applyOpsOplogEntry, applyOpsOplogEntry.getEntry().toBSON(), &result);
    return result;
}

namespace {
/**
 * This function scans a BSON object 'obj' (which is expected to have the format of an applyOps
 * oplog entry) and returns a vector of pairs <BSONElement, uint64_t> where each BSONElement refers
 * to an element in the BSON object and the uint64_t is always set to the value of 'start'.
 * Elements which should never be copied from an applyOps oplog entry to the oplog entries derived
 * from the individual operations in the applyOps are not included.
 *
 * The return vector references the original object so callers must ensure the original object
 * remains valid while the vector is in use.
 */
typedef std::pair<BSONElement, uint64_t> ElementReference;
std::vector<ElementReference> getCommonElementReferences(const BSONObj& obj, uint64_t start) {
    std::vector<ElementReference> have;
    BSONObjIterator i(obj);
    while (i.more()) {
        BSONElement el = i.next();
        // These top-level fields are never useful in an extracted operation, so we don't have to
        // track them or add them.
        //
        // Furthermore, excluding 'o', 'nss', and 'op' from the common elements means we will always
        // catch (during the final parse) operations which do not have those fields, without the
        // necessity of doing another expensive parse.
        std::string_view fieldName = el.fieldNameStringData();
        if (fieldName == OplogEntry::kObjectFieldName || fieldName == OplogEntry::kNssFieldName ||
            fieldName == OplogEntry::kOpTypeFieldName ||
            fieldName == OplogEntry::kUpsertFieldName ||
            fieldName == OplogEntry::kMultiOpTypeFieldName)
            continue;
        have.emplace_back(el, start);
    }
    return have;
}
}  // namespace

// static
void ApplyOps::extractOperationsTo(const OplogEntry& applyOpsOplogEntry,
                                   const BSONObj& topLevelDoc,
                                   std::vector<OplogEntry>* operations) {
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "ApplyOps::extractOperations(): not a command: "
                          << redact(applyOpsOplogEntry.toBSONForLogging()),
            applyOpsOplogEntry.isCommand());

    uassert(ErrorCodes::CommandNotSupported,
            str::stream() << "ApplyOps::extractOperations(): not applyOps command: "
                          << redact(applyOpsOplogEntry.toBSONForLogging()),
            OplogEntry::CommandType::kApplyOps == applyOpsOplogEntry.getCommandType());

    // The algorithm here is
    // 1. Get a vector of common elements to be added to the operations from the topLevelDoc.
    //    This vector is annotated with an integer which will hold an index value.
    // 2. Make a StringMap mapping the element names from the vector to the elements.
    // 3. For each operation in the applyOps array, copy every element to a new BSON object.
    //    If the element name is in the vector (determined using the map), mark the element
    //    in the vector with the current applyOpsIndex
    // 4. For each element in the vector, if the element is not annotated with the current
    //    index, add it to the new object.
    // 5. Parse the new object.
    //
    // This algorithm allows scanning the topLevelDoc only once, the applyOpsArray only once,
    // and creating a map only once.  Using an incrementing annotation on the vector means
    // we don't have to reset the annotations each time through.
    auto commonElements = getCommonElementReferences(topLevelDoc, ~uint64_t(0));
    StringMap<ElementReference*> commonNamesMap;
    for (auto& elementRef : commonElements) {
        commonNamesMap.insert(std::make_pair(elementRef.first.fieldName(), &elementRef));
    }
    auto operationDocs = applyOpsOplogEntry.getObject().firstElement().Obj();

    uint64_t applyOpsIdx = 0;
    BSONObj prevObj;
    for (const auto& operationDocElem : operationDocs) {
        auto operationDoc = operationDocElem.Obj();
        BSONObjBuilder builder;
        {
            BSONObjIterator it(operationDoc);
            while (it.more()) {
                BSONElement e = it.next();
                builder.append(e);
                std::string_view fieldName = e.fieldNameStringData();
                auto commonElementIter = commonNamesMap.find(fieldName);
                if (commonElementIter != commonNamesMap.end()) {
                    commonElementIter->second->second = applyOpsIdx;
                }
            }
        }
        for (auto&& elementRef : commonElements) {
            if (elementRef.second != applyOpsIdx) {
                builder.append(elementRef.first);
            }
        }
        auto operation = builder.obj();

        operations->emplace_back(operation);

        // Preserve index of operation in the "applyOps" oplog entry, timestamp, and wall clock time
        // of the "applyOps" entry.
        auto& lastOperation = operations->back();
        lastOperation.setApplyOpsIndex(applyOpsIdx);
        lastOperation.setApplyOpsTimestamp(applyOpsOplogEntry.getTimestamp());
        lastOperation.setApplyOpsWallClockTime(applyOpsOplogEntry.getWallClockTime());
        ++applyOpsIdx;
    }
}

}  // namespace repl
}  // namespace mongo
