/* dbwebserver.cpp

   This is the administrative web page displayed on port 28017.
*/

/**
*    Copyright (C) 2008 10gen Inc.
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

#include "mongo/db/dbwebserver.h"

#include <pcrecpp.h>

#include "mongo/base/init.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/background.h"
#include "mongo/db/commands.h"
#include "mongo/db/db.h"
#include "mongo/db/instance.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/snapshots.h"
#include "mongo/rpc/command_reply.h"
#include "mongo/rpc/command_reply_builder.h"
#include "mongo/rpc/command_request.h"
#include "mongo/rpc/command_request_builder.h"
#include "mongo/rpc/metadata.h"
#include "mongo/util/admin_access.h"
#include "mongo/util/md5.hpp"
#include "mongo/util/mongoutils/html.h"
#include "mongo/util/ramlog.h"
#include "mongo/util/version.h"

namespace mongo {

using std::map;
using std::stringstream;
using std::vector;

using namespace html;

namespace {

void doUnlockedStuff(stringstream& ss) {
    // This is in the header already ss << "port:      " << port << '\n'
    ss << "<pre>";
    ss << mongodVersion() << '\n';
    ss << "git hash: " << gitVersion() << '\n';
    ss << openSSLVersion("OpenSSL version: ", "\n");
    ss << "uptime: " << time(0) - serverGlobalParams.started << " seconds\n";
    ss << "</pre>";
}

bool prisort(const Prioritizable* a, const Prioritizable* b) {
    return a->priority() < b->priority();
}

void htmlHelp(Command* command, stringstream& ss) {
    string helpStr;
    {
        stringstream h;
        command->help(h);
        helpStr = h.str();
    }

    ss << "\n<tr><td>";
    if (command->isWebUI())
        ss << "<a href=\"/" << command->getName() << "?text=1\">";
    ss << command->getName();
    if (command->isWebUI())
        ss << "</a>";
    ss << "</td>\n";
    ss << "<td>";
    ss << "UNUSED ";
    if (command->slaveOk())
        ss << "S ";
    if (command->adminOnly())
        ss << "A";
    ss << "</td>";
    ss << "<td>";
    if (helpStr != "no help defined") {
        const char* p = helpStr.c_str();
        while (*p) {
            if (*p == '<') {
                ss << "&lt;";
                p++;
                continue;
            } else if (*p == '{')
                ss << "<code>";
            else if (*p == '}') {
                ss << "}</code>";
                p++;
                continue;
            }
            if (strncmp(p, "http:", 5) == 0) {
                ss << "<a href=\"";
                const char* q = p;
                while (*q && *q != ' ' && *q != '\n')
                    ss << *q++;
                ss << "\">";
                q = p;
                if (str::startsWith(q, "http://www.mongodb.org/display/"))
                    q += 31;
                while (*q && *q != ' ' && *q != '\n') {
                    ss << (*q == '+' ? ' ' : *q);
                    q++;
                    if (*q == '#')
                        while (*q && *q != ' ' && *q != '\n')
                            q++;
                }
                ss << "</a>";
                p = q;
                continue;
            }
            if (*p == '\n')
                ss << "<br>";
            else
                ss << *p;
            p++;
        }
    }
    ss << "</td>";
    ss << "</tr>\n";
}

class LogPlugin : public WebStatusPlugin {
public:
    LogPlugin() : WebStatusPlugin("Log", 100), _log(0) {
        _log = RamLog::get("global");
    }

    virtual void init() {}

    virtual void run(OperationContext* txn, stringstream& ss) {
        _log->toHTML(ss);
    }
    RamLog* _log;
};

class FavIconHandler : public DbWebHandler {
public:
    FavIconHandler() : DbWebHandler("favicon.ico", 0, false) {}

    virtual void handle(OperationContext* txn,
                        const char* rq,
                        const std::string& url,
                        BSONObj params,
                        string& responseMsg,
                        int& responseCode,
                        vector<string>& headers,
                        const SockAddr& from) {
        responseCode = 404;
        headers.push_back("Content-Type: text/plain;charset=utf-8");
        responseMsg = "no favicon\n";
    }

} faviconHandler;

class StatusHandler : public DbWebHandler {
public:
    StatusHandler() : DbWebHandler("_status", 1, false) {}

    virtual void handle(OperationContext* txn,
                        const char* rq,
                        const std::string& url,
                        BSONObj params,
                        string& responseMsg,
                        int& responseCode,
                        vector<string>& headers,
                        const SockAddr& from) {
        headers.push_back("Content-Type: application/json;charset=utf-8");
        responseCode = 200;

        static vector<string> commands;
        if (commands.size() == 0) {
            commands.push_back("serverStatus");
            commands.push_back("buildinfo");
        }

        BSONObjBuilder buf(1024);

        for (unsigned i = 0; i < commands.size(); i++) {
            string cmd = commands[i];

            Command* c = Command::findCommand(cmd);
            verify(c);

            BSONObj co;
            {
                BSONObjBuilder b;
                b.append(cmd, 1);

                if (cmd == "serverStatus" && params["repl"].type()) {
                    b.append("repl", atoi(params["repl"].valuestr()));
                }

                co = b.obj();
            }

            string errmsg;

            BSONObjBuilder sub;
            if (!c->run(txn, "admin.$cmd", co, 0, errmsg, sub))
                buf.append(cmd, errmsg);
            else
                buf.append(cmd, sub.obj());
        }

        responseMsg = buf.obj().jsonString();
    }

} statusHandler;

class CommandListHandler : public DbWebHandler {
public:
    CommandListHandler() : DbWebHandler("_commands", 1, true) {}

    virtual void handle(OperationContext* txn,
                        const char* rq,
                        const std::string& url,
                        BSONObj params,
                        string& responseMsg,
                        int& responseCode,
                        vector<string>& headers,
                        const SockAddr& from) {
        headers.push_back("Content-Type: text/html;charset=utf-8");
        responseCode = 200;

        stringstream ss;
        ss << start("Commands List");
        ss << p(a("/", "back", "Home"));
        ss << p(
            "<b>MongoDB List of "
            "<a href=\"http://dochub.mongodb.org/core/commands\">Commands</a>"
            "</b>\n");

        const Command::CommandMap* m = Command::commandsByBestName();
        ss << "S:slave-ok  R:read-lock  W:write-lock  A:admin-only<br>\n";
        ss << table();
        ss << "<tr><th>Command</th><th>Attributes</th><th>Help</th></tr>\n";
        for (Command::CommandMap::const_iterator i = m->begin(); i != m->end(); ++i) {
            htmlHelp(i->second, ss);
        }
        ss << _table() << _end();

        responseMsg = ss.str();
    }
} commandListHandler;

class CommandsHandler : public DbWebHandler {
public:
    CommandsHandler() : DbWebHandler("DUMMY COMMANDS", 2, true) {}

    bool _cmd(const string& url, string& cmd, bool& text, bo params) const {
        cmd = str::after(url, '/');
        text = params["text"].boolean();
        return true;
    }

    Command* _cmd(const string& cmdName) const {
        Command* cmd = Command::findCommand(cmdName);
        if (cmd && cmd->isWebUI()) {
            return cmd;
        }

        return nullptr;
    }

    virtual bool handles(const string& url) const {
        string cmd;
        bool text;
        if (!_cmd(url, cmd, text, bo()))
            return false;
        return _cmd(cmd) != 0;
    }

    virtual void handle(OperationContext* txn,
                        const char* rq,
                        const std::string& url,
                        BSONObj params,
                        string& responseMsg,
                        int& responseCode,
                        vector<string>& headers,
                        const SockAddr& from) {
        string cmd;
        bool text = false;
        verify(_cmd(url, cmd, text, params));
        Command* c = _cmd(cmd);
        verify(c);

        BSONObj cmdObj = BSON(cmd << 1);

        rpc::CommandRequestBuilder requestBuilder{};

        requestBuilder.setDatabase("admin").setCommandName(cmd).setCommandArgs(cmdObj).setMetadata(
            rpc::makeEmptyMetadata());

        auto cmdRequestMsg = requestBuilder.done();
        rpc::CommandRequest cmdRequest{&cmdRequestMsg};
        rpc::CommandReplyBuilder cmdReplyBuilder{};

        Command::execCommand(txn, c, cmdRequest, &cmdReplyBuilder);

        auto cmdReplyMsg = cmdReplyBuilder.done();
        rpc::CommandReply cmdReply{&cmdReplyMsg};

        responseCode = 200;

        string j = cmdReply.getCommandReply().jsonString(Strict, text);
        responseMsg = j;

        if (text) {
            headers.push_back("Content-Type: text/plain;charset=utf-8");
            responseMsg += '\n';
        } else {
            headers.push_back("Content-Type: application/json;charset=utf-8");
        }
    }

} commandsHandler;


MONGO_INITIALIZER(WebStatusLogPlugin)(InitializerContext*) {
    if (serverGlobalParams.isHttpInterfaceEnabled) {
        new LogPlugin;
    }
    return Status::OK();
}

}  // namespace


DbWebServer::DbWebServer(const string& ip, int port, ServiceContext* ctx, AdminAccess* webUsers)
    : MiniWebServer("admin web console", ip, port, ctx), _webUsers(webUsers) {
    WebStatusPlugin::initAll();
}

void DbWebServer::doRequest(const char* rq,
                            string url,
                            string& responseMsg,
                            int& responseCode,
                            vector<string>& headers,
                            const SockAddr& from) {
    Client* client = &cc();
    auto txn = client->makeOperationContext();

    if (url.size() > 1) {
        if (!_allowed(txn.get(), rq, headers, from)) {
            responseCode = 401;
            headers.push_back("Content-Type: text/plain;charset=utf-8");
            responseMsg = "not allowed\n";
            return;
        }

        {
            BSONObj params;
            const size_t pos = url.find("?");
            if (pos != string::npos) {
                MiniWebServer::parseParams(params, url.substr(pos + 1));
                url = url.substr(0, pos);
            }

            DbWebHandler* handler = DbWebHandler::findHandler(url);
            if (handler) {
                if (handler->requiresREST(url) && !serverGlobalParams.rest) {
                    _rejectREST(responseMsg, responseCode, headers);
                } else {
                    const string callback = params.getStringField("jsonp");

                    uassert(13453,
                            "server not started with --jsonp",
                            callback.empty() || serverGlobalParams.jsonp);

                    handler->handle(
                        txn.get(), rq, url, params, responseMsg, responseCode, headers, from);

                    if (responseCode == 200 && !callback.empty()) {
                        responseMsg = callback + '(' + responseMsg + ')';
                    }
                }

                return;
            }
        }

        if (!serverGlobalParams.rest) {
            _rejectREST(responseMsg, responseCode, headers);
            return;
        }

        responseCode = 404;
        headers.push_back("Content-Type: text/html;charset=utf-8");
        responseMsg = "<html><body>unknown url</body></html>\n";
        return;
    }

    // generate home page

    if (!_allowed(txn.get(), rq, headers, from)) {
        responseCode = 401;
        headers.push_back("Content-Type: text/plain;charset=utf-8");
        responseMsg = "not allowed\n";
        return;
    }

    responseCode = 200;
    stringstream ss;
    string dbname;
    {
        stringstream z;
        z << serverGlobalParams.binaryName << ' ' << prettyHostName();
        dbname = z.str();
    }

    ss << start(dbname) << h2(dbname);
    ss << "<p><a href=\"/_commands\">List all commands</a> | \n";
    ss << "<a href=\"/_replSet\">Replica set status</a></p>\n";

    ss << a("",
            "These read-only context-less commands can be executed from the web "
            "interface. Results are json format, unless ?text=1 is appended in which "
            "case the result is output as text for easier human viewing",
            "Commands")
       << ": ";

    auto m = Command::commandsByBestName();

    for (Command::CommandMap::const_iterator i = m->begin(); i != m->end(); ++i) {
        if (!i->second->isWebUI())
            continue;

        stringstream h;
        i->second->help(h);

        const string help = h.str();
        ss << "<a href=\"/" << i->first << "?text=1\"";
        if (help != "no help defined") {
            ss << " title=\"" << help << '"';
        }

        ss << ">" << i->first << "</a> ";
    }

    ss << '\n';

    doUnlockedStuff(ss);

    WebStatusPlugin::runAll(txn.get(), ss);

    ss << "</body></html>\n";
    responseMsg = ss.str();
    headers.push_back("Content-Type: text/html;charset=utf-8");
}

bool DbWebServer::_allowed(OperationContext* txn,
                           const char* rq,
                           vector<string>& headers,
                           const SockAddr& from) {
    AuthorizationSession* authSess = AuthorizationSession::get(txn->getClient());
    if (!authSess->getAuthorizationManager().isAuthEnabled()) {
        return true;
    }

    if (from.isLocalHost() && !_webUsers->haveAdminUsers(txn)) {
        authSess->grantInternalAuthorization();
        return true;
    }

    string auth = getHeader(rq, "Authorization");

    if (auth.size() > 0 && auth.find("Digest ") == 0) {
        auth = auth.substr(7) + ", ";

        map<string, string> parms;
        pcrecpp::StringPiece input(auth);

        string name, val;
        pcrecpp::RE re("(\\w+)=\"?(.*?)\"?,\\s*");
        while (re.Consume(&input, &name, &val)) {
            parms[name] = val;
        }

        // Only users in the admin DB are visible by the webserver
        UserName userName(parms["username"], "admin");
        User* user;
        AuthorizationManager& authzManager = authSess->getAuthorizationManager();
        Status status = authzManager.acquireUser(txn, userName, &user);
        if (!status.isOK()) {
            if (status.code() != ErrorCodes::UserNotFound) {
                uasserted(17051, status.reason());
            }
        } else {
            uassert(
                17090, "External users don't have a password", !user->getCredentials().isExternal);

            string ha1 = user->getCredentials().password;
            authzManager.releaseUser(user);
            if (ha1.empty()) {
                return false;
            }

            const string ha2 = md5simpledigest((string) "GET" + ":" + parms["uri"]);

            stringstream r;
            r << ha1 << ':' << parms["nonce"];
            if (parms["nc"].size() && parms["cnonce"].size() && parms["qop"].size()) {
                r << ':';
                r << parms["nc"];
                r << ':';
                r << parms["cnonce"];
                r << ':';
                r << parms["qop"];
            }
            r << ':';
            r << ha2;

            const string r1 = md5simpledigest(r.str());

            if (r1 == parms["response"]) {
                Status status = authSess->addAndAuthorizeUser(txn, userName);
                uassertStatusOK(status);
                return true;
            }
        }
    }

    stringstream authHeader;
    authHeader << "WWW-Authenticate: "
               << "Digest realm=\"mongo\", "
               << "nonce=\"abc\", "
               << "algorithm=MD5, qop=\"auth\" ";

    headers.push_back(authHeader.str());
    return 0;
}

void DbWebServer::_rejectREST(string& responseMsg, int& responseCode, vector<string>& headers) {
    responseCode = 403;
    stringstream ss;
    ss << "REST is not enabled.  use --rest to turn on.\n";
    ss << "check that port " << _port << " is secured for the network too.\n";
    responseMsg = ss.str();
    headers.push_back("Content-Type: text/plain;charset=utf-8");
}


// -- status framework ---
WebStatusPlugin::WebStatusPlugin(const string& secionName, double priority, const string& subheader)
    : Prioritizable(priority), _name(secionName), _subHeading(subheader) {
    if (!_plugins)
        _plugins = new vector<WebStatusPlugin*>();
    _plugins->push_back(this);
}

void WebStatusPlugin::initAll() {
    if (!_plugins)
        return;

    sort(_plugins->begin(), _plugins->end(), prisort);

    for (unsigned i = 0; i < _plugins->size(); i++)
        (*_plugins)[i]->init();
}

void WebStatusPlugin::runAll(OperationContext* txn, stringstream& ss) {
    if (!_plugins)
        return;

    for (unsigned i = 0; i < _plugins->size(); i++) {
        WebStatusPlugin* p = (*_plugins)[i];
        ss << "<hr>\n"
           << "<b>" << p->_name << "</b>";

        ss << " " << p->_subHeading;

        ss << "<br>\n";

        p->run(txn, ss);
    }
}

vector<WebStatusPlugin*>* WebStatusPlugin::_plugins = 0;


DbWebHandler::DbWebHandler(const string& name, double priority, bool requiresREST)
    : Prioritizable(priority), _name(name), _requiresREST(requiresREST) {
    {
        // setup strings
        _defaultUrl = "/";
        _defaultUrl += name;

        stringstream ss;
        ss << name << " priority: " << priority << " rest: " << requiresREST;
        _toString = ss.str();
    }

    {
        // add to handler list
        if (!_handlers)
            _handlers = new vector<DbWebHandler*>();
        _handlers->push_back(this);
        sort(_handlers->begin(), _handlers->end(), prisort);
    }
}

DbWebHandler* DbWebHandler::findHandler(const string& url) {
    if (!_handlers)
        return 0;

    for (unsigned i = 0; i < _handlers->size(); i++) {
        DbWebHandler* h = (*_handlers)[i];
        if (h->handles(url))
            return h;
    }

    return 0;
}

vector<DbWebHandler*>* DbWebHandler::_handlers = 0;

void webServerListenThread(std::shared_ptr<DbWebServer> dbWebServer) {
    Client::initThread("websvr");

    dbWebServer->initAndListen();
}

}  // namespace mongo
