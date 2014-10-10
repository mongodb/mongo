// SERVER-6649 - issues round-tripping strings with embedded NUL bytes

t = db.string_with_nul_bytes.js;
t.drop();

string = "string with a NUL (\0) byte";
t.insert({str:string});
assert.eq(t.findOne().str, string);
assert.eq(t.findOne().str.length, string.length); // just to be sure
