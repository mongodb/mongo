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
#include "DocumentSourceProject.h"

#include "Document.h"
#include "Expression.h"
#include "Value.h"

namespace mongo
{
    shared_ptr<DocumentSourceProject> DocumentSourceProject::create(
	shared_ptr<DocumentSource> pTheSource)
    {
	shared_ptr<DocumentSourceProject> pSource(
	    new DocumentSourceProject(pTheSource));
	return pSource;
    }

    DocumentSourceProject::DocumentSourceProject(
	shared_ptr<DocumentSource> pTheSource):
	pSource(pTheSource),
	vpExpression(),
	ravelWhich(-1),
	iRavel(0),
	nRavel(0),
	pNoRavelDocument(),
	pRavelValue()
    {
    }

    bool DocumentSourceProject::eof()
    {
	/*
	  If we're raveling an array, and there are more elements, then we
	  can return more documents.
	*/
	if ((ravelWhich >= 0) && (iRavel < nRavel))
	    return false;

	return pSource->eof();
    }

    bool DocumentSourceProject::advance()
    {
	if (ravelWhich >= 0)
	{
	    if (++iRavel < nRavel)
		return true;

	    /* restart array element iteration for the next document */
	    iRavel = 0;
	}

	/* release the last document and advance */
	pRavelValue.reset();
	pNoRavelDocument.reset();
	return pSource->advance();
    }

    shared_ptr<Document> DocumentSourceProject::getCurrent()
    {
	if (!pNoRavelDocument.get())
	{
	    shared_ptr<Document> pInDocument(pSource->getCurrent());
	    pNoRavelDocument = Document::create();

	    /*
	      Use the expressions to create a new Document out of the
	      source Document
	    */
	    const size_t n = vFieldName.size();
	    for(size_t i = 0; i < n; ++i)
	    {
		string outName(vFieldName[i]);
		shared_ptr<Expression> pExpression(vpExpression[i]);

		/* get the value for the field */
		shared_ptr<const Value> pOutValue(
		    pExpression->evaluate(pInDocument));

		/*
		  If we're raveling this field, and it's an array, then we're
		  going to pick off elements one by one, and make fields of
		  them below.
		*/
		if (((int)i == ravelWhich) && (pOutValue->getType() == Array))
		{
		    pRavelValue = pOutValue;
		    nRavel = pRavelValue->getArray()->size();

		    /*
		      The $ravel of an empty array is a nul value.  If we
		      encounter this, use the non-ravel path, but replace
		      pOutField with a nul.
		    */
		    if (nRavel == 0)
		    {
			pRavelValue.reset();
			pOutValue = Value::getNull();
		    }
		}

		/* add the field to the document under construction */
		pNoRavelDocument->addField(outName, pOutValue);
	    }
	}

	/*
	  If we're raveling a field, create an alternate document.  In the
	  alternate (clone), replace the raveled array field with the element
	  at the appropriate index.
	 */
	if (pRavelValue.get())
	{
	    /* clone the document with an array we're raveling */
	    shared_ptr<Document> pRavelDocument(pNoRavelDocument->clone());

	    /* get access to the array of values */
	    const vector<shared_ptr<const Value>> *pvpValue =
		pRavelValue->getArray();

	    /* grab the individual array element */
	    shared_ptr<const Value> pArrayValue((*pvpValue)[iRavel]);

	    /* substitute the named field into the prototype document */
	    pRavelDocument->setField(
		(size_t)ravelWhich, vFieldName[ravelWhich], pArrayValue);

	    return pRavelDocument;
	}

	return pNoRavelDocument;
    }

    DocumentSourceProject::~DocumentSourceProject()
    {
    }

    void DocumentSourceProject::addField(
	string fieldName, shared_ptr<Expression> pExpression, bool ravelArray)
    {
	assert(fieldName.length()); // CW TODO must be a non-empty string
	assert(pExpression); // CW TODO must be a non-null expression
	assert(!ravelArray || (ravelWhich < 0));
	    // CW TODO only one ravel allowed

	/* if we're raveling, remember which field */
	if (ravelArray)
	    ravelWhich = vFieldName.size();

	vFieldName.push_back(fieldName);
	vpExpression.push_back(pExpression);
    }
}
