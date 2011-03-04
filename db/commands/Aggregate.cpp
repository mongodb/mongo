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
#include "../cursor.h"
#include "../Document.h"
#include "../DocumentSource.h"
#include "../Field.h"
#include "../FieldIterator.h"
#include "../DocumentSourceCursor.h"
#include "../pdfile.h"
#include "Aggregate.h"

namespace mongo {
    
    // singleton static self-registering instance
    static Aggregate aggregateCmd;

    Aggregate::Aggregate() :
	Command("pipeline")
    {
    }

    Command::LockType Aggregate::locktype() const
    {
	return READ;
    }

    bool Aggregate::slaveOk() const
    {
	return true;
    }

    void Aggregate::help(stringstream &help) const
    {
	help << "{ aggregate : [ { <data-pipe-op>: {...}}, ... ] }";
    }

    Aggregate::~Aggregate()
    {
    }

    bool Aggregate::run(const string &db, BSONObj &cmdObj,
			string &errmsg,
			BSONObjBuilder &result, bool fromRepl)
    {
	string collectionName;
	vector<BSONElement> pipeline;

	/* gather the specification for the aggregation */
	for(BSONObj::iterator cmdIterator = cmdObj.begin();
	    cmdIterator.more(); )
	{
	    BSONElement cmdElement = cmdIterator.next();
	    const char *pFieldName = cmdElement.fieldName();

	    /* look for the aggregation command */
	    if (!strcmp(pFieldName, "pipeline"))
	    {
		pipeline = cmdElement.Array();
		continue;
	    }

	    /* check for the collection name */
	    if (!strcmp(pFieldName, "collection"))
	    {
		collectionName = cmdElement.String();
		continue;
	    }

	    /* we didn't recognize a field in the command */
	    ostringstream sb;
	    sb <<
		"Aggregate::run(): unrecognized field \"" <<
		cmdElement.fieldName();
	    errmsg = sb.str();
	    return false;
	}

	/*
	  If we get here, we've harvested the fields we expect

	  Set up the document source pipeline.
	*/
	BSONArrayBuilder resultArray; // where we'll stash the results

	/* connect up a cursor to the specified collection */
	shared_ptr<Cursor> pCursor(
	    findTableScan(collectionName.c_str(), BSONObj()));
	shared_ptr<DocumentSource> pSource(new DocumentSourceCursor(pCursor));

	const size_t nSteps = pipeline.size();
	for(size_t iStep = 0; iStep < nSteps; ++iStep)
	{
	    BSONElement bsonElement(pipeline[iStep]);
	    const char *pFieldName = bsonElement.fieldName();

	    if (strcmp(pFieldName, "$project") == 0)
		pSource = setupProject(&bsonElement, pSource);
	}

	/*
	  Iterate through the resulting documents, and add them to the result.
	*/
	for(bool hasDocument = !pSource->eof(); hasDocument;
	    hasDocument = pSource->advance())
	{
	    shared_ptr<Document> pDocument(pSource->getCurrent());

	    /* add the document to the result set */
	    BSONObjBuilder documentBuilder;
	    pDocument->toBson(&documentBuilder);
	    resultArray.append(documentBuilder.done());
	}

	result.appendArray("result", resultArray.done());
	
	return true;
    }

    shared_ptr<DocumentSource> Aggregate::setupProject(
	BSONElement *pBsonElement, shared_ptr<DocumentSource> pSource)
    {
	return pSource; // TODO
    }
}
