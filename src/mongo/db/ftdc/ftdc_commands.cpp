/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <boost/filesystem.hpp>

#include "mongo/base/init.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/ftdc/controller.h"
#include "mongo/db/ftdc/file_reader.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"

namespace mongo {
namespace {

/**
 * Get the most recent document FTDC collected from its periodic collectors.
 *
 * Document will be empty if FTDC has never run.
 */
class GetDiagnosticDataCommand final : public Command {
public:
    GetDiagnosticDataCommand() : Command("getDiagnosticData") {}

    bool adminOnly() const override {
        return true;
    }

    void help(std::stringstream& help) const override {
        help << "get latest diagnostic data collection snapshot";
    }

    bool slaveOk() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) override {

        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::serverStatus)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::replSetGetStatus)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString("local", "oplog.rs")),
                ActionType::collStats)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const std::string& db,
             const BSONObj& cmdObj,
             std::string& errmsg,
             BSONObjBuilder& result) override {

        result.append(
            "data",
            FTDCController::get(opCtx->getServiceContext())->getMostRecentPeriodicDocument());

        return true;
    }
};


Command* ftdcCommand;

MONGO_INITIALIZER(CreateDiagnosticDataCommand)(InitializerContext* context) {
    ftdcCommand = new GetDiagnosticDataCommand();

    return Status::OK();
}

/**
 * Get the content of an FTDC data file
 *
 * The command can dump the content of an ftdc archive file.
 *
 */
class GetDiagnosticDataFromFileCommand final : public Command {
public:
    GetDiagnosticDataFromFileCommand() : Command("getDiagnosticDataFromFile") {}

    bool adminOnly() const override {
        return true;
    }

    void help(std::stringstream& help) const override {
        help << "get diagnostic data from file";
    }

    bool slaveOk() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) override {

        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::serverStatus)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::replSetGetStatus)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString("local", "oplog.rs")),
                ActionType::collStats)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        return Status::OK();
    }

    bool run(OperationContext* txn,
             const std::string& db,
             BSONObj& cmdObj,
             int options,
             std::string& errmsg,
             BSONObjBuilder& result) override {

        /*
        * Handle the input parameters
        * filename : actually the filename of the archive we would like to handle
        * skip, limit : skip and limit as like any
        * showOutput: turn off data output generation to check filesize in records for example.
        * startDate: start date of data output generation
        * endDate: end date of data output generation
        */

        std::string dbPath;
        std::string filePath;

        long long skip, limit;
        bool showOutput;
        BSONElement startDateFilter;
        BSONElement endDateFilter;
        BSONObj fieldList;

        // TODO: this seems dangerous, what is when there is no storage key here? Is that possible?
        uassertStatusOK(bsonExtractStringFieldWithDefault(
            serverGlobalParams.parsedOpts.getObjectField("storage"),
            "dbPath",
            "/opt/data",
            &dbPath));

        uassertStatusOK(
            bsonExtractStringFieldWithDefault(cmdObj, "filename", "metrics.interim", &filePath));
        // TODO: check the value a bit better, not to have any bad affect.
        std::size_t foundPos = filePath.find('/');
        if (foundPos != std::string::npos) {
            result.append("errmsg", "/ not allowed in the filename");
            return false;
        }

        uassertStatusOK(
            bsonExtractBooleanFieldWithDefault(cmdObj, "showOutput", true, &showOutput));
        uassertStatusOK(bsonExtractIntegerFieldWithDefault(cmdObj, "skip", 0, &skip));
        uassertStatusOK(bsonExtractIntegerFieldWithDefault(cmdObj, "limit", 100, &limit));

        /*
        *   Extract dates
        *
        *   startDate  defaults to ISODate("1970-01-01T00:00:00Z")
        *   endDate    should be checked if it is ISODate("1970-01-01T00:00:00Z") should be set
        *              to Date.now() as nothing can be there for the future
        */

        Status statussdf = bsonExtractTypedField(cmdObj, "startDate", Date, &startDateFilter);
        if (!statussdf.isOK()) {
            bsonExtractTypedField(BSON("dateepoch" << Date_t::fromMillisSinceEpoch(0)),
                                  "dateepoch",
                                  Date,
                                  &startDateFilter);
        }

        Status statusedf = bsonExtractTypedField(cmdObj, "endDate", Date, &endDateFilter);
        if (!statusedf.isOK()) {
            bsonExtractTypedField(BSON("datenow" << DATENOW), "datenow", Date, &endDateFilter);
        }

        try {
            FTDCFileReader reader;
            std::stringstream ss;
            ss << dbPath << "/diagnostic.data/" << filePath;

            boost::filesystem::path p(ss.str());
            reader.open(p);

            int docsNumber = 0, matchedDocsNumber = 0;
            BSONArrayBuilder list;
            auto sw = reader.hasNext();
            while (sw.isOK() && sw.getValue() && (limit == 0 || matchedDocsNumber < (skip + limit))) {
                BSONObj oneEntry = std::get<1>(reader.next()).getOwned();
                Date_t entryEndDate = oneEntry.getField("end").Date();
                if (entryEndDate > startDateFilter.Date() &&
                    entryEndDate < endDateFilter.Date()) {
                    matchedDocsNumber++;
                    if (showOutput && matchedDocsNumber > skip) {
                        list.append(oneEntry);
                    }
                }
                sw = reader.hasNext();
                docsNumber++;
            }

            result.append("data", list.arr());
            result.append("numDocumentsRead", docsNumber);
            result.append("numDocumentsMatched", matchedDocsNumber);
            result.append("startDateFilter", startDateFilter.Date());
            result.append("endDateFilter", endDateFilter.Date());
            result.append("skip", skip);
            result.append("limit", limit);

        } catch (std::exception e) {
            result.append("errmsg", "Problem occured during the process");
            return false;
        }

        return true;
    }
};

