// record_store_test_validate.h

/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#pragma once


#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class ValidateAdaptorSpy : public ValidateAdaptor {
public:
    ValidateAdaptorSpy() {}

    ValidateAdaptorSpy(const std::set<std::string>& remain) : _remain(remain) {}

    ~ValidateAdaptorSpy() {}

    Status validate(const RecordId& recordId, const RecordData& recordData, size_t* dataSize) {
        std::string s(recordData.data());
        ASSERT(1 == _remain.erase(s));

        *dataSize = recordData.size();
        return Status::OK();
    }

    bool allValidated() {
        return _remain.empty();
    }

private:
    std::set<std::string> _remain;  // initially contains all inserted records
};

class ValidateTest : public mongo::unittest::Test {
public:
    ValidateTest()
        : _harnessHelper(newHarnessHelper()), _rs(_harnessHelper->newNonCappedRecordStore()) {}

    ServiceContext::UniqueOperationContext newOperationContext() {
        return _harnessHelper->newOperationContext();
    }

    RecordStore& getRecordStore() {
        return *_rs;
    }

    const std::set<std::string>& getInsertedRecords() {
        return _remain;
    }

    void setUp() {
        {
            ServiceContext::UniqueOperationContext opCtx(newOperationContext());
            ASSERT_EQUALS(0, _rs->numRecords(opCtx.get()));
        }

        int nToInsert = 10;
        for (int i = 0; i < nToInsert; i++) {
            ServiceContext::UniqueOperationContext opCtx(newOperationContext());
            {
                std::stringstream ss;
                ss << "record " << i;
                std::string data = ss.str();
                ASSERT(_remain.insert(data).second);

                WriteUnitOfWork uow(opCtx.get());
                StatusWith<RecordId> res =
                    _rs->insertRecord(opCtx.get(), data.c_str(), data.size() + 1, false);
                ASSERT_OK(res.getStatus());
                uow.commit();
            }
        }

        {
            ServiceContext::UniqueOperationContext opCtx(newOperationContext());
            ASSERT_EQUALS(nToInsert, _rs->numRecords(opCtx.get()));
        }
    }

private:
    std::unique_ptr<HarnessHelper> _harnessHelper;
    std::unique_ptr<RecordStore> _rs;
    std::set<std::string> _remain;
};

}  // namespace
}  // namespace mongo
