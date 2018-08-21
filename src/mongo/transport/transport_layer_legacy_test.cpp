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

#include "mongo/platform/basic.h"

#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/transport_layer_legacy.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

class ServiceEntryPointUtil : public ServiceEntryPoint {
public:
    void startSession(transport::SessionHandle session) override {
        Message m;
        Status s = session->sourceMessage(&m).wait();

        ASSERT_NOT_OK(s);

        tll->end(session);

        _sessions.emplace_back(std::move(session));
    }

    DbResponse handleRequest(OperationContext* opCtx, const Message& request) override {
        MONGO_UNREACHABLE;
    }

    // Sessions end as soon as they're started, so this doesn't need to do anything.
    void endAllSessions(transport::Session::TagMask tags) override {
        for (auto& session : _sessions) {
            tll->end(session);
        }
        _sessions.clear();
    }

    Status start() override {
        return Status::OK();
    }

    bool shutdown(Milliseconds timeout) override {
        return true;
    }

    void appendStats(BSONObjBuilder*) const override {}

    size_t numOpenSessions() const override {
        return 0ULL;
    }

    transport::TransportLayerLegacy* tll = nullptr;

    std::list<transport::SessionHandle> _sessions;
};

// This test verifies a fix for SERVER-28239.  The actual service entry point in use by mongod and
// mongos calls end() manually, which occasionally was the second call to end() if after a primary
// stepdown.  This tests verifies our fix (making end() safe to call multiple times)
TEST(TransportLayerLegacy, endSessionsDoesntDoubleClose) {
    // Disabling this test until we can figure out the best way to allocate port numbers for unit
    // tests
    return;
    ServiceEntryPointUtil sepu;

    transport::TransportLayerLegacy::Options opts{};
    opts.port = 27017;
    transport::TransportLayerLegacy tll(opts, &sepu);

    sepu.tll = &tll;

    tll.setup().transitional_ignore();
    tll.start().transitional_ignore();

    stdx::mutex mutex;
    bool end = false;
    stdx::condition_variable cv;

    stdx::thread thr{[&] {
        Socket s;
        SockAddr sa{"localhost", 27017, AF_INET};
        s.connect(sa);

        stdx::unique_lock<stdx::mutex> lk(mutex);
        cv.wait(lk, [&] { return end; });
    }};

    while (Listener::globalTicketHolder.used() == 0) {
    }

    sepu.endAllSessions(transport::Session::TagMask{});

    while (Listener::globalTicketHolder.used() == 1) {
    }

    {
        stdx::lock_guard<stdx::mutex> lk(mutex);
        end = true;
        cv.notify_one();
    }

    thr.join();

    ASSERT(Listener::globalTicketHolder.used() == 0);

    tll.shutdown();
}

}  // namespace
}  // namespace mongo
