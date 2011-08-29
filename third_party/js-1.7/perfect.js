// Some simple testing of new, eval and some string stuff.

// constructor -- expression array initialization
function ExprArray(n,v)
{
    // Initializes n values to v coerced to a string.
    for (var i = 0; i < n; i++) {
	this[i] = "" + v;
    }
}


// Print the perfect numbers up to n and the sum expression for n's divisors.
function perfect(n)
{
    print("The perfect numbers up to " +  n + " are:");

    // We build sumOfDivisors[i] to hold a string expression for
    // the sum of the divisors of i, excluding i itself.
    var sumOfDivisors = new ExprArray(n+1,1);
    for (var divisor = 2; divisor <= n; divisor++) {
	for (var j = divisor + divisor; j <= n; j += divisor) {
	    sumOfDivisors[j] += " + " + divisor;
	}
	// At this point everything up to 'divisor' has its sumOfDivisors
	// expression calculated, so we can determine whether it's perfect
	// already by evaluating.
	if (eval(sumOfDivisors[divisor]) == divisor) {
	    print("" + divisor + " = " + sumOfDivisors[divisor]);
	}
    }
    print("That's all.");
}


print("\nA number is 'perfect' if it is equal to the sum of its")
print("divisors (excluding itself).\n");
perfect(500);

