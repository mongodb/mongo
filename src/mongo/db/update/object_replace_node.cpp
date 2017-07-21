/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/update/object_replace_node.h"

#include "mongo/base/data_view.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/service_context.h"
#include "mongo/db/update/storage_validation.h"

namespace mongo {

namespace {
constexpr StringData kIdFieldName = "_id"_sd;
}  // namespace

ObjectReplaceNode::ObjectReplaceNode(BSONObj val)
    : UpdateNode(Type::Replacement), _val(val.getOwned()), _containsId(false) {

    // Replace all zero-valued timestamps with the current time and check for the existence of _id.
    for (auto&& elem : _val) {

        // Do not change the _id field.
        if (elem.fieldNameStringData() == kIdFieldName) {
            _containsId = true;
            continue;
        }

        if (elem.type() == BSONType::bsonTimestamp) {
            auto timestampView = DataView(const_cast<char*>(elem.value()));

            // We don't need to do an endian-safe read here, because 0 is 0 either way.
            unsigned long long timestamp = timestampView.read<unsigned long long>();
            if (timestamp == 0) {
                ServiceContext* service = getGlobalServiceContext();
                auto ts = LogicalClock::get(service)->reserveTicks(1).asTimestamp();
                timestampView.write(tagLittleEndian(ts.asULL()));
            }
        }
    }
}

void ObjectReplaceNode::apply(mutablebson::Element element,
                              FieldRef* pathToCreate,
                              FieldRef* pathTaken,
                              StringData matchedField,
                              bool fromReplication,
                              bool validateForStorage,
                              const FieldRefSet& immutablePaths,
                              const UpdateIndexData* indexData,
                              LogBuilder* logBuilder,
                              bool* indexesAffected,
                              bool* noop) const {
    invariant(pathToCreate->empty());
    invariant(pathTaken->empty());

    auto original = element.getDocument().getObject();

    // Check for noop.
    if (original.binaryEqual(_val)) {
        *indexesAffected = false;
        *noop = true;
        return;
    }

    *indexesAffected = true;
    *noop = false;

    // Remove the contents of the provided document.
    auto current = element.leftChild();
    while (current.ok()) {

        // Keep the _id if the replacement document does not have one.
        if (!_containsId && current.getFieldName() == kIdFieldName) {
            current = current.rightSibling();
            continue;
        }

        auto toRemove = current;
        current = current.rightSibling();
        invariantOK(toRemove.remove());
    }

    // Insert the provided contents instead.
    for (auto&& elem : _val) {
        invariantOK(element.appendElement(elem));
    }

    // Validate for storage.
    if (validateForStorage) {
        storage_validation::storageValid(element.getDocument());
    }

    // Check immutable paths.
    for (auto path = immutablePaths.begin(); path != immutablePaths.end(); ++path) {

        // Find the updated field in the updated document.
        auto newElem = element;
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
                    newElem.getType() != BSONType::Array);
        }

        auto oldElem = dotted_path_support::extractElementAtPath(original, (*path)->dottedField());

        uassert(ErrorCodes::ImmutableField,
                str::stream() << "After applying the update, the '" << (*path)->dottedField()
                              << "' (required and immutable) field was "
                                 "found to have been removed --"
                              << original,
                newElem.ok() || !oldElem.ok());
        if (newElem.ok() && oldElem.ok()) {
            uassert(ErrorCodes::ImmutableField,
                    str::stream() << "After applying the update, the (immutable) field '"
                                  << (*path)->dottedField()
                                  << "' was found to have been altered to "
                                  << newElem.toString(),
                    newElem.compareWithBSONElement(oldElem, nullptr, false) == 0);
        }
    }

    if (logBuilder) {
        auto replacementObject = logBuilder->getDocument().end();
        invariantOK(logBuilder->getReplacementObject(&replacementObject));
        for (auto current = element.leftChild(); current.ok(); current = current.rightSibling()) {
            invariantOK(replacementObject.appendElement(current.getValue()));
        }
    }
}

}  // namespace mongo
