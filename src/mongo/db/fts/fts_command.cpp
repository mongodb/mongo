// fts_command.h

/**
*    Copyright (C) 2012 10gen Inc.
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
*/

#include <string>
#include <vector>

#include "mongo/db/fts/fts_command.h"
#include "mongo/db/fts/fts_enabled.h"
#include "mongo/db/fts/fts_search.h"
#include "mongo/db/fts/fts_util.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/timer.h"

namespace mongo {

    namespace fts {

        using namespace mongoutils;

        FTSCommand ftsCommand;

        FTSCommand::FTSCommand()
            : Command( "text" ) {
        }

        void FTSCommand::addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::find);
            out->push_back(Privilege(parseNs(dbname, cmdObj), actions));
        }


        bool FTSCommand::run(const string& dbname,
                             BSONObj& cmdObj,
                             int options,
                             string& errmsg,
                             BSONObjBuilder& result,
                             bool fromRepl) {

            if ( !isTextSearchEnabled() ) {
                errmsg = "text search not enabled";
                return false;
            }

            string ns = dbname + "." + cmdObj.firstElement().String();

            string search = cmdObj["search"].valuestrsafe();
            if ( search.size() == 0 ) {
                errmsg = "no search specified";
                return false;
            }

            string language = cmdObj["language"].valuestrsafe();

            int limit = cmdObj["limit"].numberInt();
            if (limit == 0)
                limit = 100;

            BSONObj filter;
            if ( cmdObj["filter"].isABSONObj() )
                filter = cmdObj["filter"].Obj();

            BSONObj projection;
            if (cmdObj["projection"].isABSONObj()) {
                projection = cmdObj["projection"].Obj();
            }

            return _run( dbname, cmdObj, options,
                         ns, search, language, limit, filter, projection, errmsg, result );
        }


    }


}
