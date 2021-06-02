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

#include "mongo/platform/basic.h"

#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/db/vector_clock_test_fixture.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
namespace {

class ReadWriteConcernDefaultsTest : public ServiceContextTest {
protected:
    void createDefaults(bool isImplicitWCMajority) {
        ReadWriteConcernDefaults::create(getServiceContext(), _lookupMock.getFetchDefaultsFn());
        auto& rwcd = ReadWriteConcernDefaults::get(getServiceContext());
        rwcd.setImplicitDefaultWriteConcernMajority(isImplicitWCMajority);
    }

    ReadWriteConcernDefaults::RWConcernDefaultAndTime getDefault() {
        return ReadWriteConcernDefaults::get(getServiceContext()).getDefault(_opCtx);
    }

    bool isCWWCSet() {
        return ReadWriteConcernDefaults::get(getServiceContext()).isCWWCSet(_opCtx);
    }

    ReadWriteConcernDefaultsLookupMock _lookupMock;

    bool _isDefaultWCMajorityEnabled{
        serverGlobalParams.featureCompatibility.isVersionInitialized() &&
        repl::feature_flags::gDefaultWCMajority.isEnabled(serverGlobalParams.featureCompatibility)};

    bool _isDefaultRCLocalEnabled{
        serverGlobalParams.featureCompatibility.isVersionInitialized() &&
        repl::feature_flags::gDefaultRCLocal.isEnabled(serverGlobalParams.featureCompatibility)};

