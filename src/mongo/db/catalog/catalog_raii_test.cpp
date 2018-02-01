/**
 *    Copyright 2018 MongoDB Inc.
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

#include <string>

#include "mongo/db/catalog/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

class CatalogRAIITestFixture : public unittest::Test {
public:
    typedef std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>
        ClientAndCtx;

    ClientAndCtx makeClientWithLocker(const std::string& clientName) {
        auto client = getGlobalServiceContext()->makeClient(clientName);
        auto opCtx = client->makeOperationContext();
        opCtx->releaseLockState();
        opCtx->setLockState(stdx::make_unique<DefaultLockerImpl>());
        return std::make_pair(std::move(client), std::move(opCtx));
    }

    const NamespaceString nss = NamespaceString("test", "coll");
    const Milliseconds timeoutMs = Milliseconds(30000);
    const Milliseconds maxExecMs = Milliseconds(1500);
    const ClientAndCtx client1 = makeClientWithLocker("client1");
    const ClientAndCtx client2 = makeClientWithLocker("client2");
};

void failsWithLockTimeout(stdx::function<void()> func,
                          Milliseconds timeoutMillis,
                          Milliseconds maxExecMs) {
    Date_t t1 = Date_t::now();
    try {
        func();
        FAIL("Should have gotten an exception due to timeout");
    } catch (const ExceptionFor<ErrorCodes::LockTimeout>&) {
        Date_t t2 = Date_t::now();
        ASSERT_GTE(t2 - t1, timeoutMillis);
        ASSERT_LTE(t2 - t1, timeoutMillis + maxExecMs);
    }
}

TEST_F(CatalogRAIITestFixture, AutoGetDBTimeout) {
    Lock::DBLock dbLock1(client1.second.get(), nss.db(), MODE_X);
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.db(), MODE_X));
    failsWithLockTimeout([&] { AutoGetDb db(client2.second.get(), nss.db(), MODE_X, timeoutMs); },
                         timeoutMs,
                         maxExecMs);
}

TEST_F(CatalogRAIITestFixture, AutoGetDBTimeout0) {
    Lock::DBLock dbLock1(client1.second.get(), nss.db(), MODE_X);
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.db(), MODE_X));
    failsWithLockTimeout(
        [&] { AutoGetDb db(client2.second.get(), nss.db(), MODE_X, Milliseconds(0)); },
        Milliseconds(0),
        maxExecMs);
}

TEST_F(CatalogRAIITestFixture, AutoGetOrCreateDbTimeout) {
    Lock::DBLock dbLock1(client1.second.get(), nss.db(), MODE_X);
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.db(), MODE_X));
    failsWithLockTimeout(
        [&] { AutoGetOrCreateDb db(client2.second.get(), nss.db(), MODE_X, timeoutMs); },
        timeoutMs,
        maxExecMs);
}

TEST_F(CatalogRAIITestFixture, AutoGetCollectionTimeout) {
    Lock::DBLock dbLock1(client1.second.get(), nss.db(), MODE_X);
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.db(), MODE_X));
    Lock::CollectionLock collLock1(client1.second.get()->lockState(), nss.toString(), MODE_X);
    ASSERT(client1.second->lockState()->isCollectionLockedForMode(nss.toString(), MODE_X));
    failsWithLockTimeout(
        [&] {
            AutoGetCollection coll(client2.second.get(),
                                   nss,
                                   MODE_X,
                                   MODE_X,
                                   AutoGetCollection::ViewMode::kViewsForbidden,
                                   timeoutMs);
        },
        timeoutMs,
        maxExecMs);
}

TEST_F(CatalogRAIITestFixture, AutoGetCollectionTimeout0) {
    Lock::DBLock dbLock1(client1.second.get(), nss.db(), MODE_X);
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.db(), MODE_X));
    Lock::CollectionLock collLock1(client1.second.get()->lockState(), nss.toString(), MODE_X);
    ASSERT(client1.second->lockState()->isCollectionLockedForMode(nss.toString(), MODE_X));
    failsWithLockTimeout(
        [&] {
            AutoGetCollection coll(client2.second.get(),
                                   nss,
                                   MODE_X,
                                   MODE_X,
                                   AutoGetCollection::ViewMode::kViewsForbidden,
                                   Milliseconds(0));
        },
        Milliseconds(0),
        maxExecMs);
}
}  // namespace
}  // namespace mongo
