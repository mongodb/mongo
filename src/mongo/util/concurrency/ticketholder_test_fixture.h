/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <functional>
#include <memory>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/duration.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo {
/**
 * Since the PriorityTicketHolder is restricted to Linux, and the SemaphoreTicketHolder is not,
 * tests for the two implementations exist in separate files. This fixture allows for a unified
 * location of TicketHolder test helpers and test cases whose behaviors hold regardless of
 * TicketHolder implementation.
 *
 * TODO SERVER-72616: Consider combining semaphore_ticketholder_test.cpp and
 * priority_ticketholder_test.cpp into a single file. Especially since there are few tests that
 * target the legacy SemaphoreTicketHolder.
 */
class TicketHolderTestFixture : public ServiceContextTest {
public:
    void setUp() override;

protected:
    class Stats;
    struct MockAdmission;

    void basicTimeout(OperationContext* opCtx, std::unique_ptr<TicketHolder> holder);

    /**
     * Tests that TicketHolder::resize() does not impact metrics outside of those related to the
     * number of tickets available(), used(), and outof().
     */
    void resizeTest(OperationContext* opCtx,
                    std::unique_ptr<TicketHolder> holder,
                    TickSourceMock<Microseconds>* tickSource);

    /**
     * Tests that ticket acquisition is interruptible.
     */
    void interruptTest(OperationContext* opCtx, std::unique_ptr<TicketHolder> holder);

    /**
     * Tests that the ticket is released with the same priority with which it was acquired.
     */
    void priorityBookkeepingTest(OperationContext* opCtx,
                                 std::unique_ptr<TicketHolder> holder,
                                 AdmissionContext::Priority newPriority,
                                 std::function<void(BSONObj& /*statsWhileProcessing*/,
                                                    BSONObj& /*statsWhenFinished*/)> checks);

    ServiceContext::UniqueClient _client;
    ServiceContext::UniqueOperationContext _opCtx;
};

/**
 * Provides easy access to instantaneous statistics of a given 'TicketHolder'.
 */
class TicketHolderTestFixture::Stats {
public:
    Stats(TicketHolder* holder) : _holder(holder){};

    long long operator[](StringData field) const {
        BSONObjBuilder bob;
        _holder->appendStats(bob);
        auto stats = bob.obj();
        return stats[field].numberLong();
    }

    BSONObj getStats() const {
        BSONObjBuilder bob;
        _holder->appendStats(bob);
        return bob.obj();
    }

    BSONObj getNonTicketStats() const {
        return getStats().removeField("out").removeField("available").removeField("totalTickets");
    }

private:
    TicketHolder* _holder;
};

/**
 * Constructs the context necessary to submit a for-test admission to a TicketHolder.
 */
struct TicketHolderTestFixture::MockAdmission {
    MockAdmission(ServiceContext* serviceContext, AdmissionContext::Priority priority) {
        client = serviceContext->getService()->makeClient("");
        opCtx = client->makeOperationContext();
        admissionPriority.emplace(opCtx.get(), priority);
    }

    ServiceContext::UniqueClient client;
    ServiceContext::UniqueOperationContext opCtx;
    boost::optional<ScopedAdmissionPriority> admissionPriority;
    boost::optional<Ticket> ticket;
};

}  // namespace mongo
