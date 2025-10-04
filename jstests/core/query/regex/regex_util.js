// Tests for RegExp.escape

let TEST_STRINGS = [
    "[db]",
    "{ab}",
    "<c2>",
    "(abc)",
    "^first^",
    "&addr",
    "k@10gen.com",
    "#4",
    "!b",
    "<>3",
    "****word+",
    "\t| |\n\r",
    "Mongo-db",
    "[{(<>)}]!@#%^&*+\\",
];

TEST_STRINGS.forEach(function (str) {
    let escaped = RegExp.escape(str);
    let regex = new RegExp(escaped);
    assert(regex.test(str), "Wrong escape for " + str);
});
