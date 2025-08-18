/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/read_write_concern_defaults.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"
#include "mongo/db/vector_clock/vector_clock_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/duration.h"

#include <cstdint>
#include <memory>
#include <string>
#include <variant>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

class ReadWriteConcernDefaultsTest : public ServiceContextTest {
protected:
    void createDefaults(bool isImplicitWCMajority) {
        ReadWriteConcernDefaults::create(getService(), _lookupMock.getFetchDefaultsFn());
        auto& rwcd = ReadWriteConcernDefaults::get(getService());
        rwcd.setImplicitDefaultWriteConcernMajority(isImplicitWCMajority);
    }

    ReadWriteConcernDefaults::RWConcernDefaultAndTime getDefault() {
        return ReadWriteConcernDefaults::get(getService()).getDefault(_opCtx);
    }

    bool isCWWCSet() {
        return ReadWriteConcernDefaults::get(getService()).isCWWCSet(_opCtx);
    }

    ReadWriteConcernDefaultsLookupMock _lookupMock;

    ServiceContext::UniqueOperationContext _opCtxHolder{makeOperationContext()};
    OperationContext* const _opCtx{_opCtxHolder.get()};
};

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWithAbsentCWRWCWithImplicitWCW1) {
    createDefaults(false /* isImplicitWCMajority */);

    // By not calling _lookupMock.setLookupCallReturnValue(), tests _defaults.lookup() returning
    // boost::none.
    auto defaults = getDefault();

    ASSERT(defaults.getDefaultReadConcern());
    ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT(defaults.getDefaultReadConcernSource());
    ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kImplicit);

    ASSERT(!defaults.getDefaultWriteConcern());
    ASSERT(!isCWWCSet());
    ASSERT(!defaults.getUpdateOpTime());
    ASSERT(!defaults.getUpdateWallClockTime());
    ASSERT_EQ(Date_t(), defaults.localUpdateWallClockTime());
    ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);
}

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWithAbsentCWRWCWithImplicitWCMajority) {
    createDefaults(true /* isImplicitWCMajority */);

    // By not calling _lookupMock.setLookupCallReturnValue(), tests _defaults.lookup() returning
    // boost::none.
    ASSERT(!isCWWCSet());
    auto defaults = getDefault();

    ASSERT(defaults.getDefaultReadConcern());
    ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT(defaults.getDefaultReadConcernSource());
    ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kImplicit);

    ASSERT(defaults.getDefaultWriteConcern());
    ASSERT(holds_alternative<std::string>(defaults.getDefaultWriteConcern()->w));
    ASSERT_EQ(WriteConcernOptions::kMajority,
              get<std::string>(defaults.getDefaultWriteConcern()->w));
    ASSERT(!defaults.getUpdateOpTime());
    ASSERT(!defaults.getUpdateWallClockTime());
    ASSERT_EQ(Date_t(), defaults.localUpdateWallClockTime());
    ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);
}

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWithCWRWCNeverSetWithImplicitWCW1) {
    createDefaults(false /* isImplicitWCMajority */);

    // _defaults.lookup() returning default constructed RWConcern not boost::none.
    _lookupMock.setLookupCallReturnValue({});

    ASSERT(!isCWWCSet());
    auto defaults = getDefault();

    ASSERT(defaults.getDefaultReadConcern());
    ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT(defaults.getDefaultReadConcernSource());
    ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kImplicit);

    ASSERT(!defaults.getDefaultWriteConcern());
    ASSERT(!defaults.getUpdateOpTime());
    ASSERT(!defaults.getUpdateWallClockTime());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());
    ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);
}

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWithCWRWCNeverSetWithImplicitWCMajority) {
    createDefaults(true /* isImplicitWCMajority */);

    // _defaults.lookup() returning default constructed RWConcern not boost::none.
    ASSERT(!isCWWCSet());
    _lookupMock.setLookupCallReturnValue({});
    auto defaults = getDefault();

    ASSERT(defaults.getDefaultReadConcern());
    ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT(defaults.getDefaultReadConcernSource());
    ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kImplicit);

    ASSERT(defaults.getDefaultWriteConcern());
    ASSERT(holds_alternative<std::string>(defaults.getDefaultWriteConcern()->w));
    ASSERT_EQ(WriteConcernOptions::kMajority,
              get<std::string>(defaults.getDefaultWriteConcern()->w));
    ASSERT(!defaults.getUpdateOpTime());
    ASSERT(!defaults.getUpdateWallClockTime());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());
    ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);
}

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWithUnsetCWRWCWithImplicitWCW1) {
    createDefaults(false /* isImplicitWCMajority */);

    RWConcernDefault newDefaults;
    newDefaults.setUpdateOpTime(Timestamp(1, 2));
    newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    ASSERT(!isCWWCSet());
    auto defaults = getDefault();

    ASSERT(defaults.getDefaultReadConcern());
    ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT(defaults.getDefaultReadConcernSource());
    ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kImplicit);

    ASSERT(!defaults.getDefaultWriteConcern());
    ASSERT_EQ(Timestamp(1, 2), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());
    ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);
}

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWithUnsetCWRWCWithImplicitWCMajority) {
    createDefaults(true /* isImplicitWCMajority */);

    RWConcernDefault newDefaults;
    newDefaults.setUpdateOpTime(Timestamp(1, 2));
    newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    ASSERT(!isCWWCSet());
    auto defaults = getDefault();

    ASSERT(defaults.getDefaultReadConcern());
    ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT(defaults.getDefaultReadConcernSource());
    ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kImplicit);

    ASSERT(defaults.getDefaultWriteConcern());
    ASSERT(holds_alternative<std::string>(defaults.getDefaultWriteConcern()->w));
    ASSERT_EQ(WriteConcernOptions::kMajority,
              get<std::string>(defaults.getDefaultWriteConcern()->w));
    ASSERT_EQ(Timestamp(1, 2), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());
    ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);
}

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWithCWRWCNotSetThenSetWithImplicitWCW1) {
    createDefaults(false /* isImplicitWCMajority */);

    // _defaults.lookup() returning default constructed RWConcern not boost::none.
    _lookupMock.setLookupCallReturnValue({});
    ASSERT(!isCWWCSet());
    auto oldDefaults = getDefault();

    ASSERT(oldDefaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT(oldDefaults.getDefaultReadConcernSource());
    ASSERT(oldDefaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kImplicit);

    ASSERT(!oldDefaults.getDefaultWriteConcern());
    ASSERT(!oldDefaults.getUpdateOpTime());
    ASSERT(!oldDefaults.getUpdateWallClockTime());
    ASSERT_GT(oldDefaults.localUpdateWallClockTime(), Date_t());
    ASSERT(oldDefaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);

    RWConcernDefault newDefaults;
    newDefaults.setDefaultReadConcern(
        repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern));
    WriteConcernOptions wc(
        4, WriteConcernOptions::SyncMode::UNSET, WriteConcernOptions::kNoTimeout);
    newDefaults.setDefaultWriteConcern(wc);
    newDefaults.setUpdateOpTime(Timestamp(1, 2));
    newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    ReadWriteConcernDefaults::get(getService()).invalidate();
    ASSERT(isCWWCSet());
    auto defaults = getDefault();
    ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT(defaults.getDefaultReadConcernSource());
    ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kGlobal);

    ASSERT(holds_alternative<int64_t>(defaults.getDefaultWriteConcern()->w));
    ASSERT_EQ(4, get<int64_t>(defaults.getDefaultWriteConcern()->w));
    ASSERT_EQ(Timestamp(1, 2), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());
    ASSERT(defaults.getDefaultWriteConcernSource());
    ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kGlobal);
}

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWithCWRWCNotSetThenSetWithImplicitWCMajority) {
    createDefaults(true /* isImplicitWCMajority */);

    // _defaults.lookup() returning default constructed RWConcern not boost::none.
    _lookupMock.setLookupCallReturnValue({});
    ASSERT(!isCWWCSet());
    auto oldDefaults = getDefault();
    ASSERT(oldDefaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT(oldDefaults.getDefaultReadConcernSource());
    ASSERT(oldDefaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kImplicit);

    ASSERT(oldDefaults.getDefaultWriteConcern());
    ASSERT(holds_alternative<std::string>(oldDefaults.getDefaultWriteConcern()->w));
    ASSERT_EQ(WriteConcernOptions::kMajority,
              get<std::string>(oldDefaults.getDefaultWriteConcern()->w));
    ASSERT(!oldDefaults.getUpdateOpTime());
    ASSERT(!oldDefaults.getUpdateWallClockTime());
    ASSERT_GT(oldDefaults.localUpdateWallClockTime(), Date_t());
    ASSERT(oldDefaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);

    RWConcernDefault newDefaults;
    newDefaults.setDefaultReadConcern(
        repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern));
    WriteConcernOptions wc(
        4, WriteConcernOptions::SyncMode::UNSET, WriteConcernOptions::kNoTimeout);
    newDefaults.setDefaultWriteConcern(wc);
    newDefaults.setUpdateOpTime(Timestamp(1, 2));
    newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    ReadWriteConcernDefaults::get(getService()).invalidate();
    ASSERT(isCWWCSet());
    auto defaults = getDefault();
    ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT(defaults.getDefaultReadConcernSource());
    ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kGlobal);

    ASSERT(holds_alternative<int64_t>(defaults.getDefaultWriteConcern()->w));
    ASSERT_EQ(4, get<int64_t>(defaults.getDefaultWriteConcern()->w));
    ASSERT_EQ(Timestamp(1, 2), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());
    ASSERT(defaults.getDefaultWriteConcernSource());
    ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kGlobal);
}

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWithSetCWRWCWithImplicitWCW1) {
    createDefaults(false /* isImplicitWCMajority */);

    RWConcernDefault newDefaults;
    newDefaults.setDefaultReadConcern(
        repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern));
    WriteConcernOptions wc(
        4, WriteConcernOptions::SyncMode::UNSET, WriteConcernOptions::kNoTimeout);
    newDefaults.setDefaultWriteConcern(wc);
    newDefaults.setUpdateOpTime(Timestamp(1, 2));
    newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));
    ASSERT(isCWWCSet());
    auto defaults = getDefault();
    ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT(defaults.getDefaultReadConcernSource());
    ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kGlobal);

    ASSERT(holds_alternative<int64_t>(defaults.getDefaultWriteConcern()->w));
    ASSERT_EQ(4, get<int64_t>(defaults.getDefaultWriteConcern()->w));
    ASSERT_EQ(Timestamp(1, 2), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());
    ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kGlobal);
}

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWithSetCWRWCWithImplicitWCMajority) {
    createDefaults(true /* isImplicitWCMajority */);

    RWConcernDefault newDefaults;
    newDefaults.setDefaultReadConcern(
        repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern));
    WriteConcernOptions wc(
        4, WriteConcernOptions::SyncMode::UNSET, WriteConcernOptions::kNoTimeout);
    newDefaults.setDefaultWriteConcern(wc);
    newDefaults.setUpdateOpTime(Timestamp(1, 2));
    newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    ASSERT(isCWWCSet());
    auto defaults = getDefault();
    ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT(defaults.getDefaultReadConcernSource());
    ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kGlobal);

    ASSERT(holds_alternative<int64_t>(defaults.getDefaultWriteConcern()->w));
    ASSERT_EQ(4, get<int64_t>(defaults.getDefaultWriteConcern()->w));
    ASSERT_EQ(Timestamp(1, 2), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());
    ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kGlobal);
}

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWriteConcernSourceImplicitWithUsedDefaultWC) {
    createDefaults(true /* isImplicitWCMajority */);

    RWConcernDefault newDefaults;
    newDefaults.setDefaultWriteConcern(WriteConcernOptions());
    newDefaults.setUpdateOpTime(Timestamp(1, 2));
    newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));

    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    ASSERT(!isCWWCSet());
    auto defaults = getDefault();

    ASSERT(defaults.getDefaultReadConcern());
    ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT(defaults.getDefaultReadConcernSource());
    ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kImplicit);

    // The default write concern source should be set to implicit if wc.usedDefaultConstructedWC is
    // true
    ASSERT(holds_alternative<std::string>(defaults.getDefaultWriteConcern()->w));
    ASSERT_EQ(WriteConcernOptions::kMajority,
              get<std::string>(defaults.getDefaultWriteConcern()->w));
    ASSERT_EQ(Timestamp(1, 2), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());
    ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);
}

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWithSetAndUnSetCWRCWithImplicitWC1) {
    createDefaults(false /* isImplicitWCMajority */);

    // Setting only default read concern.
    RWConcernDefault newDefaults;
    newDefaults.setDefaultReadConcern(
        repl::ReadConcernArgs(repl::ReadConcernLevel::kAvailableReadConcern));
    newDefaults.setUpdateOpTime(Timestamp(1, 2));
    newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    ASSERT(!isCWWCSet());
    auto defaults = getDefault();
    ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kAvailableReadConcern);
    ASSERT(defaults.getDefaultReadConcernSource());
    ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kGlobal);

    ASSERT(!defaults.getDefaultWriteConcern());
    ASSERT_EQ(Timestamp(1, 2), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());
    ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);

    // unsetting default read concern.
    RWConcernDefault newDefaults2;
    newDefaults2.setUpdateOpTime(Timestamp(1, 3));
    newDefaults2.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults2));

    ReadWriteConcernDefaults::get(getService()).refreshIfNecessary(_opCtx);

    ASSERT(!isCWWCSet());
    defaults = getDefault();

    ASSERT(defaults.getDefaultReadConcern());
    ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT(defaults.getDefaultReadConcernSource());
    ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kImplicit);

    ASSERT(!defaults.getDefaultWriteConcern());
    ASSERT_EQ(Timestamp(1, 3), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());
    ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);
}

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWithSetAndUnSetCWRCWithImplicitWCMajority) {
    createDefaults(true /* isImplicitWCMajority */);

    // Setting only default read concern.
    RWConcernDefault newDefaults;
    newDefaults.setDefaultReadConcern(
        repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern));
    newDefaults.setUpdateOpTime(Timestamp(1, 2));
    newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    ASSERT(!isCWWCSet());
    auto defaults = getDefault();
    ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT(defaults.getDefaultReadConcernSource());
    ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kGlobal);

    ASSERT(defaults.getDefaultWriteConcern());
    ASSERT(holds_alternative<std::string>(defaults.getDefaultWriteConcern()->w));
    ASSERT_EQ(WriteConcernOptions::kMajority,
              get<std::string>(defaults.getDefaultWriteConcern()->w));
    ASSERT_EQ(Timestamp(1, 2), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());
    ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);

    // Unsetting default read concern.
    RWConcernDefault newDefaults2;
    newDefaults2.setUpdateOpTime(Timestamp(1, 3));
    newDefaults2.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults2));

    ReadWriteConcernDefaults::get(getService()).refreshIfNecessary(_opCtx);

    ASSERT(!isCWWCSet());
    defaults = getDefault();

    ASSERT(defaults.getDefaultReadConcern());
    ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT(defaults.getDefaultReadConcernSource());
    ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kImplicit);

    ASSERT(holds_alternative<std::string>(defaults.getDefaultWriteConcern()->w));
    ASSERT_EQ(WriteConcernOptions::kMajority,
              get<std::string>(defaults.getDefaultWriteConcern()->w));
    ASSERT_EQ(Timestamp(1, 3), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());
    ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);
}


TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultLookupFailure) {
    createDefaults(true /* isImplicitWCMajority */);
    _lookupMock.setLookupCallFailure(Status{ErrorCodes::Error(1234), "foobar"});
    ASSERT_THROWS_CODE_AND_WHAT(getDefault(), AssertionException, 1234, "foobar");
    ASSERT_THROWS_CODE_AND_WHAT(isCWWCSet(), AssertionException, 1234, "foobar");
}

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWithoutInvalidateDoesNotCallLookup) {
    createDefaults(false /* isImplicitWCMajority */);

    RWConcernDefault newDefaults;
    newDefaults.setUpdateOpTime(Timestamp(1, 2));
    newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    ASSERT(!isCWWCSet());
    auto defaults = getDefault();

    ASSERT(defaults.getDefaultReadConcern());
    ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT(defaults.getDefaultReadConcernSource());
    ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kImplicit);

    ASSERT(!defaults.getDefaultWriteConcern());
    ASSERT_EQ(Timestamp(1, 2), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());
    ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);

    RWConcernDefault newDefaults2;
    newDefaults2.setDefaultReadConcern(
        repl::ReadConcernArgs(repl::ReadConcernLevel::kAvailableReadConcern));
    WriteConcernOptions wc(
        4, WriteConcernOptions::SyncMode::UNSET, WriteConcernOptions::kNoTimeout);
    newDefaults2.setDefaultWriteConcern(wc);
    newDefaults2.setUpdateOpTime(Timestamp(3, 4));
    newDefaults2.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(5678));
    newDefaults2.setDefaultWriteConcernSource(DefaultWriteConcernSourceEnum::kGlobal);
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults2));

    ASSERT(!isCWWCSet());
    auto defaults2 = getDefault();

    ASSERT(defaults2.getDefaultReadConcern());
    ASSERT(defaults2.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT(defaults2.getDefaultReadConcernSource());
    ASSERT(defaults2.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kImplicit);

    ASSERT(!defaults2.getDefaultWriteConcern());
    ASSERT_EQ(Timestamp(1, 2), *defaults2.getUpdateOpTime());
    ASSERT_EQ(1234, defaults2.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults2.localUpdateWallClockTime(), Date_t());
    ASSERT(defaults2.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);
}

