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

#pragma once

#include "../pch.h"

#include "jsobj.h"
#include "../util/timer.h"
#include "../commands.h"

namespace mongo {
    class BSONObj;
    class BSONObjBuilder;
    class DocumentSource;
    class DocumentSourceProject;
    class Expression;
    class ExpressionNary;
    struct OpDesc; // local private struct

    /** mongodb "commands" (sent via db.$cmd.findOne(...))
        subclass to make a command.  define a singleton object for it.
        */
    class Pipeline :
        public Command {
    public:
        // virtuals from Command
        virtual bool run(const string& db, BSONObj& cmdObj, string& errmsg,
                         BSONObjBuilder& result, bool fromRepl);
        virtual LockType locktype() const;
        virtual bool slaveOk() const;
        virtual void help(stringstream& help) const;
        virtual ~Pipeline();

        Pipeline();

	class Spec :
     	    boost::noncopyable {
	public:
	    static shared_ptr<Spec> parseCommand(
		string &errmsg, BSONObj &cmdObj);

	    /*
	      Get the collection name from the command.

	      @returns the collection name
	     */
	    string getCollectionName() const;

	    /*
	      Split the current spec into a pipeline for each shard, and
	      a pipeline that combines the results within mongos.

	      This permanently alters the current spec for the merging
	      operation.

	      @returns the Spec for the pipeline command that should be sent
	        to the shards
	     */
	    shared_ptr<Spec> splitForSharded();

	    /*
	      Write the Spec as a BSONObj command.  This should be the
	      inverse of parseCommand().

	      This is only intended to be used by the shard command obtained
	      from splitForSharded().  Some pipeline operations in the merge
	      process do not have equivalent command forms, and using this on
	      the mongos Spec will cause assertions.

	      @param the builder to write the command to
	     */
	    void toBson(BSONObjBuilder *pBuilder) const;

	    /*
	      Run the Spec on the given source.

	      @param result builder to write the result to
	      @param errmsg place to put error messages, if any
	      @param pSource the document source to use at the head of the chain
	      @returns true on success, false if an error occurs
	     */
	    bool run(BSONObjBuilder &result, string &errmsg,
		shared_ptr<DocumentSource> pSource) const;

	private:
	    Spec();

	    string collectionName;
	    vector<shared_ptr<DocumentSource>> vpSource;
	};

    private:
    };

} // namespace mongo


/* ======================= INLINED IMPLEMENTATIONS ========================== */

namespace mongo {

    inline string Pipeline::Spec::getCollectionName() const {
	return collectionName;
    }

} // namespace mongo


