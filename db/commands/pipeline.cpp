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
	/* try to parse the command; if this fails, then we didn't run */
	shared_ptr<Spec> pSpec(Spec::parseCommand(errmsg, cmdObj));
	if (!pSpec.get())
	    return false;

	/* now hook up the pipeline */
        /* connect up a cursor to the specified collection */
        shared_ptr<Cursor> pCursor(
            findTableScan(pSpec->getCollectionName().c_str(), BSONObj()));
        shared_ptr<DocumentSource> pSource(
	    DocumentSourceCursor::create(pCursor));

	return pSpec->run(result, errmsg, pSource);
    }


    Pipeline::Spec::Spec():
	collectionName(),
	vpSource() {
    }

    shared_ptr<Pipeline::Spec> Pipeline::Spec::parseCommand(
	string &errmsg, BSONObj &cmdObj) {
	shared_ptr<Spec> pSpec(new Spec());
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
                pSpec->collectionName = cmdElement.String();
                continue;
            }

            /* we didn't recognize a field in the command */
            ostringstream sb;
            sb <<
               "Pipeline::parseCommand(): unrecognized field \"" <<
               cmdElement.fieldName();
            errmsg = sb.str();
	    return shared_ptr<Spec>();
        }

        /*
          If we get here, we've harvested the fields we expect for a pipeline.

          Set up the document source pipeline.
        */
	vector<shared_ptr<DocumentSource>> *pvpSource = &pSpec->vpSource;

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
		    return shared_ptr<Spec>();
                }
            }

	    pvpSource->push_back(pSource);
        }

	/*
	  CW TODO - move filters up where possible, split pipeline for sharding
	*/

	/* optimize the elements in the pipeline */
	size_t nSources = pvpSource->size();
	for(size_t iSource = 0; iSource < nSources; ++iSource)
	    (*pvpSource)[iSource]->optimize();

	/* find the first group operation, if there is one */
	size_t firstGroup = nSources + 1;
	for(firstGroup = 0; firstGroup < nSources; ++firstGroup) {
	    DocumentSource *pDS = (*pvpSource)[firstGroup].get();
	    if (dynamic_cast<DocumentSourceGroup *>(pDS))
		break;
	}

	/*
	  Break down any filters before groups into chunks we can convert for
	  matcher use.

	  LATER -- we've move these further up the chain past projections
	  where possible, remapping field names as they move past projections
	  that rename the fields.
	*/
	// CW TODO

	return pSpec;
    }

    shared_ptr<Pipeline::Spec> Pipeline::Spec::splitForSharded() {
	/* create an initialize the shard spec we'll return */
	shared_ptr<Spec> pShardSpec(new Spec());
	pShardSpec->collectionName = collectionName;

	/* look for a grouping operation */
	const size_t nSources = vpSource.size();
	DocumentSourceGroup *pGroup = NULL;
	size_t iGroup = nSources;
	for(size_t iSource = 0; iSource < nSources; ++iSource) {
	    pGroup = dynamic_cast<DocumentSourceGroup *>(
		vpSource[iSource].get());
	    if (pGroup) {
		iGroup = iSource;
		break;
	    }
	}

	/*
	  If there is a group, create a new spec with everything up to
	  and including that.

	  If there is no group, the shard spec will include everything,
	  and the merger spec will just include a union.
	 */
	const size_t copyLimit = pGroup ? (iGroup + 1) : nSources;
	for(size_t iSource = 0; iSource < copyLimit; ++iSource)
	    pShardSpec->vpSource.push_back(vpSource[iSource]);

	/*
	  If there was a grouping operator, fork that and put it at the
	  beginning of the mongos spec.
	*/
	size_t iCopyTo = 0;
    	if (pGroup)
	    vpSource[iCopyTo++] = pGroup->createMerger();

	/*
	  In the original spec, move all the document sources down, eliminating
	  those that were copied into the shard spec.  Then eliminate all the
	  trailing entries.
	 */
	for(size_t iCopyFrom = copyLimit; iCopyFrom < nSources;
	    ++iCopyTo, ++iCopyFrom)
	    vpSource[iCopyTo] = vpSource[iCopyFrom];
	vpSource.resize(iCopyTo);

	return pShardSpec;
    }

    void Pipeline::Spec::toBson(BSONObjBuilder *pBuilder) const {
	assert(false && "unimplemented");
    }

    bool Pipeline::Spec::run(BSONObjBuilder &result, string &errmsg,
			     shared_ptr<DocumentSource> pSource) const {
	/* now chain together the sources we found */
	const size_t nSources = vpSource.size();
	for(size_t iSource = 0; iSource < nSources; ++iSource)
	{
	    shared_ptr<DocumentSource> pTemp(vpSource[iSource]);
	    pTemp->setSource(pSource);
	    pSource = pTemp;
	}
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

} // namespace mongo