TEST_F(ReadWriteConcernDefaultsTest, TestInvalidate) {
    createDefaults(false /* isImplicitWCMajority */);

    RWConcernDefault newDefaults;
    newDefaults.setUpdateOpTime(Timestamp(1, 2));
    newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    ASSERT(!isCWWCSet());
    auto defaults = getDefault();

    ASSERT(defaults.getDefaultReadConcern());
    ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT(defaults.getDefaultReadConcernSource());
    ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kImplicit);

    ASSERT(!defaults.getDefaultWriteConcern());
    ASSERT_EQ(Timestamp(1, 2), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());
    ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);

    RWConcernDefault newDefaults2;
    newDefaults2.setDefaultReadConcern(
        repl::ReadConcernArgs(repl::ReadConcernLevel::kAvailableReadConcern));
    WriteConcernOptions wc(
        4, WriteConcernOptions::SyncMode::UNSET, WriteConcernOptions::kNoTimeout);
    newDefaults2.setDefaultWriteConcern(wc);
    newDefaults2.setUpdateOpTime(Timestamp(3, 4));
    newDefaults2.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(5678));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults2));

    ReadWriteConcernDefaults::get(getService()).invalidate();
    ASSERT(isCWWCSet());
    auto defaults2 = getDefault();
    ASSERT(defaults2.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kAvailableReadConcern);
    ASSERT(defaults2.getDefaultReadConcernSource());
    ASSERT(defaults2.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kGlobal);

    ASSERT(holds_alternative<int64_t>(defaults2.getDefaultWriteConcern()->w));
    ASSERT_EQ(4, get<int64_t>(defaults2.getDefaultWriteConcern()->w));
    ASSERT_EQ(Timestamp(3, 4), *defaults2.getUpdateOpTime());
    ASSERT_EQ(5678, defaults2.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults2.localUpdateWallClockTime(), Date_t());
    ASSERT(defaults2.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kGlobal);
}