    ServiceContext::UniqueOperationContext _opCtxHolder{makeOperationContext()};
    OperationContext* const _opCtx{_opCtxHolder.get()};
};

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWithAbsentCWRWCWithImplicitWCW1) {
    createDefaults(false /* isImplicitWCMajority */);

    // By not calling _lookupMock.setLookupCallReturnValue(), tests _defaults.lookup() returning
    // boost::none.
    auto defaults = getDefault();
    if (_isDefaultRCLocalEnabled) {
        ASSERT(defaults.getDefaultReadConcern());
        ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
               repl::ReadConcernLevel::kLocalReadConcern);
        ASSERT(defaults.getDefaultReadConcernSource());
        ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kImplicit);
    } else {
        ASSERT(!defaults.getDefaultReadConcern());
        ASSERT(!defaults.getDefaultReadConcernSource());
    }

    ASSERT(!defaults.getDefaultWriteConcern());
    ASSERT(!isCWWCSet());
    ASSERT(!defaults.getUpdateOpTime());
    ASSERT(!defaults.getUpdateWallClockTime());
    ASSERT_EQ(Date_t(), defaults.localUpdateWallClockTime());
    if (_isDefaultWCMajorityEnabled) {
        ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);
    }
}

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWithAbsentCWRWCWithImplicitWCMajority) {
    if (!_isDefaultWCMajorityEnabled) {
        return;
    }

    createDefaults(true /* isImplicitWCMajority */);

    // By not calling _lookupMock.setLookupCallReturnValue(), tests _defaults.lookup() returning
    // boost::none.
    ASSERT(!isCWWCSet());
    auto defaults = getDefault();
    if (_isDefaultRCLocalEnabled) {
        ASSERT(defaults.getDefaultReadConcern());
        ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
               repl::ReadConcernLevel::kLocalReadConcern);
        ASSERT(defaults.getDefaultReadConcernSource());
        ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kImplicit);
    } else {
        ASSERT(!defaults.getDefaultReadConcern());
        ASSERT(!defaults.getDefaultReadConcernSource());
    }

    ASSERT(defaults.getDefaultWriteConcern());
    ASSERT_EQ(WriteConcernOptions::kMajority, defaults.getDefaultWriteConcern().get().wMode);
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
    if (_isDefaultRCLocalEnabled) {
        ASSERT(defaults.getDefaultReadConcern());
        ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
               repl::ReadConcernLevel::kLocalReadConcern);
        ASSERT(defaults.getDefaultReadConcernSource());
        ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kImplicit);
    } else {
        ASSERT(!defaults.getDefaultReadConcern());
        ASSERT(!defaults.getDefaultReadConcernSource());
    }

    ASSERT(!defaults.getDefaultWriteConcern());
    ASSERT(!defaults.getUpdateOpTime());
    ASSERT(!defaults.getUpdateWallClockTime());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());
    if (_isDefaultWCMajorityEnabled) {
        ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);
    }
}

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWithCWRWCNeverSetWithImplicitWCMajority) {
    if (!_isDefaultWCMajorityEnabled) {
        return;
    }

    createDefaults(true /* isImplicitWCMajority */);

    // _defaults.lookup() returning default constructed RWConcern not boost::none.
    ASSERT(!isCWWCSet());
    _lookupMock.setLookupCallReturnValue({});
    auto defaults = getDefault();
    if (_isDefaultRCLocalEnabled) {
        ASSERT(defaults.getDefaultReadConcern());
        ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
               repl::ReadConcernLevel::kLocalReadConcern);
        ASSERT(defaults.getDefaultReadConcernSource());
        ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kImplicit);
    } else {
        ASSERT(!defaults.getDefaultReadConcern());
        ASSERT(!defaults.getDefaultReadConcernSource());
    }

    ASSERT(defaults.getDefaultWriteConcern());
    ASSERT_EQ(WriteConcernOptions::kMajority, defaults.getDefaultWriteConcern().get().wMode);
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
    if (_isDefaultRCLocalEnabled) {
        ASSERT(defaults.getDefaultReadConcern());
        ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
               repl::ReadConcernLevel::kLocalReadConcern);
        ASSERT(defaults.getDefaultReadConcernSource());
        ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kImplicit);
    } else {
        ASSERT(!defaults.getDefaultReadConcern());
        ASSERT(!defaults.getDefaultReadConcernSource());
    }

    ASSERT(!defaults.getDefaultWriteConcern());
    ASSERT_EQ(Timestamp(1, 2), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());
    if (_isDefaultWCMajorityEnabled) {
        ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);
    }
}

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWithUnsetCWRWCWithImplicitWCMajority) {
    if (!_isDefaultWCMajorityEnabled) {
        return;
    }

    createDefaults(true /* isImplicitWCMajority */);

    RWConcernDefault newDefaults;
    newDefaults.setUpdateOpTime(Timestamp(1, 2));
    newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    ASSERT(!isCWWCSet());
    auto defaults = getDefault();
    if (_isDefaultRCLocalEnabled) {
        ASSERT(defaults.getDefaultReadConcern());
        ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
               repl::ReadConcernLevel::kLocalReadConcern);
        ASSERT(defaults.getDefaultReadConcernSource());
        ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kImplicit);
    } else {
        ASSERT(!defaults.getDefaultReadConcern());
        ASSERT(!defaults.getDefaultReadConcernSource());
    }

    ASSERT(defaults.getDefaultWriteConcern());
    ASSERT_EQ(WriteConcernOptions::kMajority, defaults.getDefaultWriteConcern().get().wMode);
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
    if (_isDefaultRCLocalEnabled) {
        ASSERT(oldDefaults.getDefaultReadConcern()->getLevel() ==
               repl::ReadConcernLevel::kLocalReadConcern);
        ASSERT(oldDefaults.getDefaultReadConcernSource());
        ASSERT(oldDefaults.getDefaultReadConcernSource() ==
               DefaultReadConcernSourceEnum::kImplicit);
    } else {
        ASSERT(!oldDefaults.getDefaultReadConcernSource());
    }
    ASSERT(!oldDefaults.getDefaultWriteConcern());
    ASSERT(!oldDefaults.getUpdateOpTime());
    ASSERT(!oldDefaults.getUpdateWallClockTime());
    ASSERT_GT(oldDefaults.localUpdateWallClockTime(), Date_t());
    if (_isDefaultWCMajorityEnabled) {
        ASSERT(oldDefaults.getDefaultWriteConcernSource() ==
               DefaultWriteConcernSourceEnum::kImplicit);
    }

    RWConcernDefault newDefaults;
    newDefaults.setDefaultReadConcern(
        repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern));
    WriteConcernOptions wc;
    wc.wNumNodes = 4;
    wc.usedDefaultConstructedWC = false;
    wc.notExplicitWValue = false;
    newDefaults.setDefaultWriteConcern(wc);
    newDefaults.setUpdateOpTime(Timestamp(1, 2));
    newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    ReadWriteConcernDefaults::get(getServiceContext()).invalidate();
    if (_isDefaultWCMajorityEnabled) {
        ASSERT(isCWWCSet());
    }
    auto defaults = getDefault();
    ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kLocalReadConcern);
    if (_isDefaultRCLocalEnabled) {
        ASSERT(defaults.getDefaultReadConcernSource());
        ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kGlobal);
    } else {
        ASSERT(!defaults.getDefaultReadConcernSource());
    }
    ASSERT_EQ(4, defaults.getDefaultWriteConcern()->wNumNodes);
    ASSERT_EQ(Timestamp(1, 2), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());
    if (_isDefaultWCMajorityEnabled) {
        ASSERT(defaults.getDefaultWriteConcernSource());
        ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kGlobal);
    }
}

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWithCWRWCNotSetThenSetWithImplicitWCMajority) {
    if (!_isDefaultWCMajorityEnabled) {
        return;
    }

    createDefaults(true /* isImplicitWCMajority */);

    // _defaults.lookup() returning default constructed RWConcern not boost::none.
    _lookupMock.setLookupCallReturnValue({});
    ASSERT(!isCWWCSet());
    auto oldDefaults = getDefault();
    if (_isDefaultRCLocalEnabled) {
        ASSERT(oldDefaults.getDefaultReadConcern()->getLevel() ==
               repl::ReadConcernLevel::kLocalReadConcern);
        ASSERT(oldDefaults.getDefaultReadConcernSource());
        ASSERT(oldDefaults.getDefaultReadConcernSource() ==
               DefaultReadConcernSourceEnum::kImplicit);
    } else {
        ASSERT(!oldDefaults.getDefaultReadConcernSource());
    }
    ASSERT(oldDefaults.getDefaultWriteConcern());
    ASSERT_EQ(WriteConcernOptions::kMajority, oldDefaults.getDefaultWriteConcern().get().wMode);
    ASSERT(!oldDefaults.getUpdateOpTime());
    ASSERT(!oldDefaults.getUpdateWallClockTime());
    ASSERT_GT(oldDefaults.localUpdateWallClockTime(), Date_t());
    ASSERT(oldDefaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);

    RWConcernDefault newDefaults;
    newDefaults.setDefaultReadConcern(
        repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern));
    WriteConcernOptions wc;
    wc.wNumNodes = 4;
    wc.usedDefaultConstructedWC = false;
    wc.notExplicitWValue = false;
    newDefaults.setDefaultWriteConcern(wc);
    newDefaults.setUpdateOpTime(Timestamp(1, 2));
    newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    ReadWriteConcernDefaults::get(getServiceContext()).invalidate();
    ASSERT(isCWWCSet());
    auto defaults = getDefault();
    ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kLocalReadConcern);
    if (_isDefaultRCLocalEnabled) {
        ASSERT(defaults.getDefaultReadConcernSource());
        ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kGlobal);
    } else {
        ASSERT(!defaults.getDefaultReadConcernSource());
    }

    ASSERT_EQ(4, defaults.getDefaultWriteConcern()->wNumNodes);
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
    WriteConcernOptions wc;
    wc.wNumNodes = 4;
    wc.usedDefaultConstructedWC = false;
    wc.notExplicitWValue = false;
    newDefaults.setDefaultWriteConcern(wc);
    newDefaults.setUpdateOpTime(Timestamp(1, 2));
    newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    if (_isDefaultWCMajorityEnabled) {
        ASSERT(isCWWCSet());
    }
    auto defaults = getDefault();
    ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kLocalReadConcern);
    if (_isDefaultRCLocalEnabled) {
        ASSERT(defaults.getDefaultReadConcernSource());
        ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kGlobal);
    } else {
        ASSERT(!defaults.getDefaultReadConcernSource());
    }
    ASSERT_EQ(4, defaults.getDefaultWriteConcern()->wNumNodes);
    ASSERT_EQ(Timestamp(1, 2), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());
    if (_isDefaultWCMajorityEnabled) {
        ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kGlobal);
    }
}

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWithSetCWRWCWithImplicitWCMajority) {
    if (!_isDefaultWCMajorityEnabled) {
        return;
    }

    createDefaults(true /* isImplicitWCMajority */);

    RWConcernDefault newDefaults;
    newDefaults.setDefaultReadConcern(
        repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern));
    WriteConcernOptions wc;
    wc.wNumNodes = 4;
    wc.usedDefaultConstructedWC = false;
    wc.notExplicitWValue = false;
    newDefaults.setDefaultWriteConcern(wc);
    newDefaults.setUpdateOpTime(Timestamp(1, 2));
    newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    ASSERT(isCWWCSet());
    auto defaults = getDefault();
    ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kLocalReadConcern);
    if (_isDefaultRCLocalEnabled) {
        ASSERT(defaults.getDefaultReadConcernSource());
        ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kGlobal);
    }

    ASSERT_EQ(4, defaults.getDefaultWriteConcern()->wNumNodes);
    ASSERT_EQ(Timestamp(1, 2), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());
    ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kGlobal);
}

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWriteConcernSourceImplicitWithUsedDefaultWC) {
    if (!_isDefaultWCMajorityEnabled) {
        return;
    }

    createDefaults(true /* isImplicitWCMajority */);

    RWConcernDefault newDefaults;
    newDefaults.setDefaultWriteConcern(WriteConcernOptions());
    newDefaults.setUpdateOpTime(Timestamp(1, 2));
    newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));

    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    ASSERT(!isCWWCSet());
    auto defaults = getDefault();
    if (_isDefaultRCLocalEnabled) {
        ASSERT(defaults.getDefaultReadConcern());
        ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
               repl::ReadConcernLevel::kLocalReadConcern);
        ASSERT(defaults.getDefaultReadConcernSource());
        ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kImplicit);
    } else {
        ASSERT(!defaults.getDefaultReadConcern());
        ASSERT(!defaults.getDefaultReadConcernSource());
    }

    // The default write concern source should be set to implicit if wc.usedDefaultConstructedWC is
    // true
    ASSERT_EQ(0, defaults.getDefaultWriteConcern()->wNumNodes);
    ASSERT_EQ(WriteConcernOptions::kMajority, defaults.getDefaultWriteConcern().get().wMode);
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
    if (_isDefaultRCLocalEnabled) {
        ASSERT(defaults.getDefaultReadConcernSource());
        ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kGlobal);
    }
    ASSERT(!defaults.getDefaultWriteConcern());
    ASSERT_EQ(Timestamp(1, 2), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());
    if (_isDefaultWCMajorityEnabled) {
        ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);
    }

    // unsetting default read concern.
    RWConcernDefault newDefaults2;
    newDefaults2.setUpdateOpTime(Timestamp(1, 3));
    newDefaults2.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults2));

    ReadWriteConcernDefaults::get(getServiceContext()).refreshIfNecessary(_opCtx);

    ASSERT(!isCWWCSet());
    defaults = getDefault();
    if (_isDefaultRCLocalEnabled) {
        ASSERT(defaults.getDefaultReadConcern());
        ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
               repl::ReadConcernLevel::kLocalReadConcern);
        ASSERT(defaults.getDefaultReadConcernSource());
        ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kImplicit);
    } else {
        ASSERT(!defaults.getDefaultReadConcern());
        ASSERT(!defaults.getDefaultReadConcernSource());
    }

    ASSERT(!defaults.getDefaultWriteConcern());
    ASSERT_EQ(Timestamp(1, 3), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());
    if (_isDefaultWCMajorityEnabled) {
        ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);
    }
}

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWithSetAndUnSetCWRCWithImplicitWCMajority) {
    if (!_isDefaultWCMajorityEnabled) {
        return;
    }

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
    if (_isDefaultRCLocalEnabled) {
        ASSERT(defaults.getDefaultReadConcernSource());
        ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kGlobal);
    }
    ASSERT(defaults.getDefaultWriteConcern());
    ASSERT_EQ(WriteConcernOptions::kMajority, defaults.getDefaultWriteConcern().get().wMode);
    ASSERT_EQ(Timestamp(1, 2), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());
    ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);

    // Unsetting default read concern.
    RWConcernDefault newDefaults2;
    newDefaults2.setUpdateOpTime(Timestamp(1, 3));
    newDefaults2.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults2));

    ReadWriteConcernDefaults::get(getServiceContext()).refreshIfNecessary(_opCtx);

    ASSERT(!isCWWCSet());
    defaults = getDefault();
    if (_isDefaultRCLocalEnabled) {
        ASSERT(defaults.getDefaultReadConcern());
        ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
               repl::ReadConcernLevel::kLocalReadConcern);
        ASSERT(defaults.getDefaultReadConcernSource());
        ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kImplicit);
    } else {
        ASSERT(!defaults.getDefaultReadConcern());
        ASSERT(!defaults.getDefaultReadConcernSource());
    }

    ASSERT_EQ(WriteConcernOptions::kMajority, defaults.getDefaultWriteConcern().get().wMode);
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
    if (_isDefaultRCLocalEnabled) {
        ASSERT(defaults.getDefaultReadConcern());
        ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
               repl::ReadConcernLevel::kLocalReadConcern);
        ASSERT(defaults.getDefaultReadConcernSource());
        ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kImplicit);
    } else {
        ASSERT(!defaults.getDefaultReadConcern());
        ASSERT(!defaults.getDefaultReadConcernSource());
    }
    ASSERT(!defaults.getDefaultWriteConcern());
    ASSERT_EQ(Timestamp(1, 2), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());
    if (_isDefaultWCMajorityEnabled) {
        ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);
    }

    RWConcernDefault newDefaults2;
    newDefaults2.setDefaultReadConcern(
        repl::ReadConcernArgs(repl::ReadConcernLevel::kAvailableReadConcern));
    WriteConcernOptions wc;
    wc.wNumNodes = 4;
    wc.usedDefaultConstructedWC = false;
    wc.notExplicitWValue = false;
    newDefaults2.setDefaultWriteConcern(wc);
    newDefaults2.setUpdateOpTime(Timestamp(3, 4));
    newDefaults2.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(5678));
    if (_isDefaultWCMajorityEnabled) {
        newDefaults2.setDefaultWriteConcernSource(DefaultWriteConcernSourceEnum::kGlobal);
    }
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults2));

    ASSERT(!isCWWCSet());
    auto defaults2 = getDefault();
    if (_isDefaultRCLocalEnabled) {
        ASSERT(defaults2.getDefaultReadConcern());
        ASSERT(defaults2.getDefaultReadConcern()->getLevel() ==
               repl::ReadConcernLevel::kLocalReadConcern);
        ASSERT(defaults2.getDefaultReadConcernSource());
        ASSERT(defaults2.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kImplicit);
    } else {
        ASSERT(!defaults2.getDefaultReadConcern());
        ASSERT(!defaults2.getDefaultReadConcernSource());
    }
    ASSERT(!defaults2.getDefaultWriteConcern());
    ASSERT_EQ(Timestamp(1, 2), *defaults2.getUpdateOpTime());
    ASSERT_EQ(1234, defaults2.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults2.localUpdateWallClockTime(), Date_t());
    if (_isDefaultWCMajorityEnabled) {
        ASSERT(defaults2.getDefaultWriteConcernSource() ==
               DefaultWriteConcernSourceEnum::kImplicit);
    }
}

