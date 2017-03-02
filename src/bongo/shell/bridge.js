/**
 * Wrapper around a bongobridge process. Construction of a BongoBridge instance will start a new
 * bongobridge process that listens on 'options.port' and forwards messages to 'options.dest'.
 *
 * @param {Object} options
 * @param {string} options.dest - The host:port to forward messages to.
 * @param {string} [options.hostName=localhost] - The hostname to specify when connecting to the
 * bongobridge process.
 * @param {number} [options.port=allocatePort()] - The port number the bongobridge should listen on.
 *
 * @returns {Proxy} Acts as a typical connection object to options.hostName:options.port that has
 * additional functions exposed to shape network traffic from other processes.
 */
function BongoBridge(options) {
    'use strict';

    if (!(this instanceof BongoBridge)) {
        return new BongoBridge(options);
    }

    options = options || {};
    if (!options.hasOwnProperty('dest')) {
        throw new Error('Missing required field "dest"');
    }

    var hostName = options.hostName || 'localhost';

    this.dest = options.dest;
    this.port = options.port || allocatePort();

    // The connection used by a test for running commands against the bongod or bongos process.
    var userConn;

    // A separate (hidden) connection for configuring the bongobridge process.
    var controlConn;

    // Start the bongobridge on port 'this.port' routing network traffic to 'this.dest'.
    var args = ['bongobridge', '--port', this.port, '--dest', this.dest];
    var keysToSkip = [
        'dest',
        'hostName',
        'port',
    ];

    // Append any command line arguments that are optional for bongobridge.
    Object.keys(options).forEach(function(key) {
        if (Array.contains(keysToSkip, key)) {
            return;
        }

        var value = options[key];
        if (value === null || value === undefined) {
            throw new Error("Value '" + value + "' for '" + key + "' option is ambiguous; specify" +
                            " {flag: ''} to add --flag command line options'");
        }

        args.push('--' + key);
        if (value !== '') {
            args.push(value.toString());
        }
    });

    var pid = _startBongoProgram.apply(null, args);

    /**
     * Initializes the bongo shell's connections to the bongobridge process. Throws an error if the
     * bongobridge process stopped running or if a connection cannot be made.
     *
     * The bongod or bongos process corresponding to this bongobridge process may need to connect to
     * itself through the bongobridge process, e.g. when running the _isSelf command. This means
     * the bongobridge process needs to be running prior to the other process. However, to avoid
     * spurious failures during situations where the bongod or bongos process is not ready to accept
     * connections, connections to the bongobridge process should only be made after the other
     * process is known to be reachable:
     *
     *     var bridge = new BongoBridge(...);
     *     var conn = BongoRunner.runBongoXX(...);
     *     assert.neq(null, conn);
     *     bridge.connectToBridge();
     */
    this.connectToBridge = function connectToBridge() {
        var failedToStart = false;
        assert.soon(() => {
            if (!checkProgram(pid)) {
                failedToStart = true;
                return true;
            }

            try {
                userConn = new Bongo(hostName + ':' + this.port);
            } catch (e) {
                return false;
            }
            return true;
        }, 'failed to connect to the bongobridge on port ' + this.port);
        assert(!failedToStart, 'bongobridge failed to start on port ' + this.port);

        // The BongoRunner.runBongoXX() functions define a 'name' property on the returned
        // connection object that is equivalent to its 'host' property. Certain functions in
        // ReplSetTest and ShardingTest use the 'name' property instead of the 'host' property, so
        // we define it here for consistency.
        Object.defineProperty(userConn, 'name', {
            enumerable: true,
            get: function() {
                return this.host;
            },
        });

        controlConn = new Bongo(hostName + ':' + this.port);
    };

    /**
     * Terminates the bongobridge process.
     */
    this.stop = function stop() {
        _stopBongoProgram(this.port);
    };

    // Throws an error if 'obj' is not a BongoBridge instance.
    function throwErrorIfNotBongoBridgeInstance(obj) {
        if (!(obj instanceof BongoBridge)) {
            throw new Error('Expected BongoBridge instance, but got ' + tojson(obj));
        }
    }

    // Runs a command intended to configure the bongobridge.
    function runBridgeCommand(conn, cmdName, cmdArgs) {
        // The wire version of this bongobridge is detected as the wire version of the corresponding
        // bongod or bongos process because the message is simply forwarded to that process.
        // Commands to configure the bongobridge process must support being sent as an OP_QUERY
        // message in order to handle when the bongobridge is a proxy for a bongos process or when
        // --readMode=legacy is passed to the bongo shell. Create a new Object with 'cmdName' as the
        // first key and $forBridge=true.
        var cmdObj = {};
        cmdObj[cmdName] = 1;
        cmdObj.$forBridge = true;
        Object.extend(cmdObj, cmdArgs);

        var dbName = 'test';
        var noQueryOptions = 0;
        return conn.runCommand(dbName, cmdObj, noQueryOptions);
    }

    /**
     * Allows communication between 'this.dest' and the 'dest' of each of the 'bridges'.
     *
     * Configures 'this' bridge to accept new connections from the 'dest' of each of the 'bridges'.
     * Additionally configures each of the 'bridges' to accept new connections from 'this.dest'.
     *
     * @param {(BongoBridge|BongoBridge[])} bridges
     */
    this.reconnect = function reconnect(bridges) {
        if (!Array.isArray(bridges)) {
            bridges = [bridges];
        }
        bridges.forEach(throwErrorIfNotBongoBridgeInstance);

        this.acceptConnectionsFrom(bridges);
        bridges.forEach(bridge => bridge.acceptConnectionsFrom(this));
    };

    /**
     * Disallows communication between 'this.dest' and the 'dest' of each of the 'bridges'.
     *
     * Configures 'this' bridge to close existing connections and reject new connections from the
     * 'dest' of each of the 'bridges'. Additionally configures each of the 'bridges' to close
     * existing connections and reject new connections from 'this.dest'.
     *
     * @param {(BongoBridge|BongoBridge[])} bridges
     */
    this.disconnect = function disconnect(bridges) {
        if (!Array.isArray(bridges)) {
            bridges = [bridges];
        }
        bridges.forEach(throwErrorIfNotBongoBridgeInstance);

        this.rejectConnectionsFrom(bridges);
        bridges.forEach(bridge => bridge.rejectConnectionsFrom(this));
    };

    /**
     * Configures 'this' bridge to accept new connections from the 'dest' of each of the 'bridges'.
     *
     * @param {(BongoBridge|BongoBridge[])} bridges
     */
    this.acceptConnectionsFrom = function acceptConnectionsFrom(bridges) {
        if (!Array.isArray(bridges)) {
            bridges = [bridges];
        }
        bridges.forEach(throwErrorIfNotBongoBridgeInstance);

        bridges.forEach(bridge => {
            var res = runBridgeCommand(controlConn, 'acceptConnectionsFrom', {host: bridge.dest});
            assert.commandWorked(res,
                                 'failed to configure the bongobridge listening on port ' +
                                     this.port + ' to accept new connections from ' + bridge.dest);
        });
    };

    /**
     * Configures 'this' bridge to close existing connections and reject new connections from the
     * 'dest' of each of the 'bridges'.
     *
     * @param {(BongoBridge|BongoBridge[])} bridges
     */
    this.rejectConnectionsFrom = function rejectConnectionsFrom(bridges) {
        if (!Array.isArray(bridges)) {
            bridges = [bridges];
        }
        bridges.forEach(throwErrorIfNotBongoBridgeInstance);

        bridges.forEach(bridge => {
            var res = runBridgeCommand(controlConn, 'rejectConnectionsFrom', {host: bridge.dest});
            assert.commandWorked(res,
                                 'failed to configure the bongobridge listening on port ' +
                                     this.port + ' to hang up connections from ' + bridge.dest);
        });
    };

    /**
     * Configures 'this' bridge to delay forwarding requests from the 'dest' of each of the
     * 'bridges' to 'this.dest' by the specified amount.
     *
     * @param {(BongoBridge|BongoBridge[])} bridges
     * @param {number} delay - The delay to apply in milliseconds.
     */
    this.delayMessagesFrom = function delayMessagesFrom(bridges, delay) {
        if (!Array.isArray(bridges)) {
            bridges = [bridges];
        }
        bridges.forEach(throwErrorIfNotBongoBridgeInstance);

        bridges.forEach(bridge => {
            var res = runBridgeCommand(controlConn, 'delayMessagesFrom', {
                host: bridge.dest,
                delay: delay,
            });
            assert.commandWorked(res,
                                 'failed to configure the bongobridge listening on port ' +
                                     this.port + ' to delay messages from ' + bridge.dest + ' by ' +
                                     delay + ' milliseconds');
        });
    };

    /**
     * Configures 'this' bridge to uniformly discard requests from the 'dest' of each of the
     * 'bridges' to 'this.dest' with probability 'lossProbability'.
     *
     * @param {(BongoBridge|BongoBridge[])} bridges
     * @param {number} lossProbability
     */
    this.discardMessagesFrom = function discardMessagesFrom(bridges, lossProbability) {
        if (!Array.isArray(bridges)) {
            bridges = [bridges];
        }
        bridges.forEach(throwErrorIfNotBongoBridgeInstance);

        bridges.forEach(bridge => {
            var res = runBridgeCommand(controlConn, 'discardMessagesFrom', {
                host: bridge.dest,
                loss: lossProbability,
            });
            assert.commandWorked(res,
                                 'failed to configure the bongobridge listening on port ' +
                                     this.port + ' to discard messages from ' + bridge.dest +
                                     ' with probability ' + lossProbability);
        });
    };

    // Use a Proxy to "extend" the underlying connection object. The C++ functions, e.g.
    // runCommand(), require that they are called on the Bongo instance itself and so typical
    // prototypical inheritance isn't possible.
    return new Proxy(this, {
        get: function get(target, property, receiver) {
            // If the property is defined on the BongoBridge instance itself, then
            // return it.
            // Otherwise, get the value of the property from the Bongo instance.
            if (target.hasOwnProperty(property)) {
                return target[property];
            }
            var value = userConn[property];
            if (typeof value === 'function') {
                return value.bind(userConn);
            }
            return value;
        },

        set: function set(target, property, value, receiver) {
            // Delegate setting the value of any property to the Bongo instance so
            // that it can be
            // accessed in functions acting on the Bongo instance directly instead of
            // this Proxy.
            // For example, the "slaveOk" property needs to be set on the Bongo
            // instance in order
            // for the query options bit to be set correctly.
            userConn[property] = value;
            return true;
        },
    });
}
