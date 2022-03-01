/* Test case for bug 340714
 *
 * Uses nsIConverterInputStream to decode UTF-16 text with all combinations
 * of UTF-16BE and UTF-16LE with and without BOM.
 *
 * Sample text is: "Все счастливые семьи похожи друг на друга, каждая несчастливая семья несчастлива по-своему."
 *
 * The enclosing quotation marks are included in the sample text to test that
 * UTF-16LE is recognized even when there is no BOM and the UTF-16LE decoder is
 * not explicitly called. This only works when the first character of the text
 * is an eight-bit character.
 */

const { NetUtil } = ChromeUtils.import("resource://gre/modules/NetUtil.jsm");

const beBOM = "%FE%FF";
const leBOM = "%FF%FE";
const sampleUTF16BE =
  "%00%22%04%12%04%41%04%35%00%20%04%41%04%47%04%30%04%41%04%42%04%3B%04%38%04%32%04%4B%04%35%00%20%04%41%04%35%04%3C%04%4C%04%38%00%20%04%3F%04%3E%04%45%04%3E%04%36%04%38%00%20%04%34%04%40%04%43%04%33%00%20%04%3D%04%30%00%20%04%34%04%40%04%43%04%33%04%30%00%2C%00%20%04%3A%04%30%04%36%04%34%04%30%04%4F%00%20%04%3D%04%35%04%41%04%47%04%30%04%41%04%42%04%3B%04%38%04%32%04%30%04%4F%00%20%04%41%04%35%04%3C%04%4C%04%4F%00%20%04%3D%04%35%04%41%04%47%04%30%04%41%04%42%04%3B%04%38%04%32%04%30%00%20%04%3F%04%3E%00%2D%04%41%04%32%04%3E%04%35%04%3C%04%43%00%2E%00%22";
const sampleUTF16LE =
  "%22%00%12%04%41%04%35%04%20%00%41%04%47%04%30%04%41%04%42%04%3B%04%38%04%32%04%4B%04%35%04%20%00%41%04%35%04%3C%04%4C%04%38%04%20%00%3F%04%3E%04%45%04%3E%04%36%04%38%04%20%00%34%04%40%04%43%04%33%04%20%00%3D%04%30%04%20%00%34%04%40%04%43%04%33%04%30%04%2C%00%20%00%3A%04%30%04%36%04%34%04%30%04%4F%04%20%00%3D%04%35%04%41%04%47%04%30%04%41%04%42%04%3B%04%38%04%32%04%30%04%4F%04%20%00%41%04%35%04%3C%04%4C%04%4F%04%20%00%3D%04%35%04%41%04%47%04%30%04%41%04%42%04%3B%04%38%04%32%04%30%04%20%00%3F%04%3E%04%2D%00%41%04%32%04%3E%04%35%04%3C%04%43%04%2E%00%22%00";
const expected =
  '"\u0412\u0441\u0435 \u0441\u0447\u0430\u0441\u0442\u043B\u0438\u0432\u044B\u0435 \u0441\u0435\u043C\u044C\u0438 \u043F\u043E\u0445\u043E\u0436\u0438 \u0434\u0440\u0443\u0433 \u043D\u0430 \u0434\u0440\u0443\u0433\u0430, \u043A\u0430\u0436\u0434\u0430\u044F \u043D\u0435\u0441\u0447\u0430\u0441\u0442\u043B\u0438\u0432\u0430\u044F \u0441\u0435\u043C\u044C\u044F \u043D\u0435\u0441\u0447\u0430\u0441\u0442\u043B\u0438\u0432\u0430 \u043F\u043E-\u0441\u0432\u043E\u0435\u043C\u0443."';

Services.prefs.setBoolPref("security.allow_eval_with_system_principal", true);
registerCleanupFunction(() => {
  Services.prefs.clearUserPref("security.allow_eval_with_system_principal");
});

function makeText(withBOM, charset) {
  // eslint-disable-next-line no-eval
  var theText = eval("sample" + charset);
  if (withBOM) {
    if (charset == "UTF16BE") {
      theText = beBOM + theText;
    } else {
      theText = leBOM + theText;
    }
  }
  return theText;
}

function testCase(withBOM, charset, charsetDec, decoder, bufferLength) {
  var dataURI =
    "data:text/plain;charset=" + charsetDec + "," + makeText(withBOM, charset);

  var ConverterInputStream = Components.Constructor(
    "@mozilla.org/intl/converter-input-stream;1",
    "nsIConverterInputStream",
    "init"
  );

  var channel = NetUtil.newChannel({
    uri: dataURI,
    loadUsingSystemPrincipal: true,
  });
  var testInputStream = channel.open();
  var testConverter = new ConverterInputStream(
    testInputStream,
    decoder,
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

  if (outStr != expected) {
    dump(
      "Failed with BOM = " +
        withBOM +
        "; charset = " +
        charset +
        "; charset declaration = " +
        charsetDec +
        "; decoder = " +
        decoder +
        "; bufferLength = " +
        bufferLength +
        "\n"
    );
    if (outStr.length == expected.length) {
      for (let i = 0; i < outStr.length; ++i) {
        if (outStr.charCodeAt(i) != expected.charCodeAt(i)) {
          dump(
            i +
              ": " +
              outStr.charCodeAt(i).toString(16) +
              " != " +
              expected.charCodeAt(i).toString(16) +
              "\n"
          );
        }
      }
    }
  }

  // escape the strings before comparing for better readability
  Assert.equal(escape(outStr), escape(expected));
}

function run_test() {
  /*       BOM    charset    charset   decoder     buffer
                               declaration           length */
  testCase(true, "UTF16LE", "UTF-16", "UTF-16BE", 64);
  testCase(true, "UTF16BE", "UTF-16", "UTF-16LE", 64);
  testCase(true, "UTF16LE", "UTF-16", "UTF-16LE", 64);
  testCase(true, "UTF16BE", "UTF-16", "UTF-16BE", 64);
  testCase(false, "UTF16LE", "UTF-16", "UTF-16LE", 64);
  testCase(false, "UTF16BE", "UTF-16", "UTF-16BE", 64);
  testCase(true, "UTF16LE", "UTF-16", "UTF-16BE", 65);
  testCase(true, "UTF16BE", "UTF-16", "UTF-16LE", 65);
  testCase(true, "UTF16LE", "UTF-16", "UTF-16LE", 65);
  testCase(true, "UTF16BE", "UTF-16", "UTF-16BE", 65);
  testCase(false, "UTF16LE", "UTF-16", "UTF-16LE", 65);
  testCase(false, "UTF16BE", "UTF-16", "UTF-16BE", 65);
}
