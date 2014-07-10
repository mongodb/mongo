// fts_command_mongod.h

/**
*    Copyright (C) 2012-2014 MongoDB Inc.
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

#include <string>

#include "mongo/db/client.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/fts/fts_command.h"
#include "mongo/db/fts/fts_util.h"
#include "mongo/db/query/get_runner.h"
#include "mongo/db/query/type_explain.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/timer.h"

namespace mongo {

    namespace fts {

        /*
         * Runs the command object cmdobj on the db with name dbname and puts result in result.
         * @param dbname, name of db
         * @param cmdobj, object that contains entire command
         * @param options
         * @param errmsg, reference to error message
         * @param result, reference to builder for result
         * @param fromRepl
         * @return true if successful, false otherwise
         */
        bool FTSCommand::_run(OperationContext* txn,
                              const string& dbname,
                              BSONObj& cmdObj,
                              int cmdOptions,
                              const string& ns,
                              const string& searchString,
                              string language, // "" for not-set
                              int limit,
                              BSONObj& filter,
                              BSONObj& projection,
                              string& errmsg,
                              BSONObjBuilder& result ) {

            Timer comm;

            // Rewrite the cmd as a normal query.
            BSONObjBuilder queryBob;
            queryBob.appendElements(filter);

            BSONObjBuilder textBob;
            textBob.append("$search", searchString);
            if (!language.empty()) {
                textBob.append("$language", language);
            }
            queryBob.append("$text", textBob.obj());

            // This is the query we exec.
            BSONObj queryObj = queryBob.obj();

            // We sort by the score.
            BSONObj sortSpec = BSON("$s" << BSON("$meta" << LiteParsedQuery::metaTextScore));

            // We also project the score into the document and strip it out later during the reformatting
            // of the results.
            BSONObjBuilder projBob;
            projBob.appendElements(projection);
            projBob.appendElements(sortSpec);
            BSONObj projObj = projBob.obj();

            Client::ReadContext ctx(txn, ns);

            CanonicalQuery* cq;
            Status canonicalizeStatus = 
                    CanonicalQuery::canonicalize(ns, 
                                                 queryObj,
                                                 sortSpec,
                                                 projObj, 
                                                 0,
                                                 limit,
                                                 BSONObj(),
                                                 &cq,
                                                 WhereCallbackReal(StringData(dbname)));
            if (!canonicalizeStatus.isOK()) {
                errmsg = canonicalizeStatus.reason();
                return false;
            }

            Runner* rawRunner;
            Status getRunnerStatus = getRunner(txn, ctx.ctx().db()->getCollection(txn, ns), cq, &rawRunner);
            if (!getRunnerStatus.isOK()) {
                errmsg = getRunnerStatus.reason();
                return false;
            }

            auto_ptr<Runner> runner(rawRunner);

            BSONArrayBuilder resultBuilder(result.subarrayStart("results"));

            // Quoth: "leave a mb for other things"
            int resultSize = 1024 * 1024;

            int numReturned = 0;

            BSONObj obj;
            while (Runner::RUNNER_ADVANCED == runner->getNext(&obj, NULL)) {
                if ((resultSize + obj.objsize()) >= BSONObjMaxUserSize) {
                    break;
                }
                // We return an array of results.  Add another element.
                BSONObjBuilder oneResultBuilder(resultBuilder.subobjStart());
                oneResultBuilder.append("score", obj["$s"].number());

                // Strip out the score from the returned obj.
                BSONObjIterator resIt(obj);
                BSONObjBuilder resBob;
                while (resIt.more()) {
                    BSONElement elt = resIt.next();
                    if (!mongoutils::str::equals("$s", elt.fieldName())) {
                        resBob.append(elt);
                    }
                }
                oneResultBuilder.append("obj", resBob.obj());
                BSONObj addedArrayObj = oneResultBuilder.done();
                resultSize += addedArrayObj.objsize();
                numReturned++;
            }

            resultBuilder.done();

            // returns some stats to the user
            BSONObjBuilder stats(result.subobjStart("stats"));

            // Fill in nscanned from the explain.
            TypeExplain* bareExplain;
            Status res = runner->getInfo(&bareExplain, NULL);
            if (res.isOK()) {
                auto_ptr<TypeExplain> explain(bareExplain);
                stats.append("nscanned", explain->getNScanned());
                stats.append("nscannedObjects", explain->getNScannedObjects());
            }

            stats.appendNumber( "n" , numReturned );
            stats.append( "timeMicros", (int)comm.micros() );
            stats.done();

            return true;
        }

    }  // namespace fts

}  // namespace mongo
