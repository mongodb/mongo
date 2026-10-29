// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/sorter/file.h"

#include "mongo/db/sorter/sorter_stats.h"
#include "mongo/db/stats/counters_sort.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

#include <fstream>
#include <string>

#include <boost/filesystem/operations.hpp>

namespace mongo::sorter {
namespace {

int64_t getSpillStorageSize() {
    return fileSpillingMetrics.fileSpilledStorageSize.get();
}

boost::filesystem::path makeFilePath(const unittest::TempDir& dir, const std::string& name) {
    return boost::filesystem::path{dir.path()} / name;
}

// Writing to a file-backed sorter file raises the gauge by the bytes written, and destroying the
// file returns the gauge to its baseline.
TEST(SorterFileGaugeTest, GaugeRisesOnWriteAndFallsOnDestruction) {
    unittest::TempDir dir{"sorter_file_gauge_test"};
    SorterTracker tracker;
    SorterFileStats stats{&tracker};

    const int64_t baseline = getSpillStorageSize();
    const std::string data(1024, 'x');

    {
        File file{makeFilePath(dir, "spill"), &stats};
        EXPECT_EQ(getSpillStorageSize(), baseline);

        file.write(data.c_str(), data.size());
        EXPECT_EQ(getSpillStorageSize(), baseline + static_cast<int64_t>(data.size()));

        // A second write accumulates.
        file.write(data.c_str(), data.size());
        EXPECT_EQ(getSpillStorageSize(), baseline + 2 * static_cast<int64_t>(data.size()));
    }

    // The file is removed on destruction, so the gauge returns to baseline.
    EXPECT_EQ(getSpillStorageSize(), baseline);
}

// A file that is never written leaves the gauge untouched.
TEST(SorterFileGaugeTest, GaugeUnchangedForUnwrittenFile) {
    unittest::TempDir dir{"sorter_file_gauge_test"};
    const int64_t baseline = getSpillStorageSize();
    {
        File file{makeFilePath(dir, "spill"), nullptr};
        EXPECT_EQ(getSpillStorageSize(), baseline);
    }
    EXPECT_EQ(getSpillStorageSize(), baseline);
}

// A kept file is not removed on destruction, so its bytes remain counted in the gauge because it is
// still occupying local disk.
TEST(SorterFileGaugeTest, KeptFileRemainsCountedWhileOnDisk) {
    unittest::TempDir dir{"sorter_file_gauge_test"};
    const auto path = makeFilePath(dir, "spill");
    const int64_t baseline = getSpillStorageSize();
    const std::string data(2048, 'y');

    {
        File file{path, nullptr};
        file.write(data.c_str(), data.size());
        file.keep();
        EXPECT_EQ(getSpillStorageSize(), baseline + static_cast<int64_t>(data.size()));
    }
    // The on-disk file was kept, so the gauge still reflects its size after destruction. (The bytes
    // remain counted for the life of the process; each test above measures against its own baseline
    // so this residual is harmless.)
    EXPECT_EQ(getSpillStorageSize(), baseline + static_cast<int64_t>(data.size()));
    ASSERT_TRUE(boost::filesystem::exists(path));
}

// A File that adopts a pre-existing on-disk file (e.g. resuming persisted spill state) accounts for
// its size, and releases it when the file is removed on destruction.
TEST(SorterFileGaugeTest, AdoptingExistingFileCountsItsSize) {
    unittest::TempDir dir{"sorter_file_gauge_test"};
    const auto path = makeFilePath(dir, "spill");

    // Create an on-disk file outside of any File object.
    const std::string data(4096, 'z');
    {
        std::ofstream raw{path.string(), std::ios::binary};
        raw.write(data.c_str(), data.size());
    }

    const int64_t baseline = getSpillStorageSize();
    {
        File file{path, nullptr};
        EXPECT_EQ(getSpillStorageSize(), baseline + static_cast<int64_t>(data.size()));
    }
    // The file is removed on destruction, releasing its bytes.
    EXPECT_EQ(getSpillStorageSize(), baseline);
    ASSERT_FALSE(boost::filesystem::exists(path));
}

}  // namespace
}  // namespace mongo::sorter
