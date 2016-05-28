//
// A view of a collection against which operations are explained rather than executed
// normally.
//

var Explainable = (function() {

    var parseVerbosity = function(verbosity) {
        // Truthy non-strings are interpreted as "allPlansExecution" verbosity.
        if (verbosity && (typeof verbosity !== "string")) {
            return "allPlansExecution";
        }

        // Falsy non-strings are interpreted as "queryPlanner" verbosity.
        if (!verbosity && (typeof verbosity !== "string")) {
            return "queryPlanner";
        }

        // If we're here, then the verbosity is a string. We reject invalid strings.
        if (verbosity !== "queryPlanner" && verbosity !== "executionStats" &&
            verbosity !== "allPlansExecution") {
            throw Error("explain verbosity must be one of {" + "'queryPlanner'," +
                        "'executionStats'," + "'allPlansExecution'}");
        }

        return verbosity;
    };

    var throwOrReturn = function(explainResult) {
        if (("ok" in explainResult && !explainResult.ok) || explainResult.$err) {
            throw _getErrorWithCode(explainResult, "explain failed: " + tojson(explainResult));
        }

        return explainResult;
    };

    function constructor(collection, verbosity) {
        //
        // Private vars.
        //

        this._collection = collection;
        this._verbosity = parseVerbosity(verbosity);

        //
        // Public methods.
        //

        this.getCollection = function() {
            return this._collection;
        };

        this.getVerbosity = function() {
            return this._verbosity;
        };

        this.setVerbosity = function(verbosity) {
            this._verbosity = parseVerbosity(verbosity);
            return this;
        };

        this.help = function() {
            print("Explainable operations");
            print("\t.aggregate(...) - explain an aggregation operation");
            print("\t.count(...) - explain a count operation");
            print("\t.distinct(...) - explain a distinct operation");
            print("\t.find(...) - get an explainable query");
            print("\t.findAndModify(...) - explain a findAndModify operation");
            print("\t.group(...) - explain a group operation");
            print("\t.remove(...) - explain a remove operation");
            print("\t.update(...) - explain an update operation");
            print("Explainable collection methods");
            print("\t.getCollection()");
            print("\t.getVerbosity()");
            print("\t.setVerbosity(verbosity)");
            return __magicNoPrint;
        };

        //
        // Pretty representations.
        //

        this.toString = function() {
            return "Explainable(" + this._collection.getFullName() + ")";
        };

        this.shellPrint = function() {
            return this.toString();
        };

        //
        // Explainable operations.
        //

        /**
         * Adds "explain: true" to "extraOpts", and then passes through to the regular collection's
         * aggregate helper.
         */
        this.aggregate = function(pipeline, extraOpts) {
            if (!(pipeline instanceof Array)) {
                // support legacy varargs form. (Also handles db.foo.aggregate())
                pipeline = Array.from(arguments);
                extraOpts = {};
            }

            // Add the explain option.
            extraOpts = extraOpts || {};
            extraOpts.explain = true;

            return this._collection.aggregate(pipeline, extraOpts);
        };

        this.count = function(query) {
            return this.find(query).count();
        };

        /**
         * .explain().find() and .find().explain() mean the same thing. In both cases, we use
         * the DBExplainQuery abstraction in order to construct the proper explain command to send
         * to the server.
         */
        this.find = function() {
            var cursor = this._collection.find.apply(this._collection, arguments);
            return new DBExplainQuery(cursor, this._verbosity);
        };

        this.findAndModify = function(params) {
            var famCmd = Object.extend({"findAndModify": this._collection.getName()}, params);
            var explainCmd = {"explain": famCmd, "verbosity": this._verbosity};
            var explainResult = this._collection.runReadCommand(explainCmd);
            return throwOrReturn(explainResult);
        };

        this.group = function(params) {
            params.ns = this._collection.getName();
            var grpCmd = {"group": this._collection.getDB()._groupFixParms(params)};
            var explainCmd = {"explain": grpCmd, "verbosity": this._verbosity};
            var explainResult = this._collection.runReadCommand(explainCmd);
            return throwOrReturn(explainResult);
        };

        this.distinct = function(keyString, query, options) {
            var distinctCmd = {
                distinct: this._collection.getName(),
                key: keyString,
                query: query || {}
            };

            if (options && options.hasOwnProperty("collation")) {
                distinctCmd.collation = options.collation;
            }

            var explainCmd = {explain: distinctCmd, verbosity: this._verbosity};
            var explainResult = this._collection.runReadCommand(explainCmd);
            return throwOrReturn(explainResult);
        };

        this.remove = function() {
            var parsed = this._collection._parseRemove.apply(this._collection, arguments);
            var query = parsed.query;
            var justOne = parsed.justOne;
            var collation = parsed.collation;

            var bulk = this._collection.initializeOrderedBulkOp();
            var removeOp = bulk.find(query);

            if (collation) {
                removeOp.collation(collation);
            }

            if (justOne) {
                removeOp.removeOne();
            } else {
                removeOp.remove();
            }

            var explainCmd = bulk.convertToExplainCmd(this._verbosity);
            var explainResult = this._collection.runCommand(explainCmd);
            return throwOrReturn(explainResult);
        };

        this.update = function() {
            var parsed = this._collection._parseUpdate.apply(this._collection, arguments);
            var query = parsed.query;
            var obj = parsed.obj;
            var upsert = parsed.upsert;
            var multi = parsed.multi;
            var collation = parsed.collation;

            var bulk = this._collection.initializeOrderedBulkOp();
            var updateOp = bulk.find(query);

            if (upsert) {
                updateOp = updateOp.upsert();
            }

            if (collation) {
                updateOp.collation(collation);
            }

            if (multi) {
                updateOp.update(obj);
            } else {
                updateOp.updateOne(obj);
            }

            var explainCmd = bulk.convertToExplainCmd(this._verbosity);
            var explainResult = this._collection.runCommand(explainCmd);
            return throwOrReturn(explainResult);
        };
    }

    //
    // Public static methods.
    //

    constructor.parseVerbosity = parseVerbosity;
    constructor.throwOrReturn = throwOrReturn;

    return constructor;
})();

/**
 * This is the user-facing method for creating an Explainable from a collection.
 */
DBCollection.prototype.explain = function(verbosity) {
    return new Explainable(this, verbosity);
};
