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
        virtual boost::shared_ptr<Document> getCurrent() = 0;

	/*
	  Set the underlying source this source should use to get Documents
	  from.

	  It is an error to set the source more than once.  This is to
	  prevent changing sources once the original source has been started;
	  this could break the state maintained by the DocumentSource.

	  @param pSource the underlying source to use
	 */
	virtual void setSource(boost::shared_ptr<DocumentSource> pSource);

	/*
	  Attempt to coalesce this DocumentSource with its successor in the
	  document processing pipeline.  If successful, the successor
	  DocumentSource should be removed from the pipeline and discarded.

	  If successful, this operation can be applied repeatedly, in an
	  attempt to coalesce several sources together.

	  The default implementation is to do nothing, and return false.

	  @param pNextSource the next source in the document processing chain.
	  @returns whether or not the attempt to coalesce was successful or not;
	    if the attempt was not successful, nothing has been changed
	 */
	virtual bool coalesce(boost::shared_ptr<DocumentSource> pNextSource);

	/*
	  Optimize the pipeline operation, if possible.  This is a local
	  optimization that only looks within this DocumentSource.  For best
	  results, first coalesce compatible sources using coalesce().

	  This is intended for any operations that include expressions, and
	  provides a hook for those to optimize those operations.

	  The default implementation is to do nothing.
	 */
	virtual void optimize();

        /*
	  Add the DocumentSource to the array builder.

	  The default implementation calls sourceToBson() in order to
	  convert the inner part of the object which will be added to the
	  array being built here.

	  @params pBuilder the array builder to add the operation to.
         */
	virtual void addToBsonArray(BSONArrayBuilder *pBuilder) const;

    protected:
	/*
	  Create an object that represents the document source.  The object
	  will have a single field whose name is the source's name.  This
	  will be used by the default implementation of addToBsonArray()
	  to add this object to a pipeline being represented in BSON.

	  @params pBuilder a blank object builder to write to
	 */
	virtual void sourceToBson(BSONObjBuilder *pBuilder) const = 0;

	/*
	  Most DocumentSources have an underlying source they get their data
	  from.  This is a convenience for them.

	  The default implementation of setSource() sets this; if you don't
	  need a source, override that to assert().  The default is to
	  assert() if this has already been set.
	*/
	boost::shared_ptr<DocumentSource> pSource;
    };


    class DocumentSourceBsonArray :
        public DocumentSource {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceBsonArray();
        virtual bool eof();
        virtual bool advance();
        virtual boost::shared_ptr<Document> getCurrent();
	virtual void setSource(boost::shared_ptr<DocumentSource> pSource);

	/*
	  Create a document source based on a BSON array.

	  This is usually put at the beginning of a chain of document sources
	  in order to fetch data from the database.

	  CAUTION:  the BSON is not read until the source is used.  Any
	  elements that appear after these documents must not be read until
	  this source is exhausted.

	  @param pCursor the cursor to use to fetch data
	*/
	static boost::shared_ptr<DocumentSourceBsonArray> create(
	    BSONElement *pBsonElement);

    protected:
	// virtuals from DocumentSource
	virtual void sourceToBson(BSONObjBuilder *pBuilder) const;

    private:
        DocumentSourceBsonArray(BSONElement *pBsonElement);

	BSONObj embeddedObject;
	BSONObjIterator arrayIterator;
	BSONElement currentElement;
	bool haveCurrent;
    };


    class DocumentSourceCursor :
        public DocumentSource {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceCursor();
        virtual bool eof();
        virtual bool advance();
        virtual boost::shared_ptr<Document> getCurrent();
	virtual void setSource(boost::shared_ptr<DocumentSource> pSource);

	/*
	  Create a document source based on a cursor.

	  This is usually put at the beginning of a chain of document sources
	  in order to fetch data from the database.

	  @param pCursor the cursor to use to fetch data
	*/
	static boost::shared_ptr<DocumentSourceCursor> create(
	    boost::shared_ptr<Cursor> pCursor);

    protected:
	// virtuals from DocumentSource
	virtual void sourceToBson(BSONObjBuilder *pBuilder) const;

    private:
        DocumentSourceCursor(boost::shared_ptr<Cursor> pTheCursor);

        boost::shared_ptr<Cursor> pCursor;
    };


    class DocumentSourceFilter :
        public DocumentSource {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceFilter();
        virtual bool eof();
        virtual bool advance();
        virtual boost::shared_ptr<Document> getCurrent();
	virtual bool coalesce(boost::shared_ptr<DocumentSource> pNextSource);
	virtual void optimize();

	/*
	  Create a filter.

          @param pBsonElement the raw BSON specification for the filter
          @returns the filter
	 */
	static boost::shared_ptr<DocumentSourceFilter> createFromBson(
	    BSONElement *pBsonElement);

        /*
          Create a filter.

          @param pFilter the expression to use to filter
          @returns the filter
         */
        static boost::shared_ptr<DocumentSourceFilter> create(
            boost::shared_ptr<Expression> pFilter);

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

    protected:
	// virtuals from DocumentSource
	virtual void sourceToBson(BSONObjBuilder *pBuilder) const;

    private:
        DocumentSourceFilter(boost::shared_ptr<Expression> pFilter);

        boost::shared_ptr<Expression> pFilter;

        void findNext();

        bool unstarted;
        bool hasNext;
        boost::shared_ptr<Document> pCurrent;
    };

    class DocumentSourceGroup :
        public DocumentSource {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceGroup();
        virtual bool eof();
        virtual bool advance();
        virtual boost::shared_ptr<Document> getCurrent();

        /*
          Create a new grouping DocumentSource.
	  
	  @returns the DocumentSource
         */
        static boost::shared_ptr<DocumentSourceGroup> create();

        /*
          Set the Id Expression.

          Documents that pass through the grouping Document are grouped
          according to this key.  This will generate the id_ field in the
          result documents.

          @param pExpression the group key
         */
        void setIdExpression(boost::shared_ptr<Expression> pExpression);

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
                        boost::shared_ptr<Accumulator> (*pAccumulatorFactory)(),
                            boost::shared_ptr<Expression> pExpression);

	/*
	  Create a grouping DocumentSource from BSON.

	  This is a convenience method that uses the above, and operates on
	  a BSONElement that has been deteremined to be an Object with an
	  element named $group.

	  @param pBsonElement the BSONELement that defines the group
	  @returns the grouping DocumentSource
	 */
        static boost::shared_ptr<DocumentSource> createFromBson(
	    BSONElement *pBsonElement);


	/*
	  Create a unifying group that can be used to combine group results
	  from shards.

	  @returns the grouping DocumentSource
	*/
	boost::shared_ptr<DocumentSource> createMerger();

    protected:
	// virtuals from DocumentSource
	virtual void sourceToBson(BSONObjBuilder *pBuilder) const;

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
            bool operator()(const boost::shared_ptr<const Value> &rL,
                            const boost::shared_ptr<const Value> &rR);
        };

        boost::shared_ptr<Expression> pIdExpression;

        typedef map<boost::shared_ptr<const Value>,
	    vector<boost::shared_ptr<Accumulator> >, KeyComparator> GroupsType;
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
        vector<boost::shared_ptr<Accumulator> (*)()> vpAccumulatorFactory;
        vector<boost::shared_ptr<Expression> > vpExpression;


        boost::shared_ptr<Document> makeDocument(
	    const GroupsType::iterator &rIter);

        GroupsType::iterator groupsIterator;
        boost::shared_ptr<Document> pCurrent;

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
        virtual boost::shared_ptr<Document> getCurrent();
	virtual void optimize();

        /*
          Create a new DocumentSource that can implement projection.

	  @returns the projection DocumentSource
        */
        static boost::shared_ptr<DocumentSourceProject> create();

        /*
          Add an output Expression to the projection.

          BSON document fields are ordered, so the new field will be
          appended to the existing set.

          @param fieldName the name of the field as it will appear
          @param pExpression the expression used to compute the field
          @param unwindArray if the result of the expression is an array value,
              the projection will create one Document per value in the array;
              a sequence of documents is generated, with each one containing one
              value from the array.  Note there can only be one unwound field
	      per projection.
        */
        void addField(string fieldName,
		      boost::shared_ptr<Expression> pExpression,
                      bool unwindArray);

	/*
	  Create a new projection DocumentSource from BSON.

	  This is a convenience for directly handling BSON, and relies on the
	  above methods.

	  @params pBsonElement the BSONElement with an object named $project
	  @returns the created projection
	 */
        static boost::shared_ptr<DocumentSource> createFromBson(
            BSONElement *pBsonElement);

    protected:
	// virtuals from DocumentSource
	virtual void sourceToBson(BSONObjBuilder *pBuilder) const;

    private:
        DocumentSourceProject();

        // configuration state
        vector<string> vFieldName; // inclusion field names
        vector<boost::shared_ptr<Expression> > vpExpression; // inclusions
        int unwindWhich; // index of field to unwind, if any, otherwise -1

        // iteration state
        boost::shared_ptr<Document> pNoUnwindDocument;
                                              // document to return, pre-unwind
        boost::shared_ptr<const Value> pUnwindArray; // field being unwound
        boost::shared_ptr<ValueIterator> pUnwind; // iterator used for unwinding
        boost::shared_ptr<const Value> pUnwindValue; // current value
    };
}

/* ======================= INLINED IMPLEMENTATIONS ========================== */

namespace mongo {
    inline bool DocumentSourceGroup::KeyComparator::operator()(
        const boost::shared_ptr<const Value> &rL,
        const boost::shared_ptr<const Value> &rR) {
        return (Value::compare(rL, rR) < 0);
    }

    inline void DocumentSourceGroup::setIdExpression(
        boost::shared_ptr<Expression> pExpression) {
        pIdExpression = pExpression;
    }
}