TEST_F(ReadWriteConcernDefaultsTest, TestInvalidate) {
    createDefaults(false /* isImplicitWCMajority */);

    RWConcernDefault newDefaults;
    newDefaults.setUpdateOpTime(Timestamp(1, 2));
    newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    ASSERT(!isCWWCSet());
    auto defaults = getDefault();
    if (_isDefaultRCLocalEnabled) {
        ASSERT(defaults.getDefaultReadConcern());
        ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
               repl::ReadConcernLevel::kLocalReadConcern);
        ASSERT(defaults.getDefaultReadConcernSource());
        ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kImplicit);
    } else {
        ASSERT(!defaults.getDefaultReadConcern());
        ASSERT(!defaults.getDefaultReadConcernSource());
    }
    ASSERT(!defaults.getDefaultWriteConcern());
    ASSERT_EQ(Timestamp(1, 2), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());
    if (_isDefaultWCMajorityEnabled) {
        ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);
    }

    RWConcernDefault newDefaults2;
    newDefaults2.setDefaultReadConcern(
        repl::ReadConcernArgs(repl::ReadConcernLevel::kAvailableReadConcern));
    WriteConcernOptions wc;
    wc.wNumNodes = 4;
    wc.usedDefaultConstructedWC = false;
    wc.notExplicitWValue = false;
    newDefaults2.setDefaultWriteConcern(wc);
    newDefaults2.setUpdateOpTime(Timestamp(3, 4));
    newDefaults2.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(5678));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults2));

    ReadWriteConcernDefaults::get(getServiceContext()).invalidate();
    if (_isDefaultWCMajorityEnabled) {
        ASSERT(isCWWCSet());
    }
    auto defaults2 = getDefault();
    ASSERT(defaults2.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kAvailableReadConcern);
    if (_isDefaultRCLocalEnabled) {
        ASSERT(defaults2.getDefaultReadConcernSource());
        ASSERT(defaults2.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kGlobal);
    }
    ASSERT_EQ(4, defaults2.getDefaultWriteConcern()->wNumNodes);
    ASSERT_EQ(Timestamp(3, 4), *defaults2.getUpdateOpTime());
    ASSERT_EQ(5678, defaults2.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults2.localUpdateWallClockTime(), Date_t());
    if (_isDefaultWCMajorityEnabled) {
        ASSERT(defaults2.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kGlobal);
    }
}

