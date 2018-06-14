/**
*    Copyright (C) 2017 MongoDB Inc.
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

#pragma once

#include "mongo/base/disallow_copying.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/db/repl/replication_consistency_markers_gen.h"

namespace mongo {

class BSONObj;
class OperationContext;
class Timestamp;

namespace repl {

class OpTime;
class StorageInterface;
struct TimestampedBSONObj;

class ReplicationConsistencyMarkersImpl : public ReplicationConsistencyMarkers {
    MONGO_DISALLOW_COPYING(ReplicationConsistencyMarkersImpl);

public:
    static constexpr StringData kDefaultMinValidNamespace = "local.replset.minvalid"_sd;
    static constexpr StringData kDefaultOplogTruncateAfterPointNamespace =
        "local.replset.oplogTruncateAfterPoint"_sd;

    explicit ReplicationConsistencyMarkersImpl(StorageInterface* storageInterface);
    ReplicationConsistencyMarkersImpl(StorageInterface* storageInterface,
                                      NamespaceString minValidNss,
                                      NamespaceString oplogTruncateAfterNss);

    void initializeMinValidDocument(OperationContext* opCtx) override;

    bool getInitialSyncFlag(OperationContext* opCtx) const override;
    void setInitialSyncFlag(OperationContext* opCtx) override;
    void clearInitialSyncFlag(OperationContext* opCtx) override;

    OpTime getMinValid(OperationContext* opCtx) const override;
    void setMinValid(OperationContext* opCtx, const OpTime& minValid) override;
    void setMinValidToAtLeast(OperationContext* opCtx, const OpTime& minValid) override;

    void setOplogTruncateAfterPoint(OperationContext* opCtx, const Timestamp& timestamp) override;
    Timestamp getOplogTruncateAfterPoint(OperationContext* opCtx) const override;

    void setAppliedThrough(OperationContext* opCtx, const OpTime& optime) override;
    void clearAppliedThrough(OperationContext* opCtx, const Timestamp& writeTimestamp) override;
    OpTime getAppliedThrough(OperationContext* opCtx) const override;

    Status createInternalCollections(OperationContext* opCtx);

private:
    /**
     * Reads the MinValid document from disk.
     * Returns boost::none if not present.
     */
    boost::optional<MinValidDocument> _getMinValidDocument(OperationContext* opCtx) const;

    /**
     * Updates the MinValid document according to the provided update spec. The collection must
     * exist, see `createInternalCollections`. If the document does not exist, it is upserted.
     *
     * This fasserts on failure.
     */
    void _updateMinValidDocument(OperationContext* opCtx, const TimestampedBSONObj& updateSpec);

    /**
     * Reads the OplogTruncateAfterPoint document from disk.
     * Returns boost::none if not present.
     */
    boost::optional<OplogTruncateAfterPointDocument> _getOplogTruncateAfterPointDocument(
        OperationContext* opCtx) const;

    /**
     * Upserts the OplogTruncateAfterPoint document according to the provided update spec. The
     * collection must already exist. See `createInternalCollections`.
     *
     * This fasserts on failure.
     */
    void _upsertOplogTruncateAfterPointDocument(OperationContext* opCtx, const BSONObj& updateSpec);

    StorageInterface* _storageInterface;
    const NamespaceString _minValidNss;
    const NamespaceString _oplogTruncateAfterPointNss;
};

}  // namespace repl
}  // namespace mongo
