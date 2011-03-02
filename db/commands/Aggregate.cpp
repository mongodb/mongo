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
#include "../commands.h"
#include "../cursor.h"
#include "../Document.h"
#include "../Field.h"
#include "../FieldIterator.h"
#include "../DocumentSourceCursor.h"
#include "../pdfile.h"
#include "Aggregate.h"

namespace mongo {
    
    // singleton static self-registering instance
    static Aggregate aggregateCmd;

    Aggregate::Aggregate() :
	Command("aggregate")
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

	DEV log() << "Aggregate::run() begins..." << endl;

	/* gather the specification for the aggregation */
	for(BSONObj::iterator cmdIterator = cmdObj.begin();
	    cmdIterator.more(); )
	{
	    BSONElement cmdElement = cmdIterator.next();
	    const char *pFieldName = cmdElement.fieldName();

	    /* look for the aggregation command */
	    if (!strcmp(pFieldName, "aggregate"))
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


	/* if we get here, we've harvested the fields we expect */
	const size_t nSteps = pipeline.size();

	/* set up the document source pipeline */

	/* connect up a cursor to the specified collection */
	shared_ptr<Cursor> pCursor(
	    findTableScan(collectionName.c_str(), BSONObj()));

/*
	for(bool hasNext = pCursor->ok(); hasNext; hasNext = pCursor->advance())
	{
	    BSONObj bsonObj = pCursor->current();
	    
	    DEV log() << "Aggregate::run() got a document" << endl;
	}
*/

	shared_ptr<DocumentSource> pSource(
	    new DocumentSourceCursor(pCursor.get()));
	for(bool hasDocument = !pSource->eof(); hasDocument;
	    hasDocument = pSource->advance())
	{
	    shared_ptr<Document> pDocument(pSource->getCurrent());
	    shared_ptr<FieldIterator> pFieldIterator(
		pDocument->createFieldIterator());

	    DEV log() << "Aggregate::run() document begin..." << endl;

	    for(bool hasField = !pFieldIterator->eof(); hasField;
		hasField = pFieldIterator->advance())
	    {
		shared_ptr<Field> pField(pFieldIterator->getCurrent());

		DEV log() << "Aggregate::run() Field \"" <<
		    pField->getName() << "\" type \"" <<
		    (int)pField->getType() << endl;
	    }
	    
	    DEV log() << "Aggregate::run() ...end document" << endl;
	}
	
	DEV log() << "Aggregate::run() ...ends" << endl;

	return true;
    }
}