TEST_F(ReadWriteConcernDefaultsTest, TestRefreshDefaultsWithEmptyCacheAndAbsentDefaults) {
    createDefaults(false /* isImplicitWCMajority */);
    ReadWriteConcernDefaults::get(getServiceContext()).refreshIfNecessary(_opCtx);

    ASSERT(!isCWWCSet());
    auto defaults = getDefault();
    if (_isDefaultRCLocalEnabled) {
        ASSERT(defaults.getDefaultReadConcern());
        ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
               repl::ReadConcernLevel::kLocalReadConcern);
        ASSERT(defaults.getDefaultReadConcernSource());
        ASSERT(defaults.getDefaultReadConcernSource() == DefaultReadConcernSourceEnum::kImplicit);
    } else {
        ASSERT(!defaults.getDefaultReadConcern());
        ASSERT(!defaults.getDefaultReadConcernSource());
    }

    ASSERT(!defaults.getDefaultWriteConcern());
    ASSERT(!defaults.getUpdateOpTime());
    ASSERT(!defaults.getUpdateWallClockTime());
    ASSERT_EQ(Date_t(), defaults.localUpdateWallClockTime());
    if (_isDefaultWCMajorityEnabled) {
        ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);
    }
}

TEST_F(ReadWriteConcernDefaultsTest, TestRefreshDefaultsWithEmptyCacheAndSetDefaults) {
    createDefaults(false /* isImplicitWCMajority */);

    RWConcernDefault newDefaults;
    newDefaults.setUpdateOpTime(Timestamp(1, 2));
    newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    ReadWriteConcernDefaults::get(getServiceContext()).refreshIfNecessary(_opCtx);

    ASSERT(!isCWWCSet());
    auto defaults = getDefault();
    ASSERT_EQ(Timestamp(1, 2), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());
    if (_isDefaultWCMajorityEnabled) {
        ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);
    }
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
    if (_isDefaultWCMajorityEnabled) {
        ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);
    }

    RWConcernDefault newDefaults2;
    newDefaults2.setUpdateOpTime(Timestamp(3, 4));
    newDefaults2.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(5678));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults2));

    ReadWriteConcernDefaults::get(getServiceContext()).refreshIfNecessary(_opCtx);

    ASSERT(!isCWWCSet());
    auto defaults2 = getDefault();
    ASSERT_EQ(Timestamp(3, 4), *defaults2.getUpdateOpTime());
    ASSERT_EQ(5678, defaults2.getUpdateWallClockTime()->toMillisSinceEpoch());
    if (_isDefaultWCMajorityEnabled) {
        ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);
    }
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
    if (_isDefaultWCMajorityEnabled) {
        ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);
    }

    RWConcernDefault newDefaults2;
    newDefaults2.setUpdateOpTime(Timestamp(5, 6));
    newDefaults2.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(5678));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults2));

    ReadWriteConcernDefaults::get(getServiceContext()).refreshIfNecessary(_opCtx);

    auto defaults2 = getDefault();
    ASSERT_EQ(Timestamp(10, 20), *defaults2.getUpdateOpTime());
    ASSERT_EQ(1234, defaults2.getUpdateWallClockTime()->toMillisSinceEpoch());
    if (_isDefaultWCMajorityEnabled) {
        ASSERT(defaults.getDefaultWriteConcernSource() == DefaultWriteConcernSourceEnum::kImplicit);
    }
}

