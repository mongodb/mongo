/**
 * Copyright (c) 2011 10gen Inc.
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
#include "db/commands/pipeline.h"

#include "db/cursor.h"
#include "db/pipeline/accumulator.h"
#include "db/pipeline/document.h"
#include "db/pipeline/document_source.h"
#include "db/pipeline/expression.h"
#include "db/pdfile.h"

namespace mongo {
    // singleton static self-registering instance
    static Pipeline pipelineCmd;

    Pipeline::Pipeline() :
        Command("pipeline") {
    }

    Command::LockType Pipeline::locktype() const {
        return READ;
    }

    bool Pipeline::slaveOk() const {
        return true;
    }

    void Pipeline::help(stringstream &help) const {
        help << "{ pipeline : [ { <data-pipe-op>: {...}}, ... ] }";
    }

    Pipeline::~Pipeline() {
    }

    bool Pipeline::run(const string &db, BSONObj &cmdObj,
                       string &errmsg,
                       BSONObjBuilder &result, bool fromRepl) {
        string collectionName;
        vector<BSONElement> pipeline;

        /* gather the specification for the aggregation */
        for(BSONObj::iterator cmdIterator = cmdObj.begin();
                cmdIterator.more(); ) {
            BSONElement cmdElement(cmdIterator.next());
            const char *pFieldName = cmdElement.fieldName();

            /* look for the aggregation command */
            if (!strcmp(pFieldName, "pipeline")) {
                pipeline = cmdElement.Array();
                continue;
            }

            /* check for the collection name */
            if (!strcmp(pFieldName, "collection")) {
                collectionName = cmdElement.String();
                continue;
            }

            /* we didn't recognize a field in the command */
            ostringstream sb;
            sb <<
               "Pipeline::run(): unrecognized field \"" <<
               cmdElement.fieldName();
            errmsg = sb.str();
            return false;
        }

        /*
          If we get here, we've harvested the fields we expect for a pipeline.

          Set up the document source pipeline.
        */
	vector<shared_ptr<DocumentSource>> vpSource;

        /* iterate over the steps in the pipeline */
        const size_t nSteps = pipeline.size();
        for(size_t iStep = 0; iStep < nSteps; ++iStep) {
            /* pull out the pipeline element as an object */
            BSONElement pipeElement(pipeline[iStep]);
            assert(pipeElement.type() == Object); // CW TODO user error
            BSONObj bsonObj(pipeElement.Obj());

	    shared_ptr<DocumentSource> pSource;

            /* use the object to add a DocumentSource to the processing chain */
            BSONObjIterator bsonIterator(bsonObj);
            while(bsonIterator.more()) {
                BSONElement bsonElement(bsonIterator.next());
                const char *pFieldName = bsonElement.fieldName();

                /* select the appropriate operation */
                if (strcmp(pFieldName, "$project") == 0) {
                    pSource =
			DocumentSourceProject::createFromBson(&bsonElement);
		}
                else if (strcmp(pFieldName, "$filter") == 0) {
                    pSource = 
			DocumentSourceFilter::createFromBson(&bsonElement);
		}
                else if (strcmp(pFieldName, "$group") == 0) {
                    pSource = DocumentSourceGroup::createFromBson(&bsonElement);
		}
                else {
                    ostringstream sb;
                    sb <<
                       "Pipeline::run(): unrecognized pipeline op \"" <<
                       pFieldName;
                    errmsg = sb.str();
                    return false;
                }
            }

	    vpSource.push_back(pSource);
        }

	/* now hook up the pipeline */
        /* connect up a cursor to the specified collection */
        shared_ptr<Cursor> pCursor(
            findTableScan(collectionName.c_str(), BSONObj()));
        shared_ptr<DocumentSource> pSource(
	    DocumentSourceCursor::create(pCursor));

	/* now chain together the sources we found */
	const size_t nSources = vpSource.size();
	for(size_t iSource = 0; iSource < nSources; ++iSource)
	{
	    shared_ptr<DocumentSource> pTemp(vpSource[iSource]);
	    pTemp->setSource(pSource);
	    pSource = pTemp;
	}
	/*
	  CW TODO - move filters up where possible, split pipeline for sharding
	*/
	/* pSource is left pointing at the last source in the chain */

        /*
          Iterate through the resulting documents, and add them to the result.
        */
        BSONArrayBuilder resultArray; // where we'll stash the results
        for(bool hasDocument = !pSource->eof(); hasDocument;
                hasDocument = pSource->advance()) {
            shared_ptr<Document> pDocument(pSource->getCurrent());

            /* add the document to the result set */
            BSONObjBuilder documentBuilder;
            pDocument->toBson(&documentBuilder);
            resultArray.append(documentBuilder.done());
        }

        result.appendArray("result", resultArray.done());

        return true;
    }
}
