/**
*    Copyright (C) 2009 10gen Inc.
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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/global_environment_experiment.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/dbwebserver.h"
#include "mongo/util/mongoutils/html.h"
#include "mongo/util/stringutils.h"


namespace mongo {

    class OperationsDataBuilder : public GlobalEnvironmentExperiment::ProcessOperationContext {
    public:
        OperationsDataBuilder(std::stringstream& stringStream)
            : _stringStream(stringStream) {

        }

        virtual void processOpContext(OperationContext* txn) {
            using namespace html;

            CurOp& co = *(txn->getCurOp());

            _stringStream << "<tr><td>" << txn->getClient()->desc() << "</td>";

            tablecell(_stringStream, co.opNum());
            tablecell(_stringStream, co.active());
            tablecell(_stringStream, txn->lockState()->reportState());
            if (co.active()) {
                tablecell(_stringStream, co.elapsedSeconds());
            }
            else {
                tablecell(_stringStream, "");
            }

            tablecell(_stringStream, co.getOp());
            tablecell(_stringStream, html::escape(co.getNS()));
            if (co.haveQuery()) {
                tablecell(_stringStream, html::escape(co.query().toString()));
            }
            else {
                tablecell(_stringStream, "");
            }

            tablecell(_stringStream, co.getRemoteString());

            tablecell(_stringStream, co.getMessage());
            tablecell(_stringStream, co.getProgressMeter().toString());

            _stringStream << "</tr>\n";
        }

    private:
        std::stringstream& _stringStream;
    };

namespace {
    class ClientListPlugin : public WebStatusPlugin {
    public:
        ClientListPlugin() : WebStatusPlugin( "clients" , 20 ) {}
        virtual void init() {}

        virtual void run(OperationContext* txn, std::stringstream& ss ) {
            using namespace html;

            ss << "\n<table border=1 cellpadding=2 cellspacing=0>";
            ss << "<tr align='left'>"
               << th( a("", "Connections to the database, both internal and external.", "Client") )
               << th( a("http://dochub.mongodb.org/core/viewingandterminatingcurrentoperation", "", "OpId") )
               << "<th>Locking</th>"
               << "<th>Waiting</th>"
               << "<th>SecsRunning</th>"
               << "<th>Op</th>"
               << th( a("http://dochub.mongodb.org/core/whatisanamespace", "", "Namespace") )
               << "<th>Query</th>"
               << "<th>client</th>"
               << "<th>msg</th>"
               << "<th>progress</th>"

               << "</tr>\n";
            
            OperationsDataBuilder opCtxDataBuilder(ss);
            getGlobalEnvironment()->forEachOperationContext(&opCtxDataBuilder);
            
            ss << "</table>\n";

        }

    } clientListPlugin;

    class CommandHelper : public GlobalEnvironmentExperiment::ProcessOperationContext {
    public:
        virtual void processOpContext(OperationContext* txn) {
            BSONObjBuilder b;
            if ( txn->getClient() )
                txn->getClient()->reportState( b );
            if ( txn->getCurOp() )
                txn->getCurOp()->reportState( &b );
            if ( txn->lockState() ) {
                StringBuilder ss;
                ss << txn->lockState();
                b.append( "lockStatePointer", ss.str() );
                b.append( "lockState", txn->lockState()->reportState() );
            }
            if ( txn->recoveryUnit() )
                txn->recoveryUnit()->reportState( &b );
            array.append( b.obj() );
        }

        BSONArrayBuilder array;
    };

    class CurrentOpContexts : public Command {
    public:
        CurrentOpContexts() : Command( "currentOpCtx" ) { }

        virtual bool isWriteCommandForConfigServer() const { return false; }

        virtual bool slaveOk() const { return true; }

        bool run( OperationContext* txn,
                  const string& dbname,
                  BSONObj& cmdObj,
                  int,
                  string& errmsg,
                  BSONObjBuilder& result,
                  bool fromRepl) {

            CommandHelper helper;
            getGlobalEnvironment()->forEachOperationContext(&helper);

            result.appendArray( "operations", helper.array.arr() );

            return true;
        }
    } currentOpContexts;

}  // namespace
}  // namespace mongo
