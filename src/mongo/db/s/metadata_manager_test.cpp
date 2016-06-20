/**
 *    Copyright (C) 2016 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/s/metadata_manager.h"

#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/metadata_loader.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using unittest::assertGet;

namespace {

TEST(MetadataManager, SetAndGetActiveMetadata) {
    MetadataManager manager;

    std::unique_ptr<CollectionMetadata> cm = stdx::make_unique<CollectionMetadata>();
    auto cmPtr = cm.get();

    manager.setActiveMetadata(std::move(cm));
    ScopedCollectionMetadata scopedMetadata = manager.getActiveMetadata();

    ASSERT_EQ(cmPtr, scopedMetadata.getMetadata());
};

TEST(MetadataManager, ResetActiveMetadata) {
    MetadataManager manager;
    manager.setActiveMetadata(stdx::make_unique<CollectionMetadata>());

    ScopedCollectionMetadata scopedMetadata1 = manager.getActiveMetadata();

    std::unique_ptr<CollectionMetadata> cm2 = stdx::make_unique<CollectionMetadata>();
    auto cm2Ptr = cm2.get();

    manager.setActiveMetadata(std::move(cm2));
    ScopedCollectionMetadata scopedMetadata2 = manager.getActiveMetadata();

    ASSERT_EQ(cm2Ptr, scopedMetadata2.getMetadata());
};

}  // namespace
}  // namespace mongo
