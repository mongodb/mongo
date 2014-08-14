// validate.cpp

/**
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommands

#include "mongo/platform/basic.h"

#include "mongo/db/commands.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/util/log.h"

namespace mongo {

    class ValidateCmd : public Command {
    public:
        ValidateCmd() : Command( "validate" ) {}

        virtual bool slaveOk() const {
            return true;
        }

        virtual void help(stringstream& h) const { h << "Validate contents of a namespace by scanning its data structures for correctness.  Slow.\n"
                                                        "Add full:true option to do a more thorough check"; }

        virtual bool isWriteCommandForConfigServer() const { return false; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::validate);
            out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
        }
        //{ validate: "collectionnamewithoutthedbpart" [, scandata: <bool>] [, full: <bool> } */

        bool run(OperationContext* txn, const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            string ns = dbname + "." + cmdObj.firstElement().valuestrsafe();

            NamespaceString ns_string(ns);
            const bool full = cmdObj["full"].trueValue();
            const bool scanData = full || cmdObj["scandata"].trueValue();

            if ( !ns_string.isNormal() && full ) {
                errmsg = "Can only run full validate on a regular collection";
                return false;
            }

            if (!serverGlobalParams.quiet) {
                LOG(0) << "CMD: validate " << ns << endl;
            }

            Client::ReadContext ctx(txn, ns_string.ns());

            Database* db = ctx.ctx().db();
            if ( !db ) {
                errmsg = "database not found";
                return false;
            }

            Collection* collection = db->getCollection( txn, ns );
            if ( !collection ) {
                errmsg = "collection not found";
                return false;
            }

            result.append( "ns", ns );

            ValidateResults results;
            Status status = collection->validate( txn, full, scanData, &results, &result );
            if ( !status.isOK() )
                return appendCommandStatus( result, status );

            result.appendBool("valid", results.valid);
            result.append("errors", results.errors);

            if ( !full ){
                result.append("warning", "Some checks omitted for speed. use {full:true} option to do more thorough scan.");
            }

            if ( !results.valid ) {
                result.append("advice", "ns corrupt. See http://dochub.mongodb.org/core/data-recovery");
            }

            return true;
        }

    } validateCmd;

}
