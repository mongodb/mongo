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
#include "Pipeline.h"

#include "../cursor.h"
#include "../Document.h"
#include "../DocumentSource.h"
#include "../DocumentSourceCursor.h"
#include "../DocumentSourceProject.h"
#include "../ExpressionField.h"
#include "../Field.h"
#include "../FieldIterator.h"
#include "../pdfile.h"

namespace mongo
{
    // singleton static self-registering instance
    static Pipeline pipelineCmd;

    Pipeline::Pipeline() :
	Command("pipeline")
    {
    }

    Command::LockType Pipeline::locktype() const
    {
	return READ;
    }

    bool Pipeline::slaveOk() const
    {
	return true;
    }

    void Pipeline::help(stringstream &help) const
    {
	help << "{ pipeline : [ { <data-pipe-op>: {...}}, ... ] }";
    }

    Pipeline::~Pipeline()
    {
    }

    bool Pipeline::run(const string &db, BSONObj &cmdObj,
			string &errmsg,
			BSONObjBuilder &result, bool fromRepl)
    {
	string collectionName;
	vector<BSONElement> pipeline;

	/* gather the specification for the aggregation */
	for(BSONObj::iterator cmdIterator = cmdObj.begin();
	    cmdIterator.more(); )
	{
	    BSONElement cmdElement(cmdIterator.next());
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
		"Pipeline::run(): unrecognized field \"" <<
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

	/* iterate over the steps in the pipeline */
	const size_t nSteps = pipeline.size();
	for(size_t iStep = 0; iStep < nSteps; ++iStep)
	{
	    /* pull out the pipeline element as an object */
	    BSONElement pipeElement(pipeline[iStep]);
	    assert(pipeElement.type() == Object); // CW TODO user error
	    BSONObj bsonObj(pipeElement.Obj());

	    /* use the object to add a DocumentSource to the processing chain */
	    BSONObjIterator bsonIterator(bsonObj);
	    while(bsonIterator.more())
	    {
		BSONElement bsonElement(bsonIterator.next());
		const char *pFieldName = bsonElement.fieldName();

		/* select the appropriate operation */
		if (strcmp(pFieldName, "$project") == 0)
		    pSource = setupProject(&bsonElement, pSource);
		else
		{
		    ostringstream sb;
		    sb <<
			"Pipeline::run(): unrecognized pipeline op \"" <<
			pFieldName;
		    errmsg = sb.str();
		    return false;
		}
	    }
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

    shared_ptr<DocumentSource> Pipeline::setupProject(
	BSONElement *pBsonElement, shared_ptr<DocumentSource> pSource)
    {
	/* validate */
	assert(pBsonElement->type() == Object); // CW TODO user error

	/* chain the projection onto the original source */
	shared_ptr<DocumentSourceProject> pProject(
	    DocumentSourceProject::create(pSource));

	/*
	  Pull out the $project object.  This should just be a list of
	  field inclusion or exclusion specifications.  Note you can't do
	  both, except for the case of _id.
	 */
	BSONObj projectObj(pBsonElement->Obj());
	BSONObjIterator fieldIterator(projectObj);
	while(fieldIterator.more())
	{
	    BSONElement outFieldElement(fieldIterator.next());
	    string outFieldName(outFieldElement.fieldName());
	    string inFieldName(outFieldName);
	    BSONType specType = outFieldElement.type();
	    int fieldInclusion = -1;
	    bool ravelArray = false;

	    assert(outFieldName.find('.') == outFieldName.npos);
	        // CW TODO user error: out field name can't use dot notation

	    switch(specType)
	    {
	    case NumberDouble:
	    {
		double inclusion = outFieldElement.numberDouble();
		if ((inclusion == 0) || (inclusion == 1))
		    fieldInclusion = (int)inclusion;
		else
		{
		    assert(false); // CW TODO unimplemented constant expression
		}
		break;
	    }

	    case NumberInt:
		/* just a plain integer include/exclude specification */
		fieldInclusion = outFieldElement.numberInt();
		assert((fieldInclusion >= 0) && (fieldInclusion <= 1));
    		    // CW TODO invalid field projection specification
		break;

	    case Bool:
		/* just a plain boolean include/exclude specification */
		fieldInclusion = outFieldElement.Bool() ? 1 : 0;
		break;

	    case String:
		/* include a field, with rename */
		fieldInclusion = 1;
		inFieldName = outFieldElement.String();
		break;

	    case Object:
	    {
		/*
		  A computed expression, or a $ravel.

		  We handle $ravel as a special case, because this is done
		  by the projection source.  For any other expression,
		  we hand over control to code that parses the expression
		  and returns an expression.
		*/
		BSONObj fieldExprObj(outFieldElement.Obj());
		BSONObjIterator exprIterator(fieldExprObj);
		size_t subFieldCount = 0;
		while(exprIterator.more())
		{
		    ++subFieldCount;

		    BSONElement exprElement(exprIterator.next());
		    const char *pOpName = exprElement.fieldName();
		    if (strcmp(pOpName, "$ravel") != 0)
			assert(false); // CW TODO parseExpression(fieldExprObj);
		    else
		    {
			assert(exprElement.type() == String);
			    // CW TODO $ravel operand must be single field name
			ravelArray = true;
			inFieldName = exprElement.String();
		    }
		}

		assert(subFieldCount == 1);
		    // CW TODO no nested object support, for now
		break;
	    }

	    default:
		assert(false); // CW TODO invalid field projection specification
	    }

	    if (fieldInclusion == 0)
		assert(false); // CW TODO unimplemented
	    else
	    {
		// CW TODO: renames, ravels, expressions
		shared_ptr<Expression> pExpression(
		    ExpressionField::create(inFieldName));
		pProject->includeField(outFieldName, pExpression, ravelArray);
	    }
	}

	return pProject;
    }
}
