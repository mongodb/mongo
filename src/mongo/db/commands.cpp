/* commands.cpp
   db "commands" (sent via db.$cmd.findOne(...))
 */

/*    Copyright 2009 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/commands.h"

#include <string>
#include <vector>

#include "mongo/bson/mutable/document.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/client.h"
#include "mongo/db/get_status_from_command_result.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/server_parameters.h"
#include "mongo/util/log.h"

namespace mongo {

    using logger::LogComponent;

    Command::CommandMap* Command::_commandsByBestName;
    Command::CommandMap* Command::_webCommands;
    Command::CommandMap* Command::_commands;

    int Command::testCommandsEnabled = 0;

    Counter64 Command::unknownCommands;
    static ServerStatusMetricField<Counter64> displayUnknownCommands( "commands.<UNKNOWN>",
        &Command::unknownCommands );

    namespace {
        ExportedServerParameter<int> testCommandsParameter(ServerParameterSet::getGlobal(),
                                                           "enableTestCommands",
                                                           &Command::testCommandsEnabled,
                                                           true,
                                                           false);
    }

    string Command::parseNsFullyQualified(const string& dbname, const BSONObj& cmdObj) const {
        BSONElement first = cmdObj.firstElement();
        uassert(17005,
                mongoutils::str::stream() << "Main argument to " << first.fieldNameStringData() <<
                        " must be a fully qualified namespace string.  Found: " <<
                        first.toString(false),
                first.type() == mongo::String &&
                NamespaceString::validCollectionComponent(first.valuestr()));
        return first.String();
    }

    string Command::parseNsCollectionRequired(const string& dbname, const BSONObj& cmdObj) const {
        // Accepts both BSON String and Symbol for collection name per SERVER-16260
        // TODO(kangas) remove Symbol support in MongoDB 3.0 after Ruby driver audit
        BSONElement first = cmdObj.firstElement();
        uassert(17009,
                "no collection name specified",
                first.canonicalType() == canonicalizeBSONType(mongo::String)
                && first.valuestrsize() > 0);
        std::string coll = first.valuestr();
        return dbname + '.' + coll;
    }

    /*virtual*/ string Command::parseNs(const string& dbname, const BSONObj& cmdObj) const {
        BSONElement first = cmdObj.firstElement();
        if (first.type() != mongo::String)
            return dbname;

        string coll = cmdObj.firstElement().valuestr();
#if defined(CLC)
        DEV if( mongoutils::str::startsWith(coll, dbname+'.') ) { 
            log() << "DEBUG parseNs Command's collection name looks like it includes the db name\n"
                << dbname << '\n' 
                << coll << '\n'
                << cmdObj.toString() << endl;
            dassert(false);
        }
#endif
        return dbname + '.' + coll;
    }

    ResourcePattern Command::parseResourcePattern(const std::string& dbname,
                                                  const BSONObj& cmdObj) const {
        std::string ns = parseNs(dbname, cmdObj);
        if (ns.find('.') == std::string::npos) {
            return ResourcePattern::forDatabaseName(ns);
        }
        return ResourcePattern::forExactNamespace(NamespaceString(ns));
    }

    void Command::htmlHelp(stringstream& ss) const {
        string helpStr;
        {
            stringstream h;
            help(h);
            helpStr = h.str();
        }
        ss << "\n<tr><td>";
        bool web = _webCommands->find(name) != _webCommands->end();
        if( web ) ss << "<a href=\"/" << name << "?text=1\">";
        ss << name;
        if( web ) ss << "</a>";
        ss << "</td>\n";
        ss << "<td>";
        if (isWriteCommandForConfigServer()) { 
            ss << "W "; 
        }
        else { 
            ss << "R "; 
        }
        if( slaveOk() )
            ss << "S ";
        if( adminOnly() )
            ss << "A";
        ss << "</td>";
        ss << "<td>";
        if( helpStr != "no help defined" ) {
            const char *p = helpStr.c_str();
            while( *p ) {
                if( *p == '<' ) {
                    ss << "&lt;";
                    p++; continue;
                }
                else if( *p == '{' )
                    ss << "<code>";
                else if( *p == '}' ) {
                    ss << "}</code>";
                    p++;
                    continue;
                }
                if( strncmp(p, "http:", 5) == 0 ) {
                    ss << "<a href=\"";
                    const char *q = p;
                    while( *q && *q != ' ' && *q != '\n' )
                        ss << *q++;
                    ss << "\">";
                    q = p;
                    if( str::startsWith(q, "http://www.mongodb.org/display/") )
                        q += 31;
                    while( *q && *q != ' ' && *q != '\n' ) {
                        ss << (*q == '+' ? ' ' : *q);
                        q++;
                        if( *q == '#' )
                            while( *q && *q != ' ' && *q != '\n' ) q++;
                    }
                    ss << "</a>";
                    p = q;
                    continue;
                }
                if( *p == '\n' ) ss << "<br>";
                else ss << *p;
                p++;
            }
        }
        ss << "</td>";
        ss << "</tr>\n";
    }

    Command::Command(StringData _name, bool web, StringData oldName) :
        name(_name.toString()),
        _commandsExecutedMetric("commands."+ _name.toString()+".total", &_commandsExecuted),
        _commandsFailedMetric("commands."+ _name.toString()+".failed", &_commandsFailed) {
        // register ourself.
        if ( _commands == 0 )
            _commands = new CommandMap();
        if( _commandsByBestName == 0 )
            _commandsByBestName = new CommandMap();
        Command*& c = (*_commands)[name];
        if ( c )
            log() << "warning: 2 commands with name: " << _name << endl;
        c = this;
        (*_commandsByBestName)[name] = this;

        if( web ) {
            if( _webCommands == 0 )
                _webCommands = new CommandMap();
            (*_webCommands)[name] = this;
        }

        if( !oldName.empty() )
            (*_commands)[oldName.toString()] = this;
    }

    void Command::help( stringstream& help ) const {
        help << "no help defined";
    }

    std::vector<BSONObj> Command::stopIndexBuilds(OperationContext* opCtx,
                                                  Database* db,
                                                  const BSONObj& cmdObj) {
        return std::vector<BSONObj>();
    }

    Command* Command::findCommand( const StringData& name ) {
        CommandMap::const_iterator i = _commands->find( name );
        if ( i == _commands->end() )
            return 0;
        return i->second;
    }

    bool Command::appendCommandStatus(BSONObjBuilder& result, const Status& status) {
        appendCommandStatus(result, status.isOK(), status.reason());
        BSONObj tmp = result.asTempObj();
        if (!status.isOK() && !tmp.hasField("code")) {
            result.append("code", status.code());
        }
        return status.isOK();
    }

    void Command::appendCommandStatus(BSONObjBuilder& result, bool ok, const std::string& errmsg) {
        BSONObj tmp = result.asTempObj();
        bool have_ok = tmp.hasField("ok");
        bool have_errmsg = tmp.hasField("errmsg");

        if (!have_ok)
            result.append( "ok" , ok ? 1.0 : 0.0 );

        if (!ok && !have_errmsg) {
            result.append("errmsg", errmsg);
        }
    }

    Status Command::getStatusFromCommandResult(const BSONObj& result) {
        return mongo::getStatusFromCommandResult(result);
    }

    Status Command::parseCommandCursorOptions(const BSONObj& cmdObj,
                                              long long defaultBatchSize,
                                              long long* batchSize) {
        invariant(batchSize);
        *batchSize = defaultBatchSize;

        BSONElement cursorElem = cmdObj["cursor"];
        if (cursorElem.eoo()) {
            return Status::OK();
        }

        if (cursorElem.type() != mongo::Object) {
            return Status(ErrorCodes::TypeMismatch, "cursor field must be missing or an object");
        }

        BSONObj cursor = cursorElem.embeddedObject();
        BSONElement batchSizeElem = cursor["batchSize"];
        if (batchSizeElem.eoo()) {
            if (!cursor.isEmpty()) {
                return Status(ErrorCodes::BadValue,
                              "cursor object can't contain fields other than batchSize");
            }

            return Status::OK();
        }

        if (!batchSizeElem.isNumber()) {
            return Status(ErrorCodes::TypeMismatch, "cursor.batchSize must be a number");
        }

        // This can change in the future, but for now all negatives are reserved.
        if (batchSizeElem.numberLong() < 0) {
            return Status(ErrorCodes::BadValue, "cursor.batchSize must not be negative");
        }

        *batchSize = batchSizeElem.numberLong();

        return Status::OK();
    }

    void Command::appendCursorResponseObject(long long cursorId,
                                             StringData cursorNamespace,
                                             BSONArray firstBatch,
                                             BSONObjBuilder* builder) {
        BSONObjBuilder cursorObj(builder->subobjStart("cursor"));
        cursorObj.append("id", cursorId);
        cursorObj.append("ns", cursorNamespace);
        cursorObj.append("firstBatch", firstBatch);
        cursorObj.done();
    }

    Status Command::checkAuthForCommand(ClientBasic* client,
                                        const std::string& dbname,
                                        const BSONObj& cmdObj) {
        std::vector<Privilege> privileges;
        this->addRequiredPrivileges(dbname, cmdObj, &privileges);
        if (client->getAuthorizationSession()->isAuthorizedForPrivileges(privileges))
            return Status::OK();
        return Status(ErrorCodes::Unauthorized, "unauthorized");
    }

    void Command::redactForLogging(mutablebson::Document* cmdObj) {}

    BSONObj Command::getRedactedCopyForLogging(const BSONObj& cmdObj) {
        namespace mmb = mutablebson;
        mmb::Document cmdToLog(cmdObj, mmb::Document::kInPlaceDisabled);
        redactForLogging(&cmdToLog);
        BSONObjBuilder bob;
        cmdToLog.writeTo(&bob);
        return bob.obj();
    }

    void Command::logIfSlow( const Timer& timer, const string& msg ) {
        int ms = timer.millis();
        if (ms > serverGlobalParams.slowMS) {
            log() << msg << " took " << ms << " ms." << endl;
        }
    }

    static Status _checkAuthorizationImpl(Command* c,
                                          ClientBasic* client,
                                          const std::string& dbname,
                                          const BSONObj& cmdObj,
                                          bool fromRepl) {
        namespace mmb = mutablebson;
        if ( c->adminOnly() && ! fromRepl && dbname != "admin" ) {
            return Status(ErrorCodes::Unauthorized, str::stream() << c->name <<
                          " may only be run against the admin database.");
        }
        if (client->getAuthorizationSession()->getAuthorizationManager().isAuthEnabled()) {
            Status status = c->checkAuthForCommand(client, dbname, cmdObj);
            if (status == ErrorCodes::Unauthorized) {
                mmb::Document cmdToLog(cmdObj, mmb::Document::kInPlaceDisabled);
                c->redactForLogging(&cmdToLog);
                return Status(ErrorCodes::Unauthorized,
                              str::stream() << "not authorized on " << dbname <<
                              " to execute command " << cmdToLog.toString());
            }
            if (!status.isOK()) {
                return status;
            }
        }
        else if (c->adminOnly() &&
                 c->localHostOnlyIfNoAuth(cmdObj) &&
                 !client->getIsLocalHostConnection()) {

            return Status(ErrorCodes::Unauthorized, str::stream() << c->name <<
                          " must run from localhost when running db without auth");
        }
        return Status::OK();
    }

    Status Command::_checkAuthorization(Command* c,
                                        ClientBasic* client,
                                        const std::string& dbname,
                                        const BSONObj& cmdObj,
                                        bool fromRepl) {
        namespace mmb = mutablebson;
        Status status = _checkAuthorizationImpl(c, client, dbname, cmdObj, fromRepl);
        if (!status.isOK()) {
            log(LogComponent::kAccessControl) << status << std::endl;
        }
        audit::logCommandAuthzCheck(client,
                                    dbname,
                                    cmdObj,
                                    c,
                                    status.code());
        return status;
    }
}

