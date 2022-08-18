//
// A DBQuery which is explained rather than executed normally. Also could be thought of as
// an "explainable cursor". Explains of .find() operations run through this abstraction.
//

var DBExplainQuery = (function() {
    //
    // Private methods.
    //

    /**
     * Many of the methods of an explain query just pass through to the underlying
     * non-explained DBQuery. Use this to generate a function which calls function 'name' on
     * 'destObj' and then returns this.
     */
    function createDelegationFunc(explainQuery, dbQuery, name) {
        return function() {
            dbQuery[name].apply(dbQuery, arguments);
            return explainQuery;
        };
    }

    function constructor(query, verbosity) {
        //
        // Private vars.
        //

        this._query = query;
        this._verbosity = Explainable.parseVerbosity(verbosity);
        this._mongo = query._mongo;
        this._finished = false;

        // Used if this query is a count, not a find.
        this._isCount = false;
        this._applySkipLimit = false;

        //
        // Public delegation methods. These just pass through to the underlying
        // DBQuery.
        //

        var delegationFuncNames = [
            "addOption",
            "allowDiskUse",
            "batchSize",
            "collation",
            "comment",
            "hint",
            "limit",
            "max",
            "maxTimeMS",
            "min",
            "readPref",
            "showDiskLoc",
            "skip",
            "sort",
        ];

        // Generate the delegation methods from the list of their names.
        var that = this;
        delegationFuncNames.forEach(function(name) {
            that[name] = createDelegationFunc(that, that._query, name);
        });

        //
        // Core public methods.
        //

        /**
         * Indicates that we are done building the query to explain, and sends the explain
         * command or query to the server.
         *
         * Returns the result of running the explain.
         */
        this.finish = function() {
            if (this._finished) {
                throw Error("query has already been explained");
            }

            // Mark this query as finished. Shouldn't be used for another explain.
            this._finished = true;

            // Explain always gets pretty printed.
            this._query._prettyShell = true;

            // Convert this explain query into an explain command, and send the command to
            // the server.
            var innerCmd;
            if (this._isCount) {
                // True means to always apply the skip and limit values.
                innerCmd = this._query._convertToCountCmd(this._applySkipLimit);
            } else {
                innerCmd = this._query._convertToCommand();
            }

            var explainCmd = {explain: innerCmd};
            explainCmd["verbosity"] = this._verbosity;

            // If "maxTimeMS" or "$readPreference" are  set on 'innerCmd', they need to be
            // propagated to the top-level of 'explainCmd' in order to have the intended effect.
            if (innerCmd.hasOwnProperty("maxTimeMS")) {
                explainCmd.maxTimeMS = innerCmd.maxTimeMS;
            }
            if (innerCmd.hasOwnProperty("$readPreference")) {
                explainCmd["$readPreference"] = innerCmd["$readPreference"];
            }

            var explainDb = this._query._db;
            var explainResult = explainDb.runReadCommand(explainCmd, null, this._query._options);

            return Explainable.throwOrReturn(explainResult);
        };

        this.next = function() {
            return this.finish();
        };

        this.hasNext = function() {
            return !this._finished;
        };

        this.forEach = function(func) {
            while (this.hasNext()) {
                func(this.next());
            }
        };

        /**
         * Returns the explain resulting from running this query as a count operation.
         *
         * If 'applySkipLimit' is true, then the skip and limit values set on this query values are
         * passed to the server; otherwise they are ignored.
         */
        this.count = function(applySkipLimit) {
            this._isCount = true;
            if (applySkipLimit) {
                this._applySkipLimit = true;
            }
            return this.finish();
        };

        /**
         * This gets called automatically by the shell in interactive mode. It should
         * print the result of running the explain.
         */
        this.shellPrint = function() {
            var result = this.finish();
            return tojson(result);
        };

        /**
         * Display help text.
         */
        this.help = function() {
            print("Explain query methods");
            print("\t.finish() - sends explain command to the server and returns the result");
            print("\t.forEach(func) - apply a function to the explain results");
            print("\t.hasNext() - whether this explain query still has a result to retrieve");
            print("\t.next() - alias for .finish()");
            print("Explain query modifiers");
            print("\t.addOption(n)");
            print("\t.batchSize(n)");
            print("\t.comment(comment)");
            print("\t.collation(collationSpec)");
            print("\t.count()");
            print("\t.hint(hintSpec)");
            print("\t.limit(n)");
            print("\t.maxTimeMS(n)");
            print("\t.max(idxDoc)");
            print("\t.min(idxDoc)");
            print("\t.readPref(mode, tagSet)");
            print("\t.showDiskLoc()");
            print("\t.skip(n)");
            print("\t.sort(sortSpec)");
            return __magicNoPrint;
        };
    }

    return constructor;
})();
