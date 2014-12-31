// wiredtiger_index_test.cpp

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

#include "mongo/platform/basic.h"

#include <boost/scoped_ptr.hpp>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/json.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/sorted_data_interface_test_harness.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_index.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

    class MyHarnessHelper : public HarnessHelper {
    public:
        MyHarnessHelper() : _dbpath( "wt_test" ), _conn( NULL ) {

            const char* config = "create,cache_size=1G,";
            int ret = wiredtiger_open( _dbpath.path().c_str(), NULL, config, &_conn);
            invariantWTOK( ret );

            _sessionCache = new WiredTigerSessionCache( _conn );
        }

        ~MyHarnessHelper() {
            delete _sessionCache;
            _conn->close(_conn, NULL);
        }

        virtual SortedDataInterface* newSortedDataInterface( bool unique ) {
            std::string ns = "test.wt";
            OperationContextNoop txn( newRecoveryUnit() );

            BSONObj spec = BSON( "key" << BSON( "a" << 1 ) <<
                                 "name" << "testIndex" <<
                                 "ns" << ns );

            IndexDescriptor desc( NULL, "", spec );

            StatusWith<std::string> result = WiredTigerIndex::generateCreateString("", desc);
            ASSERT_OK(result.getStatus());

            string uri = "table:" + ns;
            invariantWTOK( WiredTigerIndex::Create(&txn, uri, result.getValue()));

            if ( unique )
                return new WiredTigerIndexUnique( &txn, uri, &desc );
            return new WiredTigerIndexStandard( &txn, uri, &desc );
        }

        virtual RecoveryUnit* newRecoveryUnit() {
            return new WiredTigerRecoveryUnit( _sessionCache );
        }

    private:
        unittest::TempDir _dbpath;
        WT_CONNECTION* _conn;
        WiredTigerSessionCache* _sessionCache;
    };

    HarnessHelper* newHarnessHelper() {
        return new MyHarnessHelper();
    }

    TEST(WiredTigerIndexTest, GenerateCreateStringEmptyDocument) {
        BSONObj spec = fromjson("{storageEngine: {wiredTiger: {}}}");
        IndexDescriptor desc(NULL, "", spec);
        StatusWith<std::string> result = WiredTigerIndex::generateCreateString("", desc);
        const Status& status = result.getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::BadValue, status.code());
    }

    TEST(WiredTigerIndexTest, GenerateCreateStringUnknownField) {
        BSONObj spec = fromjson("{storageEngine: {wiredTiger: {unknownField: 1}}}");
        IndexDescriptor desc(NULL, "", spec);
        StatusWith<std::string> result = WiredTigerIndex::generateCreateString("", desc);
        const Status& status = result.getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::InvalidOptions, status.code());
    }

    TEST(WiredTigerIndexTest, GenerateCreateStringNonStringConfig) {
        BSONObj spec = fromjson("{storageEngine: {wiredTiger: {configString: 12345}}}");
        IndexDescriptor desc(NULL, "", spec);
        StatusWith<std::string> result = WiredTigerIndex::generateCreateString("", desc);
        const Status& status = result.getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::TypeMismatch, status.code());
    }

    TEST(WiredTigerIndexTest, GenerateCreateStringEmptyConfigString) {
        BSONObj spec = fromjson("{storageEngine: {wiredTiger: {configString: ''}}}");
        IndexDescriptor desc(NULL, "", spec);
        StatusWith<std::string> result = WiredTigerIndex::generateCreateString("", desc);
        const Status& status = result.getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::InvalidOptions, status.code());
    }

    TEST(WiredTigerIndexTest, GenerateCreateStringValidConfigFormat) {
        BSONObj spec = fromjson("{storageEngine: {wiredTiger: {configString: 'abc=def'}}}");
        IndexDescriptor desc(NULL, "", spec);
        StatusWith<std::string> result = WiredTigerIndex::generateCreateString("", desc);
        const Status& status = result.getStatus();
        ASSERT_OK(status);
        const std::string& config = result.getValue();
        ASSERT_NOT_EQUALS(std::string::npos, config.find("abc=def"));
    }

    TEST(WiredTigerIndexTest, FullValidateMetadata) {
        MyHarnessHelper harnessHelper;
        boost::scoped_ptr<SortedDataInterface> sorted(harnessHelper.newSortedDataInterface(false));
        boost::scoped_ptr<OperationContext> opCtx(harnessHelper.newOperationContext());

        long long numKeys = 0;
        BSONObjBuilder bob;
        sorted->fullValidate(opCtx.get(), true, &numKeys, &bob);
        BSONObj obj = bob.obj();

        BSONElement metadataElement = obj.getField("metadata");
        ASSERT_TRUE(metadataElement.isABSONObj());
        BSONObj metadata = metadataElement.Obj();

        BSONElement versionElement = metadata.getField("formatVersion");
        ASSERT_TRUE(versionElement.isNumber());

        BSONElement infoObjElement = metadata.getField("infoObj");
        ASSERT_EQUALS(mongo::String, infoObjElement.type());
        ASSERT_STRING_CONTAINS(infoObjElement.String(), "test.wt");
    }

}  // namespace mongo
