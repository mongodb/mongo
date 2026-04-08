import {describe, it} from "jstests/libs/mochalite.js";
import {checkLog} from "src/mongo/shell/check_log.js";

["formatAsJsonLogLine", "formatAsLogLine"].forEach((format) => {
    describe(`${format} with NumberLong`, function () {
        it("handles NumberLong(0)", function () {
            assert.eq(checkLog[format](NumberLong(0)), "0");
        });

        it("handles single-digit NumberLong(5)", function () {
            assert.eq(checkLog[format](NumberLong(5)), "5");
        });

        it("handles double-digit NumberLong(42)", function () {
            assert.eq(checkLog[format](NumberLong(42)), "42");
        });

        it("handles triple-digit NumberLong(123)", function () {
            assert.eq(checkLog[format](NumberLong(123)), "123");
        });

        it("handles negative single-digit NumberLong(-5)", function () {
            assert.eq(checkLog[format](NumberLong(-5)), "-5");
        });

        it("handles negative double-digit NumberLong(-42)", function () {
            assert.eq(checkLog[format](NumberLong(-42)), "-42");
        });

        it("handles negative triple-digit NumberLong(-123)", function () {
            assert.eq(checkLog[format](NumberLong(-123)), "-123");
        });

        it("handles unquoted just below positive threshold NumberLong(2147483647)", function () {
            assert.eq(checkLog[format](NumberLong(2147483647)), "2147483647");
        });

        it("handles quoted at positive threshold NumberLong(2147483648)", function () {
            assert.eq(checkLog[format](NumberLong("2147483648")), "2147483648");
        });

        it("handles unquoted just above negative threshold NumberLong(-2147483647)", function () {
            assert.eq(checkLog[format](NumberLong(-2147483647)), "-2147483647");
        });

        it("handles quoted at negative threshold NumberLong(-2147483648)", function () {
            assert.eq(checkLog[format](NumberLong("-2147483648")), "-2147483648");
        });

        it('handles large positive NumberLong("9876543210000")', function () {
            assert.eq(checkLog[format](NumberLong("9876543210000")), "9876543210000");
        });

        it('handles large negative NumberLong("-9876543210000")', function () {
            assert.eq(checkLog[format](NumberLong("-9876543210000")), "-9876543210000");
        });

        it('handles very large positive NumberLong("9223372036854775807")', function () {
            assert.eq(checkLog[format](NumberLong("9223372036854775807")), "9223372036854775807");
        });

        it('handles very large negative NumberLong("-9223372036854775807")', function () {
            assert.eq(checkLog[format](NumberLong("-9223372036854775807")), "-9223372036854775807");
        });
    });
});
