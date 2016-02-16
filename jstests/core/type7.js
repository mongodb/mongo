(function(){
    "use strict";

    // SERVER-20332 make JS NumberLong more robust
    //
    // Make sure swapping floatApprox, top and bottom don't break NumberLong

    // Picking 2^54 because it's representable as a double (as a power of
    // two), but big enough that the NumberLong code doesn't know it (numbers
    // over 2^53 can lose precision)
    var number = NumberLong("18014398509481984");

    {
        // Make sure all elements in a new NumberLong are valid

        assert.eq(number.floatApprox, 18014398509481984);
        assert.eq(number.top, 4194304);
        assert.eq(number.bottom, 0);
        assert.eq(number.valueOf(), 18014398509481984);
    }

    {
        // Make sure that making top and bottom invalid sets us to zero

        number.top = "a";
        number.bottom = "b";

        assert.eq(number.valueOf(), 0);
    }

    {
        // Make sure we fall back to floatApprox

        delete number.top;
        delete number.bottom;

        assert.eq(number.valueOf(), 18014398509481984);
    }

    {
        // Try breaking floatApprox

        number.floatApprox = "c";

        assert.eq(number.valueOf(), 0);
    }

    {
        // Try setting floatApprox to a non-JS number

        number.floatApprox = NumberLong("10");

        assert.eq(number.valueOf(), 10);
    }

    {
        // Try putting it all back together, with non-JS top/bottom

        number.floatApprox = 18014398509481984;
        number.top = NumberInt("4194304");
        number.bottom = NumberInt("0");

        assert.eq(number.valueOf(), 18014398509481984);
    }
})();
