/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_mock.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/sorter/container_based_spiller.h"
#include "mongo/db/sorter/container_test_utils.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/db/sorter/sorter_file_name.h"
#include "mongo/db/sorter/sorter_template_defs.h"
#include "mongo/db/sorter/sorter_test_utils.h"
#include "mongo/db/storage/ident.h"
#include "mongo/unittest/unittest.h"

#include <concepts>
#include <cstddef>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem/path.hpp>

namespace mongo::sorter::test {

struct SpillStorageState {
    std::string storageIdentifier;
    std::vector<SorterRange> ranges;
    SortOptions opts;
    IWComparator comp{ASC};
};

inline std::unique_ptr<FileBasedSorterSpiller<IntWrapper, IntWrapper>> makeFileSorterSpiller(
    const SortOptions& opts, SorterFileStats* fileStats, std::string storageIdentifier = "") {
    ASSERT(opts.tempDir);

    if (storageIdentifier.empty()) {
        return std::make_unique<FileBasedSorterSpiller<IntWrapper, IntWrapper>>(*opts.tempDir,
                                                                                fileStats);
    }
    return std::make_unique<FileBasedSorterSpiller<IntWrapper, IntWrapper>>(
        std::make_shared<SorterFile>(*opts.tempDir / storageIdentifier, fileStats), *opts.tempDir);
}

template <typename Traits>
concept StorageTraits = requires(Traits& traits,
                                 SorterTracker* tracker,
                                 const boost::filesystem::path& storageLocation,
                                 const SortOptions& opts,
                                 const std::string& storageIdentifier,
                                 SpillStorageState& spillState) {
    { Traits::kHasFileStats } -> std::convertible_to<bool>;
    { Traits::kEmptyStorageErrorCode } -> std::convertible_to<int>;
    { Traits::kCorruptedStorageErrorCode } -> std::convertible_to<int>;
    {
        traits.makeSpiller(opts)
    } -> std::same_as<std::shared_ptr<SorterSpiller<IntWrapper, IntWrapper>>>;
    {
        traits.makeSpillerForResume(opts, storageIdentifier)
    } -> std::same_as<std::shared_ptr<SorterSpiller<IntWrapper, IntWrapper>>>;
    {
        traits.makeWriter(opts)
    } -> std::same_as<std::unique_ptr<SortedStorageWriter<IntWrapper, IntWrapper>>>;
    { traits.makeEmptyStorage(storageLocation) } -> std::same_as<std::string>;
    { traits.makeCorruptedStorage(storageLocation) } -> std::same_as<std::string>;
    { traits.makeSpillState(storageLocation) } -> std::same_as<SpillStorageState>;
    { traits.corruptSpillState(spillState) } -> std::same_as<void>;
    { traits.iteratorSizeBytes() } -> std::same_as<std::size_t>;
};

struct FileTraits {
    static constexpr bool kHasFileStats = true;
    static constexpr int kEmptyStorageErrorCode = 16815;
    static constexpr int kCorruptedStorageErrorCode = 16817;

    static std::shared_ptr<SorterSpiller<IntWrapper, IntWrapper>> makeSpiller(
        const SortOptions& opts) {
        return std::shared_ptr<SorterSpiller<IntWrapper, IntWrapper>>(
            makeFileSorterSpiller(opts, /*fileStats=*/nullptr));
    }

    static std::shared_ptr<SorterSpiller<IntWrapper, IntWrapper>> makeSpillerForResume(
        const SortOptions& opts, const std::string& storageIdentifier) {
        return std::shared_ptr<SorterSpiller<IntWrapper, IntWrapper>>(
            makeFileSorterSpiller(opts, /*fileStats=*/nullptr, storageIdentifier));
    }

    static std::unique_ptr<SortedStorageWriter<IntWrapper, IntWrapper>> makeWriter(
        const SortOptions& opts) {
        invariant(opts.tempDir);
        auto spillFile = std::make_shared<SorterFile>(sorter::nextFileName(*opts.tempDir), nullptr);
        return std::make_unique<SortedFileWriter<IntWrapper, IntWrapper>>(opts, spillFile);
    }

    static std::string makeEmptyStorage(const boost::filesystem::path& storageLocation) {
        auto storagePath = storageLocation / "empty_sorter_storage";
        std::ofstream ofs(storagePath.string());
        ASSERT(ofs) << "failed to create empty temporary file: " << storagePath.string();
        return storagePath.filename().string();
    }

