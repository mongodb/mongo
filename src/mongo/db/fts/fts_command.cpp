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
            if (cmdObj["project"].isABSONObj()) {
                projection = cmdObj["project"].Obj();
            }

            return _run( dbname, cmdObj, options,
                         ns, search, language, limit, filter, projection, errmsg, result );
        }


    }


}
