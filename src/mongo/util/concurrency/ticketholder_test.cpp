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


#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/ticketholder.h"

namespace {
using namespace mongo;

TEST(TicketholderTest, BasicTimeout) {
    TicketHolder holder(1);
    ASSERT_EQ(holder.used(), 0);
    ASSERT_EQ(holder.available(), 1);
    ASSERT_EQ(holder.outof(), 1);

    {
        ScopedTicket ticket(&holder);
        ASSERT_EQ(holder.used(), 1);
        ASSERT_EQ(holder.available(), 0);
        ASSERT_EQ(holder.outof(), 1);

        ASSERT_FALSE(holder.tryAcquire());
        ASSERT_FALSE(holder.waitForTicketUntil(Date_t::now()));
        ASSERT_FALSE(holder.waitForTicketUntil(Date_t::now() + Milliseconds(1)));
        ASSERT_FALSE(holder.waitForTicketUntil(Date_t::now() + Milliseconds(42)));
    }

    ASSERT_EQ(holder.used(), 0);
    ASSERT_EQ(holder.available(), 1);
    ASSERT_EQ(holder.outof(), 1);

    ASSERT(holder.waitForTicketUntil(Date_t::now()));
    holder.release();

    ASSERT_EQ(holder.used(), 0);

    ASSERT(holder.waitForTicketUntil(Date_t::now() + Milliseconds(20)));
    ASSERT_EQ(holder.used(), 1);

    ASSERT_FALSE(holder.waitForTicketUntil(Date_t::now() + Milliseconds(2)));
    holder.release();
    ASSERT_EQ(holder.used(), 0);
}
}  // namespace