/**
 * ReadWriteConcernDefaults::generateNewCWRWCToBeSavedOnDisk() uses the current clusterTime and wall
 * clock time (for epoch and setTime/localSetTime), so testing it requires a fixture with a logical
 * clock.
 */
class ReadWriteConcernDefaultsTestWithClusterTime : public VectorClockTestFixture {
public:
    virtual ~ReadWriteConcernDefaultsTestWithClusterTime() {
        ReadWriteConcernDefaults::get(getServiceContext()).invalidate();
    }

protected:
    auto setupOldDefaults() {
        auto defaults = _rwcd.generateNewCWRWCToBeSavedOnDisk(
            operationContext(),
            repl::ReadConcernArgs(repl::ReadConcernLevel::kAvailableReadConcern),
            uassertStatusOK(WriteConcernOptions::parse(BSON("w" << 4))));

        ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
               repl::ReadConcernLevel::kAvailableReadConcern);
        if (_isDefaultRCLocalEnabled) {
            // default read concern source is not saved on disk.
            ASSERT(!defaults.getDefaultReadConcernSource());
        }

        ASSERT_EQ(4, defaults.getDefaultWriteConcern()->wNumNodes);
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
        ReadWriteConcernDefaults::create(getServiceContext(), _lookupMock.getFetchDefaultsFn());
        return ReadWriteConcernDefaults::get(getServiceContext());
    }()};

    bool _isDefaultWCMajorityEnabled{
        serverGlobalParams.featureCompatibility.isVersionInitialized() &&
        repl::feature_flags::gDefaultWCMajority.isEnabled(serverGlobalParams.featureCompatibility)};

    bool _isDefaultRCLocalEnabled{
        serverGlobalParams.featureCompatibility.isVersionInitialized() &&
        repl::feature_flags::gDefaultRCLocal.isEnabled(serverGlobalParams.featureCompatibility)};
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
            repl::ReadConcernArgs::fromBSONThrows(BSON("level"
                                                       << "local"
                                                       << "afterOpTime" << repl::OpTime())),
            boost::none),
        AssertionException,
        ErrorCodes::BadValue);
    ASSERT_THROWS_CODE(
        _rwcd.generateNewCWRWCToBeSavedOnDisk(
            operationContext(),
            repl::ReadConcernArgs::fromBSONThrows(BSON("level"
                                                       << "local"
                                                       << "afterClusterTime" << Timestamp(1, 2))),
            boost::none),
        AssertionException,
        ErrorCodes::BadValue);
    ASSERT_THROWS_CODE(
        _rwcd.generateNewCWRWCToBeSavedOnDisk(
            operationContext(),
            repl::ReadConcernArgs::fromBSONThrows(BSON("level"
                                                       << "snapshot"
                                                       << "atClusterTime" << Timestamp(1, 2))),
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

    ASSERT_EQ(5, defaults.getDefaultWriteConcern()->wNumNodes);
    ASSERT_LT(*oldDefaults.getUpdateOpTime(), *defaults.getUpdateOpTime());
    ASSERT_LT(*oldDefaults.getUpdateWallClockTime(), *defaults.getUpdateWallClockTime());
    // Default write concern source is not saved on disk.
    ASSERT(!defaults.getDefaultWriteConcernSource());

    _lookupMock.setLookupCallReturnValue(std::move(defaults));
    _rwcd.refreshIfNecessary(operationContext());
    auto newDefaults = _rwcd.getDefault(operationContext());
    ASSERT_LT(oldDefaults.localUpdateWallClockTime(), newDefaults.localUpdateWallClockTime());
    if (_isDefaultWCMajorityEnabled) {
        // Default write concern source is calculated through 'getDefault'.
        ASSERT(newDefaults.getDefaultWriteConcernSource() ==
               DefaultWriteConcernSourceEnum::kGlobal);
    }
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

    ASSERT_EQ(oldDefaults.getDefaultWriteConcern()->wNumNodes,
              defaults.getDefaultWriteConcern()->wNumNodes);
    ASSERT_LT(*oldDefaults.getUpdateOpTime(), *defaults.getUpdateOpTime());
    ASSERT_LT(*oldDefaults.getUpdateWallClockTime(), *defaults.getUpdateWallClockTime());
    // Default write concern source is not saved on disk.
    ASSERT(!defaults.getDefaultWriteConcernSource());

    _lookupMock.setLookupCallReturnValue(std::move(defaults));
    _rwcd.refreshIfNecessary(operationContext());
    auto newDefaults = _rwcd.getDefault(operationContext());
    ASSERT_LT(oldDefaults.localUpdateWallClockTime(), newDefaults.localUpdateWallClockTime());
    if (_isDefaultWCMajorityEnabled) {
        // Default write concern source is calculated through 'getDefault'.
        ASSERT(newDefaults.getDefaultWriteConcernSource() ==
               DefaultWriteConcernSourceEnum::kGlobal);
    }
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
    if (_isDefaultWCMajorityEnabled) {
        // Default write concern source is calculated through 'getDefault'.
        ASSERT(newDefaults.getDefaultWriteConcernSource() ==
               DefaultWriteConcernSourceEnum::kImplicit);
    }
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

    ASSERT_EQ(5, defaults.getDefaultWriteConcern()->wNumNodes);
    ASSERT_LT(*oldDefaults.getUpdateOpTime(), *defaults.getUpdateOpTime());
    ASSERT_LT(*oldDefaults.getUpdateWallClockTime(), *defaults.getUpdateWallClockTime());
    // Default write concern source is not saved on disk.
    ASSERT(!defaults.getDefaultWriteConcernSource());

    _lookupMock.setLookupCallReturnValue(std::move(defaults));
    _rwcd.refreshIfNecessary(operationContext());
    auto newDefaults = _rwcd.getDefault(operationContext());
    ASSERT_LT(oldDefaults.localUpdateWallClockTime(), newDefaults.localUpdateWallClockTime());
    if (_isDefaultWCMajorityEnabled) {
        // Default write concern source is calculated through 'getDefault'.
        ASSERT(newDefaults.getDefaultWriteConcernSource() ==
               DefaultWriteConcernSourceEnum::kGlobal);
    }
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
    if (_isDefaultWCMajorityEnabled) {
        // Default write concern source is calculated through 'getDefault'.
        ASSERT(newDefaults.getDefaultWriteConcernSource() ==
               DefaultWriteConcernSourceEnum::kImplicit);
    }

    VectorClockMutable::get(getServiceContext())->tickClusterTime(1);
    getMockClockSource()->advance(Milliseconds(1));

    auto defaults = _rwcd.generateNewCWRWCToBeSavedOnDisk(
        operationContext(),
        boost::none,
        uassertStatusOK(WriteConcernOptions::parse(BSON("w" << 5))));
    ASSERT(newDefaults.getDefaultReadConcern()->getLevel() ==
           defaults.getDefaultReadConcern()->getLevel());
    ASSERT_EQ(5, defaults.getDefaultWriteConcern()->wNumNodes);
    // Default write concern source is not saved on disk.
    ASSERT(!defaults.getDefaultWriteConcernSource());

    _lookupMock.setLookupCallReturnValue(std::move(defaults));
    _rwcd.refreshIfNecessary(operationContext());
    newDefaults = _rwcd.getDefault(operationContext());
    ASSERT(newDefaults.getDefaultWriteConcern());
    ASSERT_EQ(5, newDefaults.getDefaultWriteConcern()->wNumNodes);
    if (_isDefaultWCMajorityEnabled) {
        // Default write concern source is calculated through 'getDefault'.
        ASSERT(newDefaults.getDefaultWriteConcernSource() ==
               DefaultWriteConcernSourceEnum::kGlobal);
    }
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
    ASSERT_EQ(oldDefaults.getDefaultWriteConcern()->wNumNodes,
              defaults.getDefaultWriteConcern()->wNumNodes);
    ASSERT_LT(*oldDefaults.getUpdateOpTime(), *defaults.getUpdateOpTime());
    ASSERT_LT(*oldDefaults.getUpdateWallClockTime(), *defaults.getUpdateWallClockTime());
    // Default write concern source is not saved on disk.
    ASSERT(!defaults.getDefaultWriteConcernSource());

    _lookupMock.setLookupCallReturnValue(std::move(defaults));
    _rwcd.refreshIfNecessary(operationContext());
    auto newDefaults = _rwcd.getDefault(operationContext());
    ASSERT_LT(oldDefaults.localUpdateWallClockTime(), newDefaults.localUpdateWallClockTime());
    if (_isDefaultWCMajorityEnabled) {
        // Default write concern source is calculated through 'getDefault'.
        ASSERT(newDefaults.getDefaultWriteConcernSource() ==
               DefaultWriteConcernSourceEnum::kGlobal);
    }

    // Test that the implicit default read concern is still used after read concern is unset.
    if (_isDefaultRCLocalEnabled) {
        ASSERT(newDefaults.getDefaultReadConcern());
        ASSERT(newDefaults.getDefaultReadConcern()->getLevel() ==
               repl::ReadConcernLevel::kLocalReadConcern);
        ASSERT(newDefaults.getDefaultReadConcernSource());
        ASSERT(newDefaults.getDefaultReadConcernSource() ==
               DefaultReadConcernSourceEnum::kImplicit);
    } else {
        ASSERT(!newDefaults.getDefaultReadConcern());
        ASSERT(!newDefaults.getDefaultReadConcernSource());
    }
}

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime,
       TestGenerateNewCWRWCToBeSavedOnDiskInvalidUnsetWriteConcern) {
    auto oldDefaults = setupOldDefaults();
    if (repl::feature_flags::gDefaultWCMajority.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        ASSERT_THROWS_CODE(_rwcd.generateNewCWRWCToBeSavedOnDisk(
                               operationContext(), boost::none, WriteConcernOptions()),
                           AssertionException,
                           ErrorCodes::IllegalOperation);
    } else {
        auto defaults = _rwcd.generateNewCWRWCToBeSavedOnDisk(
            operationContext(), boost::none, WriteConcernOptions());
        ASSERT(defaults.getDefaultReadConcern());
        ASSERT(oldDefaults.getDefaultReadConcern()->getLevel() ==
               defaults.getDefaultReadConcern()->getLevel());
        ASSERT(!defaults.getDefaultReadConcernSource());
        ASSERT(!defaults.getDefaultWriteConcern());
        ASSERT_LT(*oldDefaults.getUpdateOpTime(), *defaults.getUpdateOpTime());
        ASSERT_LT(*oldDefaults.getUpdateWallClockTime(), *defaults.getUpdateWallClockTime());

        _lookupMock.setLookupCallReturnValue(std::move(defaults));
        _rwcd.refreshIfNecessary(operationContext());
        auto newDefaults = _rwcd.getDefault(operationContext());
        ASSERT_LT(oldDefaults.localUpdateWallClockTime(), newDefaults.localUpdateWallClockTime());
    }
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
    if (_isDefaultWCMajorityEnabled) {
        // Default write concern source is calculated through 'getDefault'.
        ASSERT(newDefaults.getDefaultWriteConcernSource() ==
               DefaultWriteConcernSourceEnum::kImplicit);
    }
}

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime,
       TestGenerateNewCWRWCToBeSavedOnDiskInvalidUnsetReadWriteConcern) {
    auto oldDefaults = setupOldDefaults();
    if (repl::feature_flags::gDefaultWCMajority.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        ASSERT_THROWS_CODE(_rwcd.generateNewCWRWCToBeSavedOnDisk(
                               operationContext(), repl::ReadConcernArgs(), WriteConcernOptions()),
                           AssertionException,
                           ErrorCodes::IllegalOperation);
    } else {
        auto defaults = _rwcd.generateNewCWRWCToBeSavedOnDisk(
            operationContext(), repl::ReadConcernArgs(), WriteConcernOptions());
        // Assert that the on disk version does not have a read/write concern set.
        ASSERT(!defaults.getDefaultReadConcern());
        ASSERT(!defaults.getDefaultReadConcernSource());

        ASSERT(!defaults.getDefaultWriteConcern());
        ASSERT_LT(*oldDefaults.getUpdateOpTime(), *defaults.getUpdateOpTime());
        ASSERT_LT(*oldDefaults.getUpdateWallClockTime(), *defaults.getUpdateWallClockTime());

        _lookupMock.setLookupCallReturnValue(std::move(defaults));
        _rwcd.refreshIfNecessary(operationContext());
        auto newDefaults = _rwcd.getDefault(operationContext());
        ASSERT_LT(oldDefaults.localUpdateWallClockTime(), newDefaults.localUpdateWallClockTime());

        if (_isDefaultRCLocalEnabled) {
            ASSERT(newDefaults.getDefaultReadConcern());
            ASSERT(newDefaults.getDefaultReadConcern()->getLevel() ==
                   repl::ReadConcernLevel::kLocalReadConcern);
            ASSERT(newDefaults.getDefaultReadConcernSource());
            ASSERT(newDefaults.getDefaultReadConcernSource() ==
                   DefaultReadConcernSourceEnum::kImplicit);
        } else {
            ASSERT(!newDefaults.getDefaultReadConcern());
            ASSERT(!newDefaults.getDefaultReadConcernSource());
        }
    }
}

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime,
       TestGenerateNewCWRWCToBeSavedOnDiskValidSetWriteConcernWithOnlyJ) {
    auto oldDefaults = setupOldDefaults();
    auto defaults = _rwcd.generateNewCWRWCToBeSavedOnDisk(
        operationContext(),
        boost::none,
        uassertStatusOK(WriteConcernOptions::parse(BSON("j" << true))));
    ASSERT(oldDefaults.getDefaultReadConcern()->getLevel() ==
           defaults.getDefaultReadConcern()->getLevel());
    ASSERT(!defaults.getDefaultReadConcernSource());
    ASSERT_EQ(1, defaults.getDefaultWriteConcern()->wNumNodes);
    ASSERT_EQ(0, defaults.getDefaultWriteConcern()->wTimeout);
    ASSERT(WriteConcernOptions::SyncMode::JOURNAL == defaults.getDefaultWriteConcern()->syncMode);
    ASSERT_LT(*oldDefaults.getUpdateOpTime(), *defaults.getUpdateOpTime());
    ASSERT_LT(*oldDefaults.getUpdateWallClockTime(), *defaults.getUpdateWallClockTime());
    // Default write concern source is not saved on disk.
    ASSERT(!defaults.getDefaultWriteConcernSource());

    _lookupMock.setLookupCallReturnValue(std::move(defaults));
    _rwcd.refreshIfNecessary(operationContext());
    auto newDefaults = _rwcd.getDefault(operationContext());
    ASSERT_LT(oldDefaults.localUpdateWallClockTime(), newDefaults.localUpdateWallClockTime());
    if (_isDefaultWCMajorityEnabled) {
        // Default write concern source is calculated through 'getDefault'.
        ASSERT(newDefaults.getDefaultWriteConcernSource() ==
               DefaultWriteConcernSourceEnum::kGlobal);
    }
}

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime,
       TestGenerateNewCWRWCToBeSavedOnDiskValidSetWriteConcernWithOnlyWtimeout) {
    auto oldDefaults = setupOldDefaults();
    auto defaults = _rwcd.generateNewCWRWCToBeSavedOnDisk(
        operationContext(),
        boost::none,
        uassertStatusOK(WriteConcernOptions::parse(BSON("wtimeout" << 12345))));
    ASSERT(oldDefaults.getDefaultReadConcern()->getLevel() ==
           defaults.getDefaultReadConcern()->getLevel());
    ASSERT(!defaults.getDefaultReadConcernSource());
    ASSERT_EQ(1, defaults.getDefaultWriteConcern()->wNumNodes);
    ASSERT_EQ(12345, defaults.getDefaultWriteConcern()->wTimeout);
    ASSERT(WriteConcernOptions::SyncMode::UNSET == defaults.getDefaultWriteConcern()->syncMode);
    ASSERT_LT(*oldDefaults.getUpdateOpTime(), *defaults.getUpdateOpTime());
    ASSERT_LT(*oldDefaults.getUpdateWallClockTime(), *defaults.getUpdateWallClockTime());
    // Default write concern source is not saved on disk.
    ASSERT(!defaults.getDefaultWriteConcernSource());

    _lookupMock.setLookupCallReturnValue(std::move(defaults));
    _rwcd.refreshIfNecessary(operationContext());
    auto newDefaults = _rwcd.getDefault(operationContext());
    ASSERT_LT(oldDefaults.localUpdateWallClockTime(), newDefaults.localUpdateWallClockTime());
    if (_isDefaultWCMajorityEnabled) {
        // Default write concern source is calculated through 'getDefault'.
        ASSERT(newDefaults.getDefaultWriteConcernSource() ==
               DefaultWriteConcernSourceEnum::kGlobal);
    }
}

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime, TestRefreshDefaultsWithDeletedDefaults) {
    RWConcernDefault origDefaults;
    origDefaults.setUpdateOpTime(Timestamp(10, 20));
    origDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(origDefaults));

    auto origCachedDefaults = _rwcd.getDefault(operationContext());
    ASSERT_EQ(Timestamp(10, 20), *origCachedDefaults.getUpdateOpTime());
    ASSERT_EQ(Date_t::fromMillisSinceEpoch(1234), *origCachedDefaults.getUpdateWallClockTime());
    if (_isDefaultWCMajorityEnabled) {
        ASSERT(origCachedDefaults.getDefaultWriteConcernSource() ==
               DefaultWriteConcernSourceEnum::kImplicit);
    }

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
    if (_isDefaultWCMajorityEnabled) {
        ASSERT(newCachedDefaults.getDefaultWriteConcernSource() ==
               DefaultWriteConcernSourceEnum::kImplicit);
    }
}

}  // namespace
}  // namespace mongo
