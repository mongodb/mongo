/**
*    Copyright (C) 2014 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/index_key_validate.h"

#include <boost/optional.hpp>
#include <cmath>
#include <limits>
#include <set>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_names.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/represent_as.h"

namespace mongo {

using std::string;

using IndexVersion = IndexDescriptor::IndexVersion;

namespace {
// When the skipIndexCreateFieldNameValidation failpoint is enabled, validation for index field
// names will be disabled. This will allow for creation of indexes with invalid field names in their
// specification.
MONGO_FP_DECLARE(skipIndexCreateFieldNameValidation);
}

Status validateKeyPattern(const BSONObj& key) {
    const ErrorCodes::Error code = ErrorCodes::CannotCreateIndex;

    if (key.objsize() > 2048)
        return Status(code, "Index key pattern too large.");

    if (key.isEmpty())
        return Status(code, "Index keys cannot be empty.");

    string pluginName = IndexNames::findPluginName(key);
    if (pluginName.size()) {
        if (!IndexNames::isKnownName(pluginName))
            return Status(
                code, mongoutils::str::stream() << "Unknown index plugin '" << pluginName << '\'');
    }

    BSONObjIterator it(key);
    while (it.more()) {
        BSONElement keyElement = it.next();

        if (keyElement.isNumber()) {
            double value = keyElement.number();
            if (std::isnan(value)) {
                return {code, "Values in the index key pattern cannot be NaN."};
            } else if (value == 0.0) {
                return {code, "Values in the index key pattern cannot be 0."};
            }
        } else if (keyElement.type() != String) {
            return {code,
                    str::stream() << "Values in index key pattern cannot be of type "
                                  << typeName(keyElement.type())
                                  << ". Only numbers > 0, numbers < 0, and strings are allowed."};
        }

        if (keyElement.type() == String && pluginName != keyElement.str()) {
            return Status(code, "Can't use more than one index plugin for a single index.");
        }

        // Ensure that the fields on which we are building the index are valid: a field must not
        // begin with a '$' unless it is part of a DBRef or text index, and a field path cannot
        // contain an empty field. If a field cannot be created or updated, it should not be
        // indexable.

        FieldRef keyField(keyElement.fieldName());

        const size_t numParts = keyField.numParts();
        if (numParts == 0) {
            return Status(code, "Index keys cannot be an empty field.");
        }

        // "$**" is acceptable for a text index.
        if (mongoutils::str::equals(keyElement.fieldName(), "$**") &&
            keyElement.valuestrsafe() == IndexNames::TEXT)
            continue;

        if (mongoutils::str::equals(keyElement.fieldName(), "_fts") &&
            keyElement.valuestrsafe() != IndexNames::TEXT) {
            return Status(code, "Index key contains an illegal field name: '_fts'");
        }

        for (size_t i = 0; i != numParts; ++i) {
            const StringData part = keyField.getPart(i);

            // Check if the index key path contains an empty field.
            if (part.empty()) {
                return Status(code, "Index keys cannot contain an empty field.");
            }

            if (part[0] != '$')
                continue;

            // Check if the '$'-prefixed field is part of a DBRef: since we don't have the
            // necessary context to validate whether this is a proper DBRef, we allow index
            // creation on '$'-prefixed names that match those used in a DBRef.
            const bool mightBePartOfDbRef =
                (i != 0) && (part == "$db" || part == "$id" || part == "$ref");

            if (!mightBePartOfDbRef) {
                return Status(code,
                              "Index key contains an illegal field name: "
                              "field name starts with '$'.");
            }
        }
    }

    return Status::OK();
}

StatusWith<BSONObj> validateIndexSpec(
    const BSONObj& indexSpec,
    const NamespaceString& expectedNamespace,
    ServerGlobalParams::FeatureCompatibility::Version featureCompatibilityVersion) {
    bool hasKeyPatternField = false;
    bool hasNamespaceField = false;
    bool hasVersionField = false;
    bool hasCollationField = false;

    auto fieldNamesValidStatus = validateIndexSpecFieldNames(indexSpec);
    if (!fieldNamesValidStatus.isOK()) {
        return fieldNamesValidStatus;
    }

    boost::optional<IndexVersion> resolvedIndexVersion;

    for (auto&& indexSpecElem : indexSpec) {
        auto indexSpecElemFieldName = indexSpecElem.fieldNameStringData();
        if (IndexDescriptor::kKeyPatternFieldName == indexSpecElemFieldName) {
            if (indexSpecElem.type() != BSONType::Object) {
                return {ErrorCodes::TypeMismatch,
                        str::stream() << "The field '" << IndexDescriptor::kKeyPatternFieldName
                                      << "' must be an object, but got "
                                      << typeName(indexSpecElem.type())};
            }

            std::vector<StringData> keys;
            for (auto&& keyElem : indexSpecElem.Obj()) {
                auto keyElemFieldName = keyElem.fieldNameStringData();
                if (std::find(keys.begin(), keys.end(), keyElemFieldName) != keys.end()) {
                    return {ErrorCodes::BadValue,
                            str::stream() << "The field '" << keyElemFieldName
                                          << "' appears multiple times in the index key pattern "
                                          << indexSpecElem.Obj()};
                }
                keys.push_back(keyElemFieldName);
            }

            hasKeyPatternField = true;
        } else if (IndexDescriptor::kNamespaceFieldName == indexSpecElemFieldName) {
            if (indexSpecElem.type() != BSONType::String) {
                return {ErrorCodes::TypeMismatch,
                        str::stream() << "The field '" << IndexDescriptor::kNamespaceFieldName
                                      << "' must be a string, but got "
                                      << typeName(indexSpecElem.type())};
            }

            StringData ns = indexSpecElem.valueStringData();
            if (ns.empty()) {
                return {ErrorCodes::BadValue,
                        str::stream() << "The field '" << IndexDescriptor::kNamespaceFieldName
                                      << "' cannot be an empty string"};
            }

            if (ns != expectedNamespace.ns()) {
                return {ErrorCodes::BadValue,
                        str::stream() << "The value of the field '"
                                      << IndexDescriptor::kNamespaceFieldName
                                      << "' ("
                                      << ns
                                      << ") doesn't match the namespace '"
                                      << expectedNamespace.ns()
                                      << "'"};
            }

            hasNamespaceField = true;
        } else if (IndexDescriptor::kIndexVersionFieldName == indexSpecElemFieldName) {
            if (!indexSpecElem.isNumber()) {
                return {ErrorCodes::TypeMismatch,
                        str::stream() << "The field '" << IndexDescriptor::kIndexVersionFieldName
                                      << "' must be a number, but got "
                                      << typeName(indexSpecElem.type())};
            }

            auto requestedIndexVersionAsInt = representAs<int>(indexSpecElem.number());
            if (!requestedIndexVersionAsInt) {
                return {ErrorCodes::BadValue,
                        str::stream()
                            << "Index version must be representable as a 32-bit integer, but got "
                            << indexSpecElem.toString(false, false)};
            }

            const IndexVersion requestedIndexVersion =
                static_cast<IndexVersion>(*requestedIndexVersionAsInt);
            auto creationAllowedStatus = IndexDescriptor::isIndexVersionAllowedForCreation(
                requestedIndexVersion, featureCompatibilityVersion, indexSpec);
            if (!creationAllowedStatus.isOK()) {
                return creationAllowedStatus;
            }

            hasVersionField = true;
            resolvedIndexVersion = requestedIndexVersion;
        } else if (IndexDescriptor::kCollationFieldName == indexSpecElemFieldName) {
            if (indexSpecElem.type() != BSONType::Object) {
                return {ErrorCodes::TypeMismatch,
                        str::stream() << "The field '" << IndexDescriptor::kNamespaceFieldName
                                      << "' must be an object, but got "
                                      << typeName(indexSpecElem.type())};
            }

            hasCollationField = true;
        } else {
            // We can assume field name is valid at this point. Validation of fieldname is handled
            // prior to this in validateIndexSpecFieldNames().
            continue;
        }
    }

    if (!resolvedIndexVersion) {
        resolvedIndexVersion = IndexDescriptor::getDefaultIndexVersion(featureCompatibilityVersion);
    }

    if (!hasKeyPatternField) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "The '" << IndexDescriptor::kKeyPatternFieldName
                              << "' field is a required property of an index specification"};
    }

    if (hasCollationField && *resolvedIndexVersion < IndexVersion::kV2) {
        return {ErrorCodes::CannotCreateIndex,
                str::stream() << "Invalid index specification " << indexSpec
                              << "; cannot create an index with the '"
                              << IndexDescriptor::kCollationFieldName
                              << "' option and "
                              << IndexDescriptor::kIndexVersionFieldName
                              << "="
                              << static_cast<int>(*resolvedIndexVersion)};
    }

    if (!hasNamespaceField || !hasVersionField) {
        BSONObjBuilder bob;

        if (!hasNamespaceField) {
            // We create a new index specification with the 'ns' field set as 'expectedNamespace' if
            // the field was omitted.
            bob.append(IndexDescriptor::kNamespaceFieldName, expectedNamespace.ns());
        }

        if (!hasVersionField) {
            // We create a new index specification with the 'v' field set as 'defaultIndexVersion'
            // if the field was omitted.
            bob.append(IndexDescriptor::kIndexVersionFieldName,
                       static_cast<int>(*resolvedIndexVersion));
        }

        bob.appendElements(indexSpec);
        return bob.obj();
    }

    return indexSpec;
}

/**
 * Top-level index spec field names are validated here. When adding a new field with a document as
 * value, is the the sub-module's responsibility to ensure that the content is valid and that only
 * expected fields are present at creation time
 */
