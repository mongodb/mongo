// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/memory_charging.h"

#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>

namespace mongo {
namespace {

class TestCharging : public MemoryCharging {
public:
    void chargeBytes(int64_t bytes) override {
        charged += bytes;
    }

    void releaseBytes(int64_t bytes) override {
        released += bytes;
    }

    int64_t charged = 0;
    int64_t released = 0;
};

TEST(MemoryChargingTest, UntrackedByDefault) {
    ASSERT_EQ(currentMemoryCharging(), nullptr);
    // No-ops when nothing is installed.
    chargeAmbient(10);
    releaseAmbient(10);
}

TEST(MemoryChargingTest, GuardInstallsAndRestores) {
    TestCharging charging;
    {
        AmbientMemoryChargingGuard guard(&charging);
        ASSERT_EQ(currentMemoryCharging(), &charging);

        chargeAmbient(16);
        releaseAmbient(6);
        ASSERT_EQ(charging.charged, 16);
        ASSERT_EQ(charging.released, 6);
    }
    ASSERT_EQ(currentMemoryCharging(), nullptr);

    // Nothing is charged once the guard is gone.
    chargeAmbient(16);
    ASSERT_EQ(charging.charged, 16);
}

TEST(MemoryChargingTest, NestedGuardsRestoreInReverseOrderAndOnlyTheAmbientChargerCharges) {
    TestCharging optimisation, storage, execution;
    {
        AmbientMemoryChargingGuard a(&optimisation);
        chargeAmbient(1);
        {
            AmbientMemoryChargingGuard b(&storage);
            chargeAmbient(2);
            {
                AmbientMemoryChargingGuard c(&execution);
                ASSERT_EQ(currentMemoryCharging(), &execution);
                chargeAmbient(4);
            }
            ASSERT_EQ(currentMemoryCharging(), &storage);
            chargeAmbient(2);
        }
        ASSERT_EQ(currentMemoryCharging(), &optimisation);
        chargeAmbient(1);
    }
    ASSERT_EQ(currentMemoryCharging(), nullptr);

    ASSERT_EQ(optimisation.charged, 2);
    ASSERT_EQ(storage.charged, 4);
    ASSERT_EQ(execution.charged, 4);
}

TEST(MemoryChargingTest, SameChargerCanBeInstalledAgainWhileBuried) {
    TestCharging optimisation, storage;
    {
        AmbientMemoryChargingGuard a(&optimisation);
        {
            AmbientMemoryChargingGuard b(&storage);
            {
                AmbientMemoryChargingGuard c(&optimisation);
                ASSERT_EQ(currentMemoryCharging(), &optimisation);
                chargeAmbient(8);
            }
            ASSERT_EQ(currentMemoryCharging(), &storage);
        }
        ASSERT_EQ(currentMemoryCharging(), &optimisation);
    }
    ASSERT_EQ(currentMemoryCharging(), nullptr);
    ASSERT_EQ(optimisation.charged, 8);
    ASSERT_EQ(storage.charged, 0);
}

TEST(MemoryChargingTest, SuspendStopsChargingAndRestoreResumes) {
    TestCharging charging;
    AmbientMemoryChargingGuard guard(&charging);
    chargeAmbient(1);
    {
        AmbientMemoryChargingGuard suspended(nullptr);
        ASSERT_EQ(currentMemoryCharging(), nullptr);
        chargeAmbient(100);
        releaseAmbient(100);
        {
            // A charger installed during a suspension replaces it, and the suspension returns.
            TestCharging inner;
            AmbientMemoryChargingGuard innerGuard(&inner);
            chargeAmbient(5);
            ASSERT_EQ(inner.charged, 5);
        }
        ASSERT_EQ(currentMemoryCharging(), nullptr);
    }
    ASSERT_EQ(currentMemoryCharging(), &charging);
    chargeAmbient(1);

    ASSERT_EQ(charging.charged, 2);
    ASSERT_EQ(charging.released, 0);
}

TEST(MemoryChargingTest, ChargerIsPerThread) {
    TestCharging charging;
    AmbientMemoryChargingGuard guard(&charging);
    MemoryCharging* seenOnOtherThread = &charging;
    stdx::thread([&] { seenOnOtherThread = currentMemoryCharging(); }).join();
    ASSERT_EQ(seenOnOtherThread, nullptr);
}

}  // namespace
}  // namespace mongo
