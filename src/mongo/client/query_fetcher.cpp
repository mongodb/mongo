/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/client/query_fetcher.h"

namespace mongo {

    QueryFetcher::QueryFetcher(executor::TaskExecutor* exec,
                               const HostAndPort& src,
                               const NamespaceString& nss,
                               const BSONObj& cmdBSON,
                               const CallbackFn& work)
        : _exec(exec),
          _fetcher(exec,
                   src,
                   nss.db().toString(),
                   cmdBSON,
                   stdx::bind(&QueryFetcher::_onFetchCallback,
                              this,
                              stdx::placeholders::_1,
                              stdx::placeholders::_2,
                              stdx::placeholders::_3)),
        _responses(0),
        _work(work) {

    }

    int QueryFetcher::_getResponses() const {
        return _responses;
    }

    void QueryFetcher::_onFetchCallback(const Fetcher::QueryResponseStatus& fetchResult,
                                        Fetcher::NextAction* nextAction,
                                        BSONObjBuilder* getMoreBob) {

        _delegateCallback(fetchResult, nextAction);

        ++_responses;

        // The fetcher will continue to call with kGetMore until an error or the last batch.
        if (fetchResult.isOK() && *nextAction == Fetcher::NextAction::kGetMore) {
            const auto batchData(fetchResult.getValue());
            invariant(getMoreBob);
            getMoreBob->append("getMore", batchData.cursorId);
            getMoreBob->append("collection", batchData.nss.coll());
        }
    }

    void QueryFetcher::_onQueryResponse(const Fetcher::QueryResponseStatus& fetchResult,
                                        Fetcher::NextAction* nextAction) {
        _work(fetchResult, nextAction);
    }

    void QueryFetcher::_delegateCallback(const Fetcher::QueryResponseStatus& fetchResult,
                                         Fetcher::NextAction* nextAction) {
        _onQueryResponse(fetchResult, nextAction);
    };

    std::string QueryFetcher::getDiagnosticString() const {
        return str::stream() << "QueryFetcher -"
                             << " responses: " << _responses
                             << " fetcher: " << _fetcher.getDiagnosticString();
    }

} // namespace mongo
