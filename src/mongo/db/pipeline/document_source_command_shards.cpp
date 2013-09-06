/**
 * Copyright 2011 (c) 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#include "mongo/pch.h"

#include "mongo/db/pipeline/document_source.h"
#include "mongo/s/shard.h"

namespace mongo {

    void DocumentSourceCommandShards::setSource(DocumentSource *pSource) {
        /* this doesn't take a source */
        verify(false);
    }

    Value DocumentSourceCommandShards::serialize(bool explain) const {
        // this has no BSON equivalent
        verify(false);
    }

    DocumentSourceCommandShards::DocumentSourceCommandShards(
        const ShardOutput& shardOutput,
        const intrusive_ptr<ExpressionContext> &pExpCtx):
        DocumentSource(pExpCtx),
        unstarted(true),
        hasCurrent(false),
        newSource(false),
        pBsonSource(),
        pCurrent(),
        iterator(shardOutput.begin()),
        listEnd(shardOutput.end())
    {}

    intrusive_ptr<DocumentSourceCommandShards>
    DocumentSourceCommandShards::create(
        const ShardOutput& shardOutput,
        const intrusive_ptr<ExpressionContext> &pExpCtx) {
        intrusive_ptr<DocumentSourceCommandShards> pSource(
            new DocumentSourceCommandShards(shardOutput, pExpCtx));
        return pSource;
    }

    boost::optional<Document> DocumentSourceCommandShards::getNext() {
        pExpCtx->checkForInterrupt();

        while(true) {
            if (!pBsonSource.get()) {
                /* if there aren't any more futures, we're done */
                if (iterator == listEnd)
                    return boost::none;

                /* grab the next command result */
                BSONObj resultObj = iterator->result;

                uassert(16390, str::stream() << "sharded pipeline failed on shard " <<
                                            iterator->shardTarget.getName() << ": " <<
                                            resultObj.toString(),
                        resultObj["ok"].trueValue());

                /* grab the result array out of the shard server's response */
                BSONElement resultArray = resultObj["result"];
                massert(16391, str::stream() << "no result array? shard:" <<
                                            iterator->shardTarget.getName() << ": " <<
                                            resultObj.toString(),
                        resultArray.type() == Array);

                // done with error checking, don't need the shard name anymore
                ++iterator;

                if (resultArray.embeddedObject().isEmpty()){
                    // this shard had no results, on to the next one
                    continue;
                }

                pBsonSource = DocumentSourceBsonArray::create(&resultArray, pExpCtx);
            }

            if (boost::optional<Document> out = pBsonSource->getNext())
                return out;

            // Source exhausted. Try next.
            pBsonSource.reset();
        }
    }
}