Command* ftdcFromFileCommand;

MONGO_INITIALIZER(CreateDiagnosticDataFromFileCommand)(InitializerContext* context) {
    ftdcFromFileCommand = new GetDiagnosticDataFromFileCommand();

    return Status::OK();
}

/**
 * Get the list of FTDC archive files in diagnostic.data directory.
 *
 */
class GetDiagnosticDataFilesCommand final : public Command {
public:
    GetDiagnosticDataFilesCommand() : Command("getDiagnosticDataFiles") {}

    bool adminOnly() const override {
        return true;
    }

    void help(std::stringstream& help) const override {
        help << "get the list of diagnostic data archive files";
    }

    bool slaveOk() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) override {

        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::serverStatus)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::replSetGetStatus)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString("local", "oplog.rs")),
                ActionType::collStats)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        return Status::OK();
    }

    bool run(OperationContext* txn,
             const std::string& db,
             BSONObj& cmdObj,
             int options,
             std::string& errmsg,
             BSONObjBuilder& result) override {

        std::string dbPath;
        bsonExtractStringFieldWithDefault(serverGlobalParams.parsedOpts.getObjectField("storage"),
                                          "dbPath",
                                          "/opt/data",
                                          &dbPath);

        try {
            std::stringstream ss;
            ss << dbPath << "/diagnostic.data/";

            boost::filesystem::path p(ss.str());
            boost::filesystem::directory_iterator di(p);
            std::vector<std::string> files;

            for (; di != boost::filesystem::directory_iterator(); di++) {
                boost::filesystem::directory_entry& de = *di;
                std::string f = de.path().filename().string();
                files.emplace_back(f);
            }
            std::sort(files.begin(), files.end());

            BSONArrayBuilder list;

            for (auto f : files) {
                list.append(f);
            }

            result.append("data", list.arr());
        } catch (std::exception e) {
            result.append("errmsg", "Problem occured during the process");
            return false;
        }
        return true;
    }
};


Command* ftdcGetArchiveFilesCommand;

MONGO_INITIALIZER(CreateDiagnosticDataArchiveListCommand)(InitializerContext* context) {
    ftdcGetArchiveFilesCommand = new GetDiagnosticDataFilesCommand();

    return Status::OK();
}

}  // namespace

}  // namespace mongo
