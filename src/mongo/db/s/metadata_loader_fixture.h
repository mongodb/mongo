/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include <vector>

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/db/s/metadata_loader.h"
#include "mongo/platform/basic.h"
#include "mongo/s/catalog/replset/catalog_manager_replica_set_test_fixture.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"

namespace mongo {

using std::unique_ptr;

class MetadataLoaderFixture : public CatalogManagerReplSetTestFixture {
public:
    MetadataLoaderFixture();
    ~MetadataLoaderFixture();

protected:
    static const std::string CONFIG_HOST_PORT;

    void setUp() override;

    void expectFindOnConfigSendErrorCode(ErrorCodes::Error code);
    void expectFindOnConfigSendBSONObjVector(std::vector<BSONObj> obj);

    void expectFindOnConfigSendCollectionDefault();
    void expectFindOnConfigSendChunksDefault();

    MetadataLoader& loader() const;

    void getMetadataFor(const OwnedPointerVector<ChunkType>& chunks, CollectionMetadata* metadata);

    ChunkVersion getMaxCollVersion() const;
    ChunkVersion getMaxShardVersion() const;

    OID _epoch;

private:
    ChunkVersion _maxCollVersion;
    unique_ptr<MetadataLoader> _loader;
    const HostAndPort configHost{HostAndPort(CONFIG_HOST_PORT)};
};

}  // namespace mongo
