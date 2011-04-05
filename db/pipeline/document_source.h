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

#pragma once

#include "pch.h"

#include "db/jsobj.h"
#include "db/pipeline/value.h"

namespace mongo {
    class Accumulator;
    class Cursor;
    class Document;
    class Expression;

    class DocumentSource :
            boost::noncopyable {
    public:
	virtual ~DocumentSource();

        /*
	  Is the source at EOF?

	  @returns true if the source has no more Documents to return.
        */
        virtual bool eof() = 0;

        /*
	  Advance the state of the DocumentSource so that it will return the
	  next Document.

	  @returns whether there is another document to fetch, i.e., whether or
	    not getCurrent() will succeed.
        */
        virtual bool advance() = 0;

        /*
          Advance the source, and return the next Expression.

	  @returns the current Document
          TODO throws an exception if there are no more expressions to return.
        */
        virtual shared_ptr<Document> getCurrent() = 0;

	/*
	  Set the underlying source this source should use to get Documents
	  from.

	  It is an error to set the source more than once.  This is to
	  prevent changing sources once the original source has been started;
	  this could break the state maintained by the DocumentSource.

	  @param pSource the underlying source to use
	 */
	virtual void setSource(shared_ptr<DocumentSource> pSource);

	/*
	  Optimize the pipeline operation, if possible.

	  This is intended for any operations that include expressions, and
	  provides a hook for those to optimize those operations.

	  The default implementation is to do nothing.
	 */
	virtual void optimize();

        /*
	  Add the pipeline operation to the builder.

	  There are some operations for which this doesn't make sense; the
	  default implementation is to assert(), and this can be used for
	  those.

	  @params pBuilder the builder to add the operation to.
         */
	virtual void toBson(BSONObjBuilder *pBuilder) const;

    protected:
	/*
	  Most DocumentSources have an underlying source they get their data
	  from.  This is a convenience for them.

	  The default implementation of setSource() sets this; if you don't
	  need a source, override that to assert().  The default is to
	  assert() if this has already been set.
	*/
	shared_ptr<DocumentSource> pSource;
    };


    class DocumentSourceCursor :
        public DocumentSource {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceCursor();
        virtual bool eof();
        virtual bool advance();
        virtual shared_ptr<Document> getCurrent();
	virtual void setSource(shared_ptr<DocumentSource> pSource);

	/*
	  Create a document source based on a cursor.

	  This is usually put at the beginning of a chain of document sources
	  in order to fetch data from the database.

	  @param pCursor the cursor to use to fetch data
	*/
	static shared_ptr<DocumentSourceCursor> create(
	    shared_ptr<Cursor> pCursor);

    private:
        DocumentSourceCursor(shared_ptr<Cursor> pTheCursor);

        boost::shared_ptr<Cursor> pCursor;
    };


    class DocumentSourceFilter :
        public DocumentSource {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceFilter();
        virtual bool eof();
        virtual bool advance();
        virtual shared_ptr<Document> getCurrent();
	virtual void optimize();
	virtual void toBson(BSONObjBuilder *pBuilder) const;

	/*
	  Create a filter.

          @param pBsonElement the raw BSON specification for the filter
          @returns the filter
	 */
	static shared_ptr<DocumentSourceFilter> createFromBson(
	    BSONElement *pBsonElement);

        /*
          Create a filter.

          @param pFilter the expression to use to filter
          @returns the filter
         */
        static shared_ptr<DocumentSourceFilter> create(
            shared_ptr<Expression> pFilter);

	/*
	  Create a BSONObj suitable for Matcher construction.

	  This is used after filter analysis has moved as many filters to
	  as early a point as possible in the document processing pipeline.
	  See db/Matcher.h and the associated wiki documentation for the
	  format.  This conversion is used to move back to the low-level
	  find() Cursor mechanism.

	  @params pBuilder the builder to write to
	 */
	void toMatcherBson(BSONObjBuilder *pBuilder) const;

    private:
        DocumentSourceFilter(shared_ptr<Expression> pFilter);

        shared_ptr<Expression> pFilter;

        void findNext();

        bool unstarted;
        bool hasNext;
        shared_ptr<Document> pCurrent;
    };

