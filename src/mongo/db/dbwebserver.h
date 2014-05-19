/** @file dbwebserver.h
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

#include <string>
#include <vector>

#include "mongo/util/admin_access.h"
#include "mongo/util/net/sock.h"
#include "mongo/db/operation_context.h"

namespace mongo {

    class Prioritizable {
    public:
        Prioritizable( double p ) : _priority(p) {}
        double priority() const { return _priority; }
    private:
        double _priority;
    };

    class DbWebHandler : public Prioritizable {
    public:
        DbWebHandler( const std::string& name , double priority , bool requiresREST );
        virtual ~DbWebHandler() {}

        virtual bool handles( const std::string& url ) const { return url == _defaultUrl; }

        virtual bool requiresREST( const std::string& url ) const { return _requiresREST; }

        virtual void handle( OperationContext* txn,
                             const char *rq, // the full request
                             const std::string& url,
                             BSONObj params,
                             // set these and return them:
                             std::string& responseMsg,
                             int& responseCode,
                             std::vector<std::string>& headers, // if completely empty, content-type: text/html will be added
                             const SockAddr &from
                           ) = 0;

        std::string toString() const { return _toString; }
        static DbWebHandler * findHandler( const std::string& url );

    private:
        std::string _name;
        bool _requiresREST;

        std::string _defaultUrl;
        std::string _toString;

        static std::vector<DbWebHandler*> * _handlers;
    };

    class WebStatusPlugin : public Prioritizable {
    public:
        WebStatusPlugin( const std::string& secionName , double priority , const std::string& subheader = "" );
        virtual ~WebStatusPlugin() {}

        virtual void run( std::stringstream& ss ) = 0;
        /** called when web server stats up */
        virtual void init() = 0;

        static void initAll();
        static void runAll( std::stringstream& ss );
    private:
        std::string _name;
        std::string _subHeading;
        static std::vector<WebStatusPlugin*> * _plugins;

    };
    void webServerThread( const AdminAccess* admins, OperationContext::Factory transactionFactory );
    std::string prettyHostName();

};
