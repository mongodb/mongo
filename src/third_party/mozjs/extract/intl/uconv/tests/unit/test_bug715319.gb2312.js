const charset = "GB2312";
const ScriptableUnicodeConverter = Components.Constructor(
  "@mozilla.org/intl/scriptableunicodeconverter",
  "nsIScriptableUnicodeConverter"
);
var gConverter;

function error(inString, outString, msg) {
  var dispIn = "";
  var dispOut = "";
  var i;
  for (i = 0; i < inString.length; ++i) {
    dispIn += " x" + inString.charCodeAt(i).toString(16);
  }
  if (!outString.length) {
    dispOut = "<empty>";
  } else {
    for (i = 0; i < outString.length; ++i) {
      dispOut += " x" + outString.charCodeAt(i).toString(16);
    }
  }
  dump('"' + dispIn + '" ==> "' + dispOut + '"\n');
  do_throw("security risk: " + msg);
}

function IsASCII(charCode) {
  return charCode <= 0x7e;
}

function test(inString) {
  var outString = gConverter.ConvertToUnicode(inString) + gConverter.Finish();

  var outLen = outString.length;
  for (var pos = 1; pos < 3; ++pos) {
    let outPos = outLen - (9 - pos);
    if (outPos < 0) {
      outPos = 0;
    }
    let c0 = inString.charCodeAt(0);
    let c1 = inString.charCodeAt(1);
    let c2 = inString.charCodeAt(2);
    let c3 = inString.charCodeAt(3);
    if (
      IsASCII(inString.charCodeAt(pos)) &&
      !(
        outString.charCodeAt(outPos) == inString.charCodeAt(pos) ||
        outString.charCodeAt(outPos) != 0xfffd ||
        // legal 4 byte range
        (0x81 <= c0 &&
          c0 <= 0xfe &&
          0x30 <= c1 &&
          c1 <= 0x39 &&
          0x81 <= c2 &&
          c2 <= 0xfe &&
          0x30 <= c3 &&
          c3 <= 0x39)
      )
    ) {
      dump("pos = " + pos + "; outPos = " + outPos + "\n");
      error(inString, outString, "ASCII input eaten");
    }
  }
}

function run_test() {
  gConverter = new ScriptableUnicodeConverter();
  gConverter.charset = charset;

  var byte1, byte2, byte3, byte4;

  // 2-byte
  for (byte1 = 1; byte1 < 0x100; ++byte1) {
    for (byte2 = 1; byte2 < 0x100; ++byte2) {
      test(String.fromCharCode(byte1, byte2) + "    foo");
    }
  }
  // 4-byte (limited)
  for (byte1 = 0x80; byte1 < 0x90; ++byte1) {
    for (byte2 = 0x20; byte2 < 0x40; ++byte2) {
      for (byte3 = 0x80; byte3 < 0x90; ++byte3) {
        for (byte4 = 0x20; byte4 < 0x40; ++byte4) {
          test(String.fromCharCode(byte1, byte2, byte3, byte4) + "  foo");
        }
      }
    }
  }
}
