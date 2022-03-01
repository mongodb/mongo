/* Test case for bug 563618
 *
 * Uses nsIConverterInputStream to decode invalid EUC-JP text
 *
 */

const { NetUtil } = ChromeUtils.import("resource://gre/modules/NetUtil.jsm");

const test = [
  // 0: 0x8e followed by hi byte, not valid JIS X 0201
  [
    "abcdefghijklmnopqrstuvwxyz12test00%8e%80foobar",
    //    expected: one replacement character, invalid byte eaten
    "abcdefghijklmnopqrstuvwxyz12test00\uFFFDfoobar",
  ],
  // 1: 0x8e followed by ASCII
  [
    "abcdefghijklmnopqrstuvwxyz12test01%8efoobar",
    //    expected: one replacement character, invalid byte not eaten
    "abcdefghijklmnopqrstuvwxyz12test01\uFFFDfoobar",
  ],
  // 2: JIS X 0208 lead byte followed by invalid hi byte
  [
    "abcdefghijklmnopqrstuvwxyz12test02%bf%80foobar",
    //    expected: one replacement character, invalid byte eaten
    "abcdefghijklmnopqrstuvwxyz12test02\uFFFDfoobar",
  ],
  // 3: JIS X 0208 lead byte followed by ASCII
  [
    "abcdefghijklmnopqrstuvwxyz12test03%bffoobar",
    //    expected: one replacement character, invalid byte not eaten
    "abcdefghijklmnopqrstuvwxyz12test03\uFFFDfoobar",
  ],
];

const ConverterInputStream = Components.Constructor(
  "@mozilla.org/intl/converter-input-stream;1",
  "nsIConverterInputStream",
  "init"
);

function testCase(testText, expectedText, bufferLength, charset) {
  var dataURI = "data:text/plain;charset=" + charset + "," + testText;
  var channel = NetUtil.newChannel({
    uri: dataURI,
    loadUsingSystemPrincipal: true,
  });
  var testInputStream = channel.open();
  var testConverter = new ConverterInputStream(
    testInputStream,
    charset,
    bufferLength,
    0xfffd
  );

  if (!(testConverter instanceof Ci.nsIUnicharLineInputStream)) {
    throw new Error("not line input stream");
  }

  var outStr = "";
  var more;
  do {
    // read the line and check for eof
    var line = {};
    more = testConverter.readLine(line);
    outStr += line.value;
  } while (more);

  if (outStr != expectedText) {
    dump("Failed with bufferLength = " + bufferLength + "\n");
    if (outStr.length == expectedText.length) {
      for (let i = 0; i < outStr.length; ++i) {
        if (outStr.charCodeAt(i) != expectedText.charCodeAt(i)) {
          dump(
            i +
              ": " +
              outStr.charCodeAt(i).toString(16) +
              " != " +
              expectedText.charCodeAt(i).toString(16) +
              "\n"
          );
        }
      }
    }
  }

  // escape the strings before comparing for better readability
  Assert.equal(escape(outStr), escape(expectedText));
}

function run_test() {
  for (var i = 0; i < test.length; ++i) {
    for (var bufferLength = 32; bufferLength < 40; ++bufferLength) {
      testCase(test[i][0], test[i][1], bufferLength, "EUC-JP");
    }
  }
}
