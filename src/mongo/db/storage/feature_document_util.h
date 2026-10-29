// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

/**
   A light-weight util library that provides functionality to assess whether a document is a
   feature document. Feature documents were documents with empty namespaces that we no longer
   generate as of 5.1, but to allow for backwards compatibility we still need to be able to account
   for them.
*/
namespace mongo::feature_document_util {
/**
 *  Allows featureDocuments to be checked with older versions.
 */
bool isFeatureDocument(const BSONObj& obj);
}  // namespace mongo::feature_document_util
