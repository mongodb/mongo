/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/derived_metadata_common.h"

namespace mongo {

/*
 * Responsible for faciliting updates to DerivedMetadataTypes.
 */
class DerivedMetadataController {
public:
    DerivedMetadataController() = default;
    virtual ~DerivedMetadataController() = default;

    /*
     * Creates a Delta from the write pre and post images. The Delta contains changes to the
     * Collection's DerivedMetadataTypes as a result of the write. The Delta is then registered with
     * the Collection's DerivedMetadataCollectionController and will be asynchronously applied to
     * the DerivedMetadataTypes.
     */
    static void createAndRegister(OperationContext* opCtx,
                                  const Collection* collection,
                                  const BSONObj& writePreImage,
                                  const BSONObj& writePostImage);

    /*
     * Pulls Deltas (up to Timestamp ts) from the DerivedMetadataCollectionController and applies
     * them to the Collection's DerivedMetadataTypes.
     */
    static void pullAndApplyDeltas(const Collection* collection, const Timestamp& ts);

private:
    /*
     * Accesses the DerivedMetadataTypes decorating the Collection to generate a Delta from the
     * write.
     */
    static DerivedMetadataDelta _createDelta(const DocumentWriteImages& write);

    /*
     * Registers an onCommit handler to pass along the Delta to the
     * DerivedMetadataCollectionController.
     */
    static void _registerDeltaOnCommit(OperationContext* opCtx,
                                       DerivedMetadataDelta delta,
                                       const Collection* collection);
};
}  // namespace mongo
