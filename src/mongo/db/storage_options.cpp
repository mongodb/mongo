/*
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/storage_options.h"

#include "mongo/bson/util/builder.h"
#include "mongo/db/server_parameters.h"

namespace mongo {

    StorageGlobalParams storageGlobalParams;

#ifdef _WIN32
    const char* StorageGlobalParams::kDefaultDbPath = "\\data\\db\\";
    const char* StorageGlobalParams::kDefaultConfigDbPath = "\\data\\configdb\\";
#else
    const char* StorageGlobalParams::kDefaultDbPath = "/data/db";
    const char* StorageGlobalParams::kDefaultConfigDbPath = "/data/configdb";
#endif

    class JournalCommitIntervalSetting : public ServerParameter {
    public:
        JournalCommitIntervalSetting() :
            ServerParameter(ServerParameterSet::getGlobal(), "journalCommitInterval",
                    false, // allowedToChangeAtStartup
                    true // allowedToChangeAtRuntime
                    ) {}

        virtual void append(OperationContext* txn, BSONObjBuilder& b, const std::string& name) {
            b << name << storageGlobalParams.journalCommitInterval;
        }

        virtual Status set(const BSONElement& newValueElement) {
            long long newValue;
            if (!newValueElement.isNumber()) {
                StringBuilder sb;
                sb << "Expected number type for journalCommitInterval via setParameter command: "
                   << newValueElement;
                return Status(ErrorCodes::BadValue, sb.str());
            }
            if (newValueElement.type() == NumberDouble &&
                (newValueElement.numberDouble() - newValueElement.numberLong()) > 0) {
                StringBuilder sb;
                sb << "journalCommitInterval must be a whole number: "
                   << newValueElement;
                return Status(ErrorCodes::BadValue, sb.str());
            }
            newValue = newValueElement.numberLong();
            if (newValue <= 1 || newValue >= 500) {
                StringBuilder sb;
                sb << "journalCommitInterval must be between 1 and 500, but attempted to set to: "
                   << newValue;
                return Status(ErrorCodes::BadValue, sb.str());
            }
            storageGlobalParams.journalCommitInterval = static_cast<unsigned>(newValue);
            return Status::OK();
        }

        virtual Status setFromString(const std::string& str) {
            unsigned newValue;
            Status status = parseNumberFromString(str, &newValue);
            if (!status.isOK()) {
                return status;
            }
            if (newValue <= 1 || newValue >= 500) {
                StringBuilder sb;
                sb << "journalCommitInterval must be between 1 and 500, but attempted to set to: "
                   << newValue;
                return Status(ErrorCodes::BadValue, sb.str());
            }
            storageGlobalParams.journalCommitInterval = newValue;
            return Status::OK();
        }
    } journalCommitIntervalSetting;

    ExportedServerParameter<bool> NoTableScanSetting(ServerParameterSet::getGlobal(),
                                                     "notablescan",
                                                     &storageGlobalParams.noTableScan,
                                                     true,
                                                     true);

    ExportedServerParameter<double> SyncdelaySetting(ServerParameterSet::getGlobal(),
                                                     "syncdelay",
                                                     &storageGlobalParams.syncdelay,
                                                     true,
                                                     true);

} // namespace mongo