    class DocumentSourceGroup :
        public DocumentSource {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceGroup();
        virtual bool eof();
        virtual bool advance();
        virtual shared_ptr<Document> getCurrent();
	virtual void toBson(BSONObjBuilder *pBuilder) const;

        /*
          Create a new grouping DocumentSource.
	  
	  @returns the DocumentSource
         */
        static shared_ptr<DocumentSourceGroup> create();

        /*
          Set the Id Expression.

          Documents that pass through the grouping Document are grouped
          according to this key.  This will generate the id_ field in the
          result documents.

          @param pExpression the group key
         */
        void setIdExpression(shared_ptr<Expression> pExpression);

        /*
          Add an accumulator.

          Accumulators become fields in the Documents that result from
          grouping.  Each unique group document must have it's own
          accumulator; the accumulator factory is used to create that.

          @param fieldName the name the accumulator result will have in the
                result documents
          @param pAccumulatorFactory used to create the accumulator for the
                group field
         */
        void addAccumulator(string fieldName,
                            shared_ptr<Accumulator> (*pAccumulatorFactory)(),
                            shared_ptr<Expression> pExpression);

	/*
	  Create a grouping DocumentSource from BSON.

	  This is a convenience method that uses the above, and operates on
	  a BSONElement that has been deteremined to be an Object with an
	  element named $group.

	  @param pBsonElement the BSONELement that defines the group
	  @returns the grouping DocumentSource
	 */
        static shared_ptr<DocumentSource> createFromBson(
	    BSONElement *pBsonElement);


	/*
	  Create a unifying group that can be used to combine group results
	  from shards.

	  @returns the grouping DocumentSource
	*/
	shared_ptr<DocumentSource> createMerger();

    private:
        DocumentSourceGroup();

	/*
	  Before returning anything, this source must fetch everything from
	  the underlying source and group it.  populate() is used to do that
	  on the first call to any method on this source.  The populated
	  boolean indicates that this has been done.
	 */
        void populate();
        bool populated;

        struct KeyComparator {
            bool operator()(const shared_ptr<const Value> &rL,
                            const shared_ptr<const Value> &rR);
        };

        shared_ptr<Expression> pIdExpression;

        typedef map<shared_ptr<const Value>,
                vector<shared_ptr<Accumulator>>, KeyComparator> GroupsType;
        GroupsType groups;

        /*
          The field names for the result documents and the accumulator
          factories for the result documents.  The Expressions are the
          common expressions used by each instance of each accumulator
          in order to find the right-hand side of what gets added to the
          accumulator.  Note that each of those is the same for each group,
          so we can share them across all groups by adding them to the
          accumulators after we use the factories to make a new set of
          accumulators for each new group.

          These three vectors parallel each other.
        */
        vector<string> vFieldName;
        vector<shared_ptr<Accumulator> (*)()> vpAccumulatorFactory;
        vector<shared_ptr<Expression>> vpExpression;


        shared_ptr<Document> makeDocument(const GroupsType::iterator &rIter);

        GroupsType::iterator groupsIterator;
        shared_ptr<Document> pCurrent;

        static string idName; // shared _id string
    };


    class DocumentSourceProject :
        public DocumentSource,
        public boost::enable_shared_from_this<DocumentSourceProject> {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceProject();
        virtual bool eof();
        virtual bool advance();
        virtual shared_ptr<Document> getCurrent();
	virtual void optimize();
	virtual void toBson(BSONObjBuilder *pBuilder) const;


        /*
          Create a new DocumentSource that can implement projection.

	  @returns the projection DocumentSource
        */
        static shared_ptr<DocumentSourceProject> create();

        /*
          Add an output Expression to the projection.

          BSON document fields are ordered, so the new field will be
          appended to the existing set.

          @param fieldName the name of the field as it will appear
          @param pExpression the expression used to compute the field
          @param ravelArray if the result of the expression is an array value,
              the projection will create one Document per value in the array;
              a sequence of documents is generated, with each one containing one
              value from the array.  Note there can only be one raveled field.
        */
        void addField(string fieldName, shared_ptr<Expression> pExpression,
                      bool ravelArray);

	/*
	  Create a new projection DocumentSource from BSON.

	  This is a convenience for directly handling BSON, and relies on the
	  above methods.

	  @params pBsonElement the BSONElement with an object named $project
	  @returns the created projection
	 */
        static shared_ptr<DocumentSource> createFromBson(
            BSONElement *pBsonElement);

    private:
        DocumentSourceProject();

        // configuration state
        vector<string> vFieldName; // inclusion field names
        vector<shared_ptr<Expression>> vpExpression; // inclusions
        int ravelWhich; // index of raveled field, if any, otherwise -1

        // iteration state
        shared_ptr<Document> pNoRavelDocument; // document to return, pre-ravel
        shared_ptr<const Value> pRavelArray; // field being raveled
        shared_ptr<ValueIterator> pRavel; // iterator used for raveling
        shared_ptr<const Value> pRavelValue; // current value
    };

    class DocumentSourceBsonArray :
        public DocumentSource {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceBsonArray();
        virtual bool eof();
        virtual bool advance();
        virtual shared_ptr<Document> getCurrent();
	virtual void setSource(shared_ptr<DocumentSource> pSource);

	/*
	  Create a document source based on a BSON array.

	  This is usually put at the beginning of a chain of document sources
	  in order to fetch data from the database.

	  CAUTION:  the BSON is not read until the source is used.  Any
	  elements that appear after these documents must not be read until
	  this source is exhausted.

	  @param pCursor the cursor to use to fetch data
	*/
	static shared_ptr<DocumentSourceBsonArray> create(
	    BSONElement *pBsonElement);

    private:
        DocumentSourceBsonArray(BSONElement *pBsonElement);

	BSONObj embeddedObject;
	BSONObjIterator arrayIterator;
	BSONElement currentElement;
    };
}


/* ======================= INLINED IMPLEMENTATIONS ========================== */

namespace mongo {
    inline bool DocumentSourceGroup::KeyComparator::operator()(
        const shared_ptr<const Value> &rL,
        const shared_ptr<const Value> &rR) {
        return (Value::compare(rL, rR) < 0);
    }

    inline void DocumentSourceGroup::setIdExpression(
        shared_ptr<Expression> pExpression) {
        pIdExpression = pExpression;
    }
}