TEST_F(ReadWriteConcernDefaultsTest, TestRefreshDefaultsWithEmptyCacheAndAbsentDefaults) {
    createDefaults(false /* isImplicitWCMajority */);
    ReadWriteConcernDefaults::get(getService()).refreshIfNecessary(_opCtx);

    ASSERT(!isCWWCSet());
    auto defaults = getDefault();

    ASSERT(defaults.getDefaultReadConcern());
    ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT(defaults.getDefaultReadConcernSource());
    ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kImplicit);

    ASSERT(!defaults.getDefaultWriteConcern());
    ASSERT(!defaults.getUpdateOpTime());
    ASSERT(!defaults.getUpdateWallClockTime());
    ASSERT_EQ(Date_t(), defaults.localUpdateWallClockTime());
    ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);
}

TEST_F(ReadWriteConcernDefaultsTest, TestRefreshDefaultsWithEmptyCacheAndSetDefaults) {
    createDefaults(false /* isImplicitWCMajority */);

    RWConcernDefault newDefaults;
    newDefaults.setUpdateOpTime(Timestamp(1, 2));
    newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    ReadWriteConcernDefaults::get(getService()).refreshIfNecessary(_opCtx);

    ASSERT(!isCWWCSet());
    auto defaults = getDefault();
    ASSERT_EQ(Timestamp(1, 2), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());
    ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);
}