Status validateIndexSpecFieldNames(const BSONObj& indexSpec) {
    if (MONGO_FAIL_POINT(skipIndexCreateFieldNameValidation)) {
        return Status::OK();
    }

    const std::set<StringData> allowedFieldNames = {
        IndexDescriptor::k2dIndexMaxFieldName,
        IndexDescriptor::k2dIndexBitsFieldName,
        IndexDescriptor::k2dIndexMaxFieldName,
        IndexDescriptor::k2dIndexMinFieldName,
        IndexDescriptor::k2dsphereCoarsestIndexedLevel,
        IndexDescriptor::k2dsphereFinestIndexedLevel,
        IndexDescriptor::k2dsphereVersionFieldName,
        IndexDescriptor::kBackgroundFieldName,
        IndexDescriptor::kCollationFieldName,
        IndexDescriptor::kDefaultLanguageFieldName,
        IndexDescriptor::kDropDuplicatesFieldName,
        IndexDescriptor::kExpireAfterSecondsFieldName,
        IndexDescriptor::kGeoHaystackBucketSize,
        IndexDescriptor::kIndexNameFieldName,
        IndexDescriptor::kIndexVersionFieldName,
        IndexDescriptor::kKeyPatternFieldName,
        IndexDescriptor::kLanguageOverrideFieldName,
        IndexDescriptor::kNamespaceFieldName,
        IndexDescriptor::kPartialFilterExprFieldName,
        IndexDescriptor::kSparseFieldName,
        IndexDescriptor::kStorageEngineFieldName,
        IndexDescriptor::kTextVersionFieldName,
        IndexDescriptor::kUniqueFieldName,
        IndexDescriptor::kWeightsFieldName,
        // Index creation under legacy writeMode can result in an index spec with an _id field.
        "_id"};

    for (auto&& indexSpecElem : indexSpec) {
        auto indexSpecElemFieldName = indexSpecElem.fieldNameStringData();
        if (!allowedFieldNames.count(indexSpecElemFieldName)) {
            return {ErrorCodes::InvalidIndexSpecificationOption,
                    str::stream() << "The field '" << indexSpecElemFieldName
                                  << "' is not valid for an index specification. Specification: "
                                  << indexSpec};
        }
    }

    return Status::OK();
}

}  // namespace mongo
