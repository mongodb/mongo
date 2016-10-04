'use strict';

/**
 * errors.js
 *
 * This file defines custom errors.
 */

function IterationEnd(message) {
    this.name = 'IterationEnd';
    this.message = message || 'Iteration instructed to terminate';
    this.stack = (new Error()).stack;
}

IterationEnd.prototype = Object.create(Error.prototype);
