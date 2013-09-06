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

#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbwebserver.h"
#include "mongo/util/mongoutils/html.h"

namespace mongo {
namespace {
    class ClientListPlugin : public WebStatusPlugin {
    public:
        ClientListPlugin() : WebStatusPlugin( "clients" , 20 ) {}
        virtual void init() {}

        virtual void run( std::stringstream& ss ) {
            using namespace mongoutils::html;

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
            {
                scoped_lock bl(Client::clientsMutex);
                for( set<Client*>::iterator i = Client::clients.begin(); i != Client::clients.end(); i++ ) {
                    Client *c = *i;
                    CurOp& co = *(c->curop());
                    ss << "<tr><td>" << c->desc() << "</td>";

                    tablecell( ss , co.opNum() );
                    tablecell( ss , co.active() );
                    tablecell( ss , c->lockState().reportState() );
                    if ( co.active() )
                        tablecell( ss , co.elapsedSeconds() );
                    else
                        tablecell( ss , "" );
                    tablecell( ss , co.getOp() );
                    tablecell( ss , html::escape( co.getNS() ) );
                    if ( co.haveQuery() )
                        tablecell( ss , html::escape( co.query().toString() ) );
                    else
                        tablecell( ss , "" );
                    tablecell( ss , co.getRemoteString() );

                    tablecell( ss , co.getMessage() );
                    tablecell( ss , co.getProgressMeter().toString() );


                    ss << "</tr>\n";
                }
            }
            ss << "</table>\n";

        }

    } clientListPlugin;
}  // namespace
}  // namespace mongo
