// The Y combinator, applied to the factorial function

function factorial(proc) {
    return function (n) {
        return (n <= 1) ? 1 : n * proc(n-1);
    }
}

function Y(outer) {
    function inner(proc) {
        function apply(arg) {
            return proc(proc)(arg);
        }
        return outer(apply);
    }
    return inner(inner);
}

print("5! is " + Y(factorial)(5));