TEST_F(ReadWriteConcernDefaultsTest, TestRefreshDefaultsWithHigherEpoch) {
    createDefaults(false /* isImplicitWCMajority */);

    RWConcernDefault newDefaults;
    newDefaults.setUpdateOpTime(Timestamp(1, 2));
    newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    ASSERT(!isCWWCSet());
    auto defaults = getDefault();
    ASSERT_EQ(Timestamp(1, 2), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);

    RWConcernDefault newDefaults2;
    newDefaults2.setUpdateOpTime(Timestamp(3, 4));
    newDefaults2.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(5678));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults2));

    ReadWriteConcernDefaults::get(getService()).refreshIfNecessary(_opCtx);

    ASSERT(!isCWWCSet());
    auto defaults2 = getDefault();
    ASSERT_EQ(Timestamp(3, 4), *defaults2.getUpdateOpTime());
    ASSERT_EQ(5678, defaults2.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);
}

TEST_F(ReadWriteConcernDefaultsTest, TestRefreshDefaultsWithLowerEpoch) {
    createDefaults(false /* isImplicitWCMajority */);

    RWConcernDefault newDefaults;
    newDefaults.setUpdateOpTime(Timestamp(10, 20));
    newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    auto defaults = getDefault();
    ASSERT(defaults.getUpdateOpTime());
    ASSERT(defaults.getUpdateWallClockTime());
    ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);

    RWConcernDefault newDefaults2;
    newDefaults2.setUpdateOpTime(Timestamp(5, 6));
    newDefaults2.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(5678));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults2));

    ReadWriteConcernDefaults::get(getService()).refreshIfNecessary(_opCtx);

    auto defaults2 = getDefault();
    ASSERT_EQ(Timestamp(10, 20), *defaults2.getUpdateOpTime());
    ASSERT_EQ(1234, defaults2.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);
}

TEST_F(ReadWriteConcernDefaultsTest, TestRefreshDefaultsWithHigherEpochNoRWCChangeNoMessage) {
    createDefaults(false /* isImplicitWCMajority */);

    RWConcernDefault newDefaults;
    newDefaults.setUpdateOpTime(Timestamp(1, 2));
    newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    ASSERT(!isCWWCSet());
    auto defaults = getDefault();
    ASSERT_EQ(Timestamp(1, 2), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);

    RWConcernDefault newDefaults2;
    newDefaults2.setUpdateOpTime(Timestamp(3, 4));
    newDefaults2.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(5678));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults2));

    unittest::LogCaptureGuard logs;
    ReadWriteConcernDefaults::get(getService()).refreshIfNecessary(_opCtx);

    ASSERT(!isCWWCSet());
    auto defaults2 = getDefault();
    ASSERT_EQ(Timestamp(3, 4), *defaults2.getUpdateOpTime());
    ASSERT_EQ(5678, defaults2.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);

    logs.stop();
    ASSERT_EQUALS(0, logs.countTextContaining("Refreshed RWC defaults"));
}

/**
 * ReadWriteConcernDefaults::generateNewCWRWCToBeSavedOnDisk() uses the current clusterTime and wall
 * clock time (for epoch and setTime/localSetTime), so testing it requires a fixture with a logical
 * clock.
 */
class ReadWriteConcernDefaultsTestWithClusterTime : public VectorClockTestFixture {
public:
    ~ReadWriteConcernDefaultsTestWithClusterTime() override {
        ReadWriteConcernDefaults::get(getService()).invalidate();
    }

protected:
    auto setupOldDefaults() {
        auto defaults = _rwcd.generateNewCWRWCToBeSavedOnDisk(
            operationContext(),
            repl::ReadConcernArgs(repl::ReadConcernLevel::kAvailableReadConcern),
            uassertStatusOK(WriteConcernOptions::parse(BSON("w" << 4))));

        ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
               repl::ReadConcernLevel::kAvailableReadConcern);
        // default read concern source is not saved on disk.
        ASSERT(!defaults.getDefaultReadConcernSource());

        ASSERT(holds_alternative<int64_t>(defaults.getDefaultWriteConcern()->w));
        ASSERT_EQ(4, get<int64_t>(defaults.getDefaultWriteConcern()->w));
        ASSERT(defaults.getUpdateOpTime());
        ASSERT(defaults.getUpdateWallClockTime());
        // Default write concern source is not saved on disk.
        ASSERT(!defaults.getDefaultWriteConcernSource());

        _lookupMock.setLookupCallReturnValue(std::move(defaults));
        auto oldDefaults = _rwcd.getDefault(operationContext());

        VectorClockMutable::get(getServiceContext())->tickClusterTime(1);
        getMockClockSource()->advance(Milliseconds(1));