#include "mongo/client/connpool.h"

namespace mongo {

    extern DBConnectionPool pool;
    // This is mainly used by the internal writes using write commands.
    extern DBConnectionPool shardConnectionPool;

    class PoolFlushCmd : public Command {
    public:
        PoolFlushCmd() : Command( "connPoolSync" , false , "connpoolsync" ) {}
        virtual void help( stringstream &help ) const { help<<"internal"; }
        virtual bool isWriteCommandForConfigServer() const { return false; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::connPoolSync);
            out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
        }

        virtual bool run(OperationContext* txn, const string&, mongo::BSONObj&, int, std::string&, mongo::BSONObjBuilder& result, bool) {
            shardConnectionPool.flush();
            pool.flush();
            return true;
        }
        virtual bool slaveOk() const {
            return true;
        }

    } poolFlushCmd;

    class PoolStats : public Command {
    public:
        PoolStats() : Command( "connPoolStats" ) {}
        virtual void help( stringstream &help ) const { help<<"stats about connection pool"; }
        virtual bool isWriteCommandForConfigServer() const { return false; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::connPoolStats);
            out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
        }
        virtual bool run(OperationContext* txn, const string&, mongo::BSONObj&, int, std::string&, mongo::BSONObjBuilder& result, bool) {
            pool.appendInfo( result );
            result.append( "numDBClientConnection" , DBClientConnection::getNumConnections() );
            result.append( "numAScopedConnection" , AScopedConnection::getNumConnections() );
            return true;
        }
        virtual bool slaveOk() const {
            return true;
        }

    } poolStatsCmd;

} // namespace mongo
