// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/update/object_replace_executor.h"

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/dotted_path/dotted_path_support.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/field_ref_set.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/vector_clock/vector_clock_mutable.h"
#include "mongo/db/update/storage_validation.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <string_view>

namespace mongo {

namespace {
using namespace std::literals::string_view_literals;
constexpr std::string_view kIdFieldName = "_id"sv;
}  // namespace

ObjectReplaceExecutor::ObjectReplaceExecutor(BSONObj replacement, bool bypassEmptyTsReplacement)
    : _replacementDoc(replacement.getOwned()),
      _containsId(false),
      _bypassEmptyTsReplacement(bypassEmptyTsReplacement) {
    // Check for the existence of the "_id" field, and if approrpriate replace all zero-valued
    // timestamps with the current time.
    for (auto&& elem : _replacementDoc) {
        // Do not change the _id field.
        if (elem.fieldNameStringData() == kIdFieldName) {
            _containsId = true;
            continue;
        }

        // For updates that originated from the oplog, we're required to apply the update
        // exactly as it was recorded (even if it contains zero-valued timestamps). Therefore,
        // we should only replace zero-valued timestamps with the current time when
        // '_bypassEmptyTsReplacement' is false.
        if (!_bypassEmptyTsReplacement && elem.type() == BSONType::timestamp) {
            auto timestampView = DataView(const_cast<char*>(elem.value()));

            // We don't need to do an endian-safe read here, because 0 is 0 either way.
            unsigned long long timestamp = timestampView.read<unsigned long long>();
            if (timestamp == 0) {
                ServiceContext* service = getGlobalServiceContext();
                if (auto vectorClock = VectorClockMutable::get(service); vectorClock) {
                    auto ts = vectorClock->tickClusterTime(1).asTimestamp();
                    timestampView.write(tagLittleEndian(ts.asULL()));
                }
            }
        }
    }
}

UpdateExecutor::ApplyResult ObjectReplaceExecutor::applyReplacementUpdate(
    ApplyParams applyParams,
    const BSONObj& replacementDoc,
    bool replacementDocContainsIdField,
    bool allowTopLevelDollarPrefixedFields) {
    auto originalDoc = applyParams.element.getDocument().getObject();

    // Check for noop.
    if (originalDoc.binaryEqual(replacementDoc)) {
        return ApplyResult::noopResult();
    }

    // Remove the contents of the provided document.
    auto current = applyParams.element.leftChild();
    while (current.ok()) {

        // Keep the _id if the replacement document does not have one.
        if (!replacementDocContainsIdField && current.getFieldName() == kIdFieldName) {
            current = current.rightSibling();
            continue;
        }

        auto toRemove = current;
        current = current.rightSibling();
        invariant(toRemove.remove());
    }

    // Insert the provided contents instead.
    for (auto&& elem : replacementDoc) {
        invariant(applyParams.element.appendElement(elem));
    }

    ApplyResult ret;

    if (!applyParams.skipDotsDollarsCheck || applyParams.validateForStorage) {
        // Validate for storage and check if the document contains any '.'/'$' field name. We pass
        // 'containsDotsAndDollarsField' unconditionally for now.
        storage_validation::scanDocument(applyParams.element.getDocument(),
                                         allowTopLevelDollarPrefixedFields,
                                         applyParams.validateForStorage,
                                         &ret.containsDotsAndDollarsField,
                                         applyParams.fromOplogApplication);
    }

    // Check immutable paths.
    for (auto path = applyParams.immutablePaths.begin(); path != applyParams.immutablePaths.end();
         ++path) {

        // Find the updated field in the updated document.
        auto newElem = applyParams.element;
        for (size_t i = 0; i < (*path)->numParts(); ++i) {
            newElem = newElem[(*path)->getPart(i)];
            if (!newElem.ok()) {
                break;
            }
            uassert(ErrorCodes::NotSingleValueField,
                    str::stream()
                        << "After applying the update to the document, the (immutable) field '"
                        << (*path)->dottedField()
                        << "' was found to be an array or array descendant.",
                    newElem.getType() != BSONType::array);
        }

        auto oldElem = bson::extractElementAtDottedPath(originalDoc, (*path)->dottedField());

        uassert(ErrorCodes::ImmutableField,
                str::stream() << "After applying the update, the '" << (*path)->dottedField()
                              << "' (required and immutable) field was "
                                 "found to have been removed --"
                              << originalDoc,
                newElem.ok() || !oldElem.ok());
        if (newElem.ok() && oldElem.ok()) {
            uassert(ErrorCodes::ImmutableField,
                    str::stream() << "After applying the update, the (immutable) field '"
                                  << (*path)->dottedField()
                                  << "' was found to have been altered to " << newElem.toString(),
                    newElem.compareWithBSONElement(oldElem, nullptr, false) == 0);
        }
    }

    return ret;
}

UpdateExecutor::ApplyResult ObjectReplaceExecutor::applyUpdate(ApplyParams applyParams) const {
    auto ret = applyReplacementUpdate(applyParams, _replacementDoc, _containsId);

    if (!ret.noop && applyParams.logMode != ApplyParams::LogMode::kDoNotGenerateOplogEntry) {
        ret.oplogEntry = update_oplog_entry::makeReplacementOplogEntry(
            applyParams.element.getDocument().getObject());
    }
    return ret;
}
}  // namespace mongo