    static std::string makeCorruptedStorage(const boost::filesystem::path& storageLocation) {
        auto storagePath = storageLocation / "corrupted_sorter_storage";
        std::ofstream ofs(storagePath.string());
        ASSERT(ofs) << "failed to create temporary file: " << storagePath.string();
        ofs << "invalid sorter data";
        return storagePath.filename().string();
    }

    static SpillStorageState makeSpillState(const boost::filesystem::path& storageLocation) {
        SpillStorageState ret;
        ret.opts = SortOptions().TempDir(storageLocation).Tracker(nullptr);

        auto sorter = IWSorter::make(ret.opts, ret.comp, makeSpiller(ret.opts));
        for (int i = 0; i < 10; ++i) {
            sorter->add(i, -i);
        }
        auto state = sorter->persistDataForShutdown();
        ret.storageIdentifier = std::move(state.storageIdentifier);
        ret.ranges = std::move(state.ranges);
        return ret;
    }

    static void corruptSpillState(SpillStorageState& state) {
        auto& range = state.ranges[0];
        range.setChecksum(range.getChecksum() ^ 1);
    }

    static std::size_t iteratorSizeBytes() {
        return MergeableSorter<IntWrapper, IntWrapper>::kFileIteratorSize;
    }
};

// TODO SERVER-117268: Complete.
struct ContainerTraits {
    static constexpr bool kHasFileStats = false;
    static constexpr int kEmptyStorageErrorCode = 0;
    static constexpr int kCorruptedStorageErrorCode = 0;

    explicit ContainerTraits(ServiceContext::UniqueOperationContext opCtx)
        : _opCtx(std::move(opCtx)),
          _containerStats(&_tracker),
          _coll(std::make_shared<CollectionMock>(
              NamespaceString::createNamespaceString_forTest("test", "coll"))),
          _collPtr(_coll.get()) {
        auto* replCoord = repl::ReplicationCoordinator::get(_opCtx.get());
        auto* replCoordMock = dynamic_cast<repl::ReplicationCoordinatorMock*>(replCoord);
        ASSERT(replCoordMock);
        replCoordMock->alwaysAllowWrites(true);
        _container.setIdent(std::make_shared<Ident>("sorted_storage_iterator_container"));
    }

    static std::shared_ptr<SorterSpiller<IntWrapper, IntWrapper>> makeSpiller(
        const SortOptions& opts) {
        MONGO_UNIMPLEMENTED;
    }

    static std::shared_ptr<SorterSpiller<IntWrapper, IntWrapper>> makeSpillerForResume(
        const SortOptions& opts, const std::string& storageIdentifier) {
        MONGO_UNIMPLEMENTED;
    }

    std::unique_ptr<SortedStorageWriter<IntWrapper, IntWrapper>> makeWriter(
        const SortOptions& opts) {
        auto& ru = *shard_role_details::getRecoveryUnit(_opCtx.get());
        const auto settings = SortedContainerWriter<IntWrapper, IntWrapper>::Settings{};
        return std::make_unique<SortedContainerWriter<IntWrapper, IntWrapper>>(
            *_opCtx, ru, _collPtr, _container, _containerStats, opts, _nextKey, settings);
    }

    static std::string makeEmptyStorage(const boost::filesystem::path& storageLocation) {
        MONGO_UNIMPLEMENTED;
    }

    static std::string makeCorruptedStorage(const boost::filesystem::path& storageLocation) {
        MONGO_UNIMPLEMENTED;
    }

    static SpillStorageState makeSpillState(const boost::filesystem::path& storageLocation) {
        MONGO_UNIMPLEMENTED;
    }

    static void corruptSpillState(SpillStorageState& state) {
        MONGO_UNIMPLEMENTED;
    }

    static std::size_t iteratorSizeBytes() {
        MONGO_UNIMPLEMENTED;
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
    SorterTracker _tracker;
    SorterContainerStats _containerStats;
    ViewableIntegerKeyedContainer _container;
    std::shared_ptr<CollectionMock> _coll;
    CollectionPtr _collPtr;
    int64_t _nextKey = 1;
};

}  // namespace mongo::sorter::test