        return oldDefaults;
    }

    ReadWriteConcernDefaultsLookupMock _lookupMock;

    ReadWriteConcernDefaults& _rwcd{[&]() -> ReadWriteConcernDefaults& {
        ReadWriteConcernDefaults::create(getService(), _lookupMock.getFetchDefaultsFn());
        return ReadWriteConcernDefaults::get(getService());
    }()};
};

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime,
       TestGenerateNewCWRWCToBeSavedOnDiskInvalidNeither) {
    ASSERT_THROWS_CODE(
        _rwcd.generateNewCWRWCToBeSavedOnDisk(operationContext(), boost::none, boost::none),
        AssertionException,
        ErrorCodes::BadValue);
}

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime,
       TestGenerateNewCWRWCToBeSavedOnDiskInvalidReadConcernLevel) {
    ASSERT_THROWS_CODE(_rwcd.generateNewCWRWCToBeSavedOnDisk(
                           operationContext(),
                           repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern),
                           boost::none),
                       AssertionException,
                       ErrorCodes::BadValue);
    ASSERT_THROWS_CODE(_rwcd.generateNewCWRWCToBeSavedOnDisk(
                           operationContext(),
                           repl::ReadConcernArgs(repl::ReadConcernLevel::kLinearizableReadConcern),
                           boost::none),
                       AssertionException,
                       ErrorCodes::BadValue);
}

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime,
       TestGenerateNewCWRWCToBeSavedOnDiskInvalidReadConcernFields) {
    ASSERT_THROWS_CODE(
        _rwcd.generateNewCWRWCToBeSavedOnDisk(
            operationContext(),
            repl::ReadConcernArgs::fromBSONThrows(BSON("level" << "local"
                                                               << "afterOpTime" << repl::OpTime())),
            boost::none),
        AssertionException,
        ErrorCodes::BadValue);
    ASSERT_THROWS_CODE(_rwcd.generateNewCWRWCToBeSavedOnDisk(
                           operationContext(),
                           repl::ReadConcernArgs::fromBSONThrows(BSON("level" << "local"
                                                                              << "afterClusterTime"
                                                                              << Timestamp(1, 2))),
                           boost::none),
                       AssertionException,
                       ErrorCodes::BadValue);
    ASSERT_THROWS_CODE(_rwcd.generateNewCWRWCToBeSavedOnDisk(
                           operationContext(),
                           repl::ReadConcernArgs::fromBSONThrows(BSON("level" << "snapshot"
                                                                              << "atClusterTime"
                                                                              << Timestamp(1, 2))),
                           boost::none),
                       AssertionException,
                       ErrorCodes::BadValue);
}

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime,
       TestGenerateNewCWRWCToBeSavedOnDiskInvalidWriteConcern) {
    ASSERT_THROWS_CODE(_rwcd.generateNewCWRWCToBeSavedOnDisk(
                           operationContext(),
                           boost::none,
                           uassertStatusOK(WriteConcernOptions::parse(BSON("w" << 0)))),
                       AssertionException,
                       ErrorCodes::BadValue);
}

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime,
       TestGenerateNewCWRWCToBeSavedOnDiskValidSetReadConcernAndWriteConcerns) {
    auto oldDefaults = setupOldDefaults();
    auto defaults = _rwcd.generateNewCWRWCToBeSavedOnDisk(
        operationContext(),
        repl::ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern),
        uassertStatusOK(WriteConcernOptions::parse(BSON("w" << 5))));
    ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kMajorityReadConcern);
    ASSERT(!defaults.getDefaultReadConcernSource());

    ASSERT(holds_alternative<int64_t>(defaults.getDefaultWriteConcern()->w));
    ASSERT_EQ(5, get<int64_t>(defaults.getDefaultWriteConcern()->w));
    ASSERT_LT(*oldDefaults.getUpdateOpTime(), *defaults.getUpdateOpTime());
    ASSERT_LT(*oldDefaults.getUpdateWallClockTime(), *defaults.getUpdateWallClockTime());
    // Default write concern source is not saved on disk.
    ASSERT(!defaults.getDefaultWriteConcernSource());

    _lookupMock.setLookupCallReturnValue(std::move(defaults));
    _rwcd.refreshIfNecessary(operationContext());
    auto newDefaults = _rwcd.getDefault(operationContext());
    ASSERT_LT(oldDefaults.localUpdateWallClockTime(), newDefaults.localUpdateWallClockTime());
    // Default write concern source is calculated through 'getDefault'.
    ASSERT(newDefaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kGlobal);
}

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime,
       TestGenerateNewCWRWCToBeSavedOnDiskValidSetReadConcernOnly) {
    auto oldDefaults = setupOldDefaults();
    auto defaults = _rwcd.generateNewCWRWCToBeSavedOnDisk(
        operationContext(),
        repl::ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern),
        boost::none);
    ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kMajorityReadConcern);
    ASSERT(!defaults.getDefaultReadConcernSource());

    ASSERT(oldDefaults.getDefaultWriteConcern()->w == defaults.getDefaultWriteConcern()->w);
    ASSERT_LT(*oldDefaults.getUpdateOpTime(), *defaults.getUpdateOpTime());
    ASSERT_LT(*oldDefaults.getUpdateWallClockTime(), *defaults.getUpdateWallClockTime());
    // Default write concern source is not saved on disk.
    ASSERT(!defaults.getDefaultWriteConcernSource());

    _lookupMock.setLookupCallReturnValue(std::move(defaults));
    _rwcd.refreshIfNecessary(operationContext());
    auto newDefaults = _rwcd.getDefault(operationContext());
    ASSERT_LT(oldDefaults.localUpdateWallClockTime(), newDefaults.localUpdateWallClockTime());
    // Default write concern source is calculated through 'getDefault'.
    ASSERT(newDefaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kGlobal);
}

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime,
       TestGenerateNewCWRWCToBeSavedOnDiskSetReadConcernWithNoOldDefaults) {
    auto defaults = _rwcd.generateNewCWRWCToBeSavedOnDisk(
        operationContext(),
        repl::ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern),
        boost::none);
    ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kMajorityReadConcern);
    ASSERT(!defaults.getDefaultWriteConcern());
    // Default write concern source is not saved on disk.
    ASSERT(!defaults.getDefaultWriteConcernSource());

    _lookupMock.setLookupCallReturnValue(std::move(defaults));
    _rwcd.refreshIfNecessary(operationContext());
    auto newDefaults = _rwcd.getDefault(operationContext());
    // Default write concern source is calculated through 'getDefault'.
    ASSERT(newDefaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);
}

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime,
       TestGenerateNewCWRWCToBeSavedOnDiskValidSetWriteConcernOnly) {
    auto oldDefaults = setupOldDefaults();
    auto defaults = _rwcd.generateNewCWRWCToBeSavedOnDisk(
        operationContext(),
        boost::none,
        uassertStatusOK(WriteConcernOptions::parse(BSON("w" << 5))));
    ASSERT(oldDefaults.getDefaultReadConcern()->getLevel() ==
           defaults.getDefaultReadConcern()->getLevel());
    ASSERT(!defaults.getDefaultReadConcernSource());

    ASSERT(holds_alternative<int64_t>(defaults.getDefaultWriteConcern()->w));
    ASSERT_EQ(5, get<int64_t>(defaults.getDefaultWriteConcern()->w));
    ASSERT_LT(*oldDefaults.getUpdateOpTime(), *defaults.getUpdateOpTime());
    ASSERT_LT(*oldDefaults.getUpdateWallClockTime(), *defaults.getUpdateWallClockTime());
    // Default write concern source is not saved on disk.
    ASSERT(!defaults.getDefaultWriteConcernSource());

    _lookupMock.setLookupCallReturnValue(std::move(defaults));
    _rwcd.refreshIfNecessary(operationContext());
    auto newDefaults = _rwcd.getDefault(operationContext());
    ASSERT_LT(oldDefaults.localUpdateWallClockTime(), newDefaults.localUpdateWallClockTime());
    // Default write concern source is calculated through 'getDefault'.
    ASSERT(newDefaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kGlobal);
}

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime,
       TestGenerateNewCWRWCToBeSavedOnDiskSetReadThenWriteConcern) {
    auto oldDefaults = _rwcd.generateNewCWRWCToBeSavedOnDisk(
        operationContext(),
        repl::ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern),
        boost::none);
    ASSERT(oldDefaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kMajorityReadConcern);
    ASSERT(!oldDefaults.getDefaultWriteConcern());
    // Default write concern source is not saved on disk.
    ASSERT(!oldDefaults.getDefaultWriteConcernSource());

    _lookupMock.setLookupCallReturnValue(std::move(oldDefaults));
    _rwcd.refreshIfNecessary(operationContext());
    auto newDefaults = _rwcd.getDefault(operationContext());
    // Default write concern source is calculated through 'getDefault'.
    ASSERT(newDefaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);

    VectorClockMutable::get(getServiceContext())->tickClusterTime(1);
    getMockClockSource()->advance(Milliseconds(1));

    auto defaults = _rwcd.generateNewCWRWCToBeSavedOnDisk(
        operationContext(),
        boost::none,
        uassertStatusOK(WriteConcernOptions::parse(BSON("w" << 5))));
    ASSERT(newDefaults.getDefaultReadConcern()->getLevel() ==
           defaults.getDefaultReadConcern()->getLevel());
    ASSERT(holds_alternative<int64_t>(defaults.getDefaultWriteConcern()->w));
    ASSERT_EQ(5, get<int64_t>(defaults.getDefaultWriteConcern()->w));
    // Default write concern source is not saved on disk.
    ASSERT(!defaults.getDefaultWriteConcernSource());

    _lookupMock.setLookupCallReturnValue(std::move(defaults));
    _rwcd.refreshIfNecessary(operationContext());
    newDefaults = _rwcd.getDefault(operationContext());
    ASSERT(newDefaults.getDefaultWriteConcern());
    ASSERT(holds_alternative<int64_t>(newDefaults.getDefaultWriteConcern()->w));
    ASSERT_EQ(5, get<int64_t>(newDefaults.getDefaultWriteConcern()->w));
    // Default write concern source is calculated through 'getDefault'.
    ASSERT(newDefaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kGlobal);
}

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime,
       TestGenerateNewCWRWCToBeSavedOnDiskValidUnsetReadConcern) {
    auto oldDefaults = setupOldDefaults();
    auto defaults = _rwcd.generateNewCWRWCToBeSavedOnDisk(
        operationContext(), repl::ReadConcernArgs(), boost::none);
    // Assert that the on disk version does not have a read/write concern set.
    ASSERT(!defaults.getDefaultReadConcern());
    ASSERT(!defaults.getDefaultReadConcernSource());

    ASSERT(defaults.getDefaultWriteConcern());
    ASSERT(oldDefaults.getDefaultWriteConcern()->w == defaults.getDefaultWriteConcern()->w);
    ASSERT_LT(*oldDefaults.getUpdateOpTime(), *defaults.getUpdateOpTime());
    ASSERT_LT(*oldDefaults.getUpdateWallClockTime(), *defaults.getUpdateWallClockTime());
    // Default write concern source is not saved on disk.
    ASSERT(!defaults.getDefaultWriteConcernSource());

    _lookupMock.setLookupCallReturnValue(std::move(defaults));
    _rwcd.refreshIfNecessary(operationContext());
    auto newDefaults = _rwcd.getDefault(operationContext());
    ASSERT_LT(oldDefaults.localUpdateWallClockTime(), newDefaults.localUpdateWallClockTime());
    // Default write concern source is calculated through 'getDefault'.
    ASSERT(newDefaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kGlobal);

    // Test that the implicit default read concern is still used after read concern is unset.
    ASSERT(newDefaults.getDefaultReadConcern());
    ASSERT(newDefaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT(newDefaults.getDefaultReadConcernSource());
    ASSERT(newDefaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kImplicit);
}

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime,
       TestGenerateNewCWRWCToBeSavedOnDiskInvalidUnsetWriteConcern) {
    setupOldDefaults();
    ASSERT_THROWS_CODE(_rwcd.generateNewCWRWCToBeSavedOnDisk(
                           operationContext(), boost::none, WriteConcernOptions()),
                       AssertionException,
                       ErrorCodes::IllegalOperation);
}

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime,
       TestGenerateNewCWRWCToBeSavedOnDiskUnsetWriteConcernNoOldDefaults) {
    auto defaults = _rwcd.generateNewCWRWCToBeSavedOnDisk(
        operationContext(), boost::none, WriteConcernOptions());
    ASSERT(!defaults.getDefaultReadConcern());
    ASSERT(!defaults.getDefaultWriteConcern());
    // Default write concern source is not saved on disk.
    ASSERT(!defaults.getDefaultWriteConcernSource());

    _lookupMock.setLookupCallReturnValue(std::move(defaults));
    _rwcd.refreshIfNecessary(operationContext());
    auto newDefaults = _rwcd.getDefault(operationContext());
    // Default write concern source is calculated through 'getDefault'.
    ASSERT(newDefaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);
}

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime,
       TestGenerateNewCWRWCToBeSavedOnDiskInvalidUnsetReadWriteConcern) {
    setupOldDefaults();
    ASSERT_THROWS_CODE(_rwcd.generateNewCWRWCToBeSavedOnDisk(
                           operationContext(), repl::ReadConcernArgs(), WriteConcernOptions()),
                       AssertionException,
                       ErrorCodes::IllegalOperation);
}

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime,
       TestGenerateNewCWRWCToBeSavedOnDiskInvalidSetWriteConcernWithoutWField) {
    setupOldDefaults();
    ASSERT_THROWS_CODE(_rwcd.generateNewCWRWCToBeSavedOnDisk(
                           operationContext(),
                           boost::none,
                           uassertStatusOK(WriteConcernOptions::parse(BSON("j" << true)))),
                       AssertionException,
                       ErrorCodes::BadValue);

    ASSERT_THROWS_CODE(_rwcd.generateNewCWRWCToBeSavedOnDisk(
                           operationContext(),
                           boost::none,
                           uassertStatusOK(WriteConcernOptions::parse(BSON("wtimeout" << 12345)))),
                       AssertionException,
                       ErrorCodes::BadValue);

    ASSERT_THROWS_CODE(
        _rwcd.generateNewCWRWCToBeSavedOnDisk(
            operationContext(),
            boost::none,
            uassertStatusOK(WriteConcernOptions::parse(BSON("j" << true << "wtimeout" << 12345)))),
        AssertionException,
        ErrorCodes::BadValue);
}

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime, TestRefreshDefaultsWithDeletedDefaults) {
    RWConcernDefault origDefaults;
    origDefaults.setUpdateOpTime(Timestamp(10, 20));
    origDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(origDefaults));

    auto origCachedDefaults = _rwcd.getDefault(operationContext());
    ASSERT_EQ(Timestamp(10, 20), *origCachedDefaults.getUpdateOpTime());
    ASSERT_EQ(Date_t::fromMillisSinceEpoch(1234), *origCachedDefaults.getUpdateWallClockTime());
    ASSERT(origCachedDefaults.getDefaultWriteConcernSource() ==
           DefaultWriteConcernSourceEnum::kImplicit);

    VectorClockMutable::get(getServiceContext())->tickClusterTime(1);
    getMockClockSource()->advance(Milliseconds(1));

    _lookupMock.setLookupCallReturnValue(RWConcernDefault());

    _rwcd.refreshIfNecessary(operationContext());

    // The cache should now contain default constructed defaults.
    auto newCachedDefaults = _rwcd.getDefault(operationContext());
    ASSERT(!newCachedDefaults.getUpdateOpTime());
    ASSERT(!newCachedDefaults.getUpdateWallClockTime());
    ASSERT_LT(origCachedDefaults.localUpdateWallClockTime(),
              newCachedDefaults.localUpdateWallClockTime());
    ASSERT(newCachedDefaults.getDefaultWriteConcernSource() ==
           DefaultWriteConcernSourceEnum::kImplicit);
}

}  // namespace
}  // namespace mongo
