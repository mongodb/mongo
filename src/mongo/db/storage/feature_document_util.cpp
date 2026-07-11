// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/feature_document_util.h"

namespace mongo {

namespace {
using namespace std::literals::string_view_literals;
static constexpr auto kIsFeatureDocumentFieldName = "isFeatureDoc"sv;
}  // namespace

namespace feature_document_util {
bool isFeatureDocument(const BSONObj& obj) {
    BSONElement firstElem = obj.firstElement();
    if (firstElem.fieldNameStringData() == kIsFeatureDocumentFieldName) {
        return firstElem.booleanSafe();
    }
    return false;
}
}  // namespace feature_document_util

}  // namespace mongo
