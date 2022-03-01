var CC = Components.Constructor;
var converter = Cc[
  "@mozilla.org/intl/scriptableunicodeconverter"
].createInstance(Ci.nsIScriptableUnicodeConverter);
converter.charset = "UTF-8";

var SIS = CC(
  "@mozilla.org/scriptableinputstream;1",
  "nsIScriptableInputStream",
  "init"
);

function test_char(code) {
  dump("test_char(0x" + code.toString(16) + ")\n");
  var original = String.fromCharCode(code);
  var nativeStream = converter.convertToInputStream(original);
  var stream = new SIS(nativeStream);
  var utf8Result = stream.read(stream.available());
  stream.close();
  var result = converter.ConvertToUnicode(utf8Result);
  Assert.equal(escape(original), escape(result));
}

function run_test() {
  // This is not a very comprehensive test.
  for (var i = 0x007f - 2; i <= 0x007f; i++) {
    test_char(i);
  }
  for (i = 0x07ff - 2; i <= 0x07ff; i++) {
    test_char(i);
  }
  for (i = 0x1000 - 2; i <= 0x1000 + 2; i++) {
    test_char(i);
  }
  for (i = 0xe000; i <= 0xe000 + 2; i++) {
    test_char(i);
  }
}
