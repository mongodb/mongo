/**
*    Copyright (C) 2011 10gen Inc.
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
*/

#include "pch.h"

#include "db/pipeline/document_source.h"

#include "db/pipeline/accumulator.h"
#include "db/pipeline/document.h"
#include "db/pipeline/expression.h"
#include "db/pipeline/value.h"

namespace mongo
{
    string DocumentSourceGroup::idName("_id");

    DocumentSourceGroup::~DocumentSourceGroup()
    {
    }

    bool DocumentSourceGroup::eof()
    {
	if (!populated)
	    populate();

	return (groupsIterator == groups.end());
    }

    bool DocumentSourceGroup::advance()
    {
	if (!populated)
	    populate();

	assert(groupsIterator != groups.end()); // CW TODO error

	++groupsIterator;
	if (groupsIterator == groups.end())
	{
	    pCurrent.reset();
	    return false;
	}

	pCurrent = makeDocument(groupsIterator);
	return true;
    }

    shared_ptr<Document> DocumentSourceGroup::getCurrent()
    {
	if (!populated)
	    populate();

	return pCurrent;
    }

    shared_ptr<DocumentSourceGroup> DocumentSourceGroup::create(
	shared_ptr<DocumentSource> pTheSource)
    {
	shared_ptr<DocumentSourceGroup> pSource(
	    new DocumentSourceGroup(pTheSource));
	return pSource;
    }

    DocumentSourceGroup::DocumentSourceGroup(
	shared_ptr<DocumentSource> pTheSource):
	populated(false),
	pSource(pTheSource),
	pIdExpression(),
	groups(),
	vFieldName(),
	vpAccumulatorFactory(),
	vpExpression()
    {
    }

    void DocumentSourceGroup::addAccumulator(
	string fieldName,
	shared_ptr<Accumulator> (*pAccumulatorFactory)(),
	shared_ptr<Expression> pExpression)
    {
	vFieldName.push_back(fieldName);
	vpAccumulatorFactory.push_back(pAccumulatorFactory);
	vpExpression.push_back(pExpression);
    }

    void DocumentSourceGroup::populate()
    {
	for(bool hasNext = !pSource->eof(); hasNext;
	    hasNext = pSource->advance())
	{
	    shared_ptr<Document> pDocument(pSource->getCurrent());

	    /* get the _id document */
	    shared_ptr<const Value> pId(pIdExpression->evaluate(pDocument));

	    /*
	      Look for the _id value in the map; if it's not there, add a
	       new entry with a blank accumulator.
	    */
	    vector<shared_ptr<Accumulator>> *pGroup;
	    GroupsType::iterator it(groups.find(pId));
	    if (it != groups.end())
	    {
		/* point at the existing accumulators */
		pGroup = &it->second;
	    }
	    else
	    {
		/* insert a new group into the map */
		groups.insert(it,
			      pair<shared_ptr<const Value>,
			      vector<shared_ptr<Accumulator>>>(
				  pId, vector<shared_ptr<Accumulator>>()));

		/* find the accumulator vector (the map value) */
		it = groups.find(pId);
		pGroup = &it->second;

		/* add the accumulators */
		const size_t n = vpAccumulatorFactory.size();
		pGroup->reserve(n);
		for(size_t i = 0; i < n; ++i)
		{
		    shared_ptr<Accumulator> pAccumulator(
			(*vpAccumulatorFactory[i])());
		    pAccumulator->addOperand(vpExpression[i]);
		    pGroup->push_back(pAccumulator);
		}
	    }

	    /* point at the existing key */
	    // unneeded atm // pId = it.first;

	    /* tickle all the accumulators for the group we found */
	    const size_t n = pGroup->size();
	    for(size_t i = 0; i < n; ++i)
		(*pGroup)[i]->evaluate(pDocument);
	}

	/* start the group iterator */
	groupsIterator = groups.begin();
	if (groupsIterator != groups.end())
	    pCurrent = makeDocument(groupsIterator);
	populated = true;
    }

    shared_ptr<Document> DocumentSourceGroup::makeDocument(
	const GroupsType::iterator &rIter)
    {
	vector<shared_ptr<Accumulator>> *pGroup = &rIter->second;
	const size_t n = vFieldName.size();
	shared_ptr<Document> pResult(Document::create(1 + n));

	/* add the _id field */
	pResult->addField(idName, rIter->first);

	/* add the rest of the fields */
	for(size_t i = 0; i < n; ++i)
	    pResult->addField(vFieldName[i], (*pGroup)[i]->getValue());

	return pResult;
    }
}

    
