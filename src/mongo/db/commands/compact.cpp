/// compact.cpp

/**
*    Copyright (C) 2013 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,b
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

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/background.h"
#include "mongo/db/commands.h"
#include "mongo/db/database.h"
#include "mongo/db/d_concurrency.h"
#include "mongo/db/curop-inl.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/structure/collection.h"

namespace mongo {

    bool compactCollection(Collection* collection, string& errmsg, bool validate,
                           BSONObjBuilder& result, double pf, int pb, bool useDefaultPadding,
                           bool preservePadding);


    // from repl/rs.cpp
    bool isCurrentlyAReplSetPrimary();

    class CompactCmd : public Command {
    public:
        virtual LockType locktype() const { return NONE; }
        virtual bool adminOnly() const { return false; }
        virtual bool slaveOk() const { return true; }
        virtual bool maintenanceMode() const { return true; }
        virtual bool logTheOp() { return false; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::compact);
            out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
        }
        virtual void help( stringstream& help ) const {
            help << "compact collection\n"
                "warning: this operation blocks the server and is slow. you can cancel with cancelOp()\n"
                "{ compact : <collection_name>, [force:<bool>], [validate:<bool>],\n"
                "  [paddingFactor:<num>], [paddingBytes:<num>] }\n"
                "  force - allows to run on a replica set primary\n"
                "  validate - check records are noncorrupt before adding to newly compacting extents. slower but safer (defaults to true in this version)\n";
        }
        CompactCmd() : Command("compact") { }

        virtual std::vector<BSONObj> stopIndexBuilds(const std::string& dbname,
                                                     const BSONObj& cmdObj) {
            std::string systemIndexes = dbname+".system.indexes";
            std::string coll = cmdObj.firstElement().valuestr();
            std::string ns = dbname + "." + coll;
            BSONObj criteria = BSON("ns" << systemIndexes << "op" << "insert" << "insert.ns" << ns);

            return IndexBuilder::killMatchingIndexBuilds(criteria);
        }

        virtual bool run(const string& db, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            string coll = cmdObj.firstElement().valuestr();
            if( coll.empty() || db.empty() ) {
                errmsg = "no collection name specified";
                return false;
            }

            if( isCurrentlyAReplSetPrimary() && !cmdObj["force"].trueValue() ) {
                errmsg = "will not run compact on an active replica set primary as this is a slow blocking operation. use force:true to force";
                return false;
            }

            NamespaceString ns(db,coll);
            if ( !ns.isNormal() ) {
                errmsg = "bad namespace name";
                return false;
            }

            if ( ns.isSystem() ) {
                // items in system.* cannot be moved as there might be pointers to them
                // i.e. system.indexes entries are pointed to from NamespaceDetails
                errmsg = "can't compact a system namespace";
                return false;
            }

            double pf = 1.0;
            int pb = 0;
            // preservePadding trumps all other compact methods
            bool preservePadding = false;
            // useDefaultPadding is used to track whether or not a padding requirement was passed in
            // if it wasn't than UsePowerOf2Sizes will be maintained when compacting
            bool useDefaultPadding = true;
            if (cmdObj.hasElement("preservePadding")) {
                preservePadding = cmdObj["preservePadding"].trueValue();
                useDefaultPadding = false;
            }

            if( cmdObj.hasElement("paddingFactor") ) {
                if (preservePadding == true) {
                    errmsg = "preservePadding is incompatible with paddingFactor";
                    return false;
                }
                useDefaultPadding = false;
                pf = cmdObj["paddingFactor"].Number();
                verify( pf >= 1.0 && pf <= 4.0 );
            }
            if( cmdObj.hasElement("paddingBytes") ) {
                if (preservePadding == true) {
                    errmsg = "preservePadding is incompatible with paddingBytes";
                    return false;
                }
                useDefaultPadding = false;
                pb = (int) cmdObj["paddingBytes"].Number();
                verify( pb >= 0 && pb <= 1024 * 1024 );
            }

            bool validate = true; // default is true at the moment
            if ( cmdObj.hasElement("validate") )
                validate = cmdObj["validate"].trueValue();

            bool ok = false;
            {
                Lock::DBWrite lk(ns.ns());
                BackgroundOperation::assertNoBgOpInProgForNs(ns.ns());
                Client::Context ctx(ns);

                Collection* collection = ctx.db()->getCollection(ns.ns());
                if( ! collection ) {
                    errmsg = "namespace does not exist";
                    return false;
                }

                if ( collection->isCapped() ) {
                    errmsg = "cannot compact a capped collection";
                    return false;
                }

                log() << "compact " << ns << " begin" << endl;

                std::vector<BSONObj> indexesInProg = stopIndexBuilds(db, cmdObj);

                if( pf != 0 || pb != 0 ) {
                    log() << "paddingFactor:" << pf << " paddingBytes:" << pb << endl;
                }
                try {
                    ok = compactCollection(collection, errmsg, validate,
                                           result, pf, pb, useDefaultPadding, preservePadding);
                }
                catch(...) {
                    log() << "compact " << ns << " end (with error)" << endl;
                    throw;
                }
                log() << "compact " << ns << " end" << endl;

                IndexBuilder::restoreIndexes(indexesInProg);
            }

            return ok;
        }
    };
    static CompactCmd compactCmd;

}
