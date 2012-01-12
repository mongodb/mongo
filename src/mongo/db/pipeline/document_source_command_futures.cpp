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
 */

#include "pch.h"

#include "db/pipeline/document_source.h"

namespace mongo {

    DocumentSourceCommandFutures::~DocumentSourceCommandFutures() {
    }

    bool DocumentSourceCommandFutures::eof() {
        /* if we haven't even started yet, do so */
        if (!pCurrent.get())
            getNextDocument();

        return (pCurrent.get() == NULL);
    }

    bool DocumentSourceCommandFutures::advance() {
        if (eof())
            return false;

        /* advance */
        getNextDocument();

        return (pCurrent.get() != NULL);
    }

    intrusive_ptr<Document> DocumentSourceCommandFutures::getCurrent() {
        assert(!eof());
        return pCurrent;
    }

    void DocumentSourceCommandFutures::setSource(
        const intrusive_ptr<DocumentSource> &pSource) {
        /* this doesn't take a source */
        assert(false);
    }

    void DocumentSourceCommandFutures::sourceToBson(
        BSONObjBuilder *pBuilder) const {
        /* this has no BSON equivalent */
        assert(false);
    }

    DocumentSourceCommandFutures::DocumentSourceCommandFutures(
        string &theErrmsg, FuturesList *pList):
        newSource(false),
        pBsonSource(),
        pCurrent(),
        iterator(pList->begin()),
        listEnd(pList->end()),
        errmsg(theErrmsg) {
    }

    intrusive_ptr<DocumentSourceCommandFutures>
    DocumentSourceCommandFutures::create(
        string &errmsg, FuturesList *pList) {
        intrusive_ptr<DocumentSourceCommandFutures> pSource(
            new DocumentSourceCommandFutures(errmsg, pList));
        return pSource;
    }

    void DocumentSourceCommandFutures::getNextDocument() {
        while(true) {
            if (!pBsonSource.get()) {
                /* if there aren't any more futures, we're done */
                if (iterator == listEnd) {
                    pCurrent.reset();
                    return;
                }

                /* grab the next command result */
                shared_ptr<Future::CommandResult> pResult(*iterator);
                ++iterator;

                /* try to wait for it */
                if (!pResult->join()) {
                    error() << "sharded pipeline failed on shard: " <<
                        pResult->getServer() << " error: " <<
                        pResult->result() << endl;
                    errmsg += "-- mongod pipeline failed: ";
                    errmsg += pResult->result().toString();

                    /* move on to the next command future */
                    continue;
                }

                /* grab the result array out of the shard server's response */
                BSONObj shardResult(pResult->result());
                BSONObjIterator objIterator(shardResult);
                while(objIterator.more()) {
                    BSONElement element(objIterator.next());
                    const char *pFieldName = element.fieldName();

                    /* find the result array and quit this loop */
                    if (strcmp(pFieldName, "result") == 0) {
                        pBsonSource = DocumentSourceBsonArray::create(&element);
                        newSource = true;
                        break;
                    }
                }
            }

            /* if we're done with this shard's results, try the next */
            if (pBsonSource->eof() ||
                (!newSource && !pBsonSource->advance())) {
                pBsonSource.reset();
                continue;
            }

            pCurrent = pBsonSource->getCurrent();
            newSource = false;
            return;
        }
    }
}
