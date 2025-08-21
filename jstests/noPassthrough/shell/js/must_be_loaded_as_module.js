/**
 * This file needs to be loaded as a module.
 * When loaded as a script, it will fail to parse on a SyntaxError, and should retry as a module.
 */

let fn = (val) => {
    return Promise.resolve(val);
};
Object.assign({}, await fn(42));
