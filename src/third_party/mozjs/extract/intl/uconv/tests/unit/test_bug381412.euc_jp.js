const charset = "EUC-JP";
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

function test(inString) {
  var outString = gConverter.ConvertToUnicode(inString) + gConverter.Finish();

  switch (outString.length) {
    case 0:
    case 1:
    case 2:
      error(inString, outString, "Unexpected error");
      break;
    case 3:
      error(inString, outString, "3 byte sequence eaten");
      break;
    case 4:
      if (
        outString.charCodeAt(0) < 0x80 &&
        outString.charCodeAt(1) < 0x80 &&
        outString.charCodeAt(2) < 0x80 &&
        outString.charCodeAt(3) < 0x80
      ) {
        error(inString, outString, "3 byte sequence converted to 1 ASCII");
      }
      break;
    case 5:
      if (
        outString != inString &&
        outString.charCodeAt(0) < 0x80 &&
        outString.charCodeAt(1) < 0x80 &&
        outString.charCodeAt(2) < 0x80 &&
        outString.charCodeAt(3) < 0x80 &&
        outString.charCodeAt(4) < 0x80
      ) {
        error(inString, outString, "3 byte sequence converted to 2 ASCII");
      }
      break;
    case 6:
      if (
        outString != inString &&
        outString.charCodeAt(0) < 0x80 &&
        outString.charCodeAt(1) < 0x80 &&
        outString.charCodeAt(2) < 0x80 &&
        outString.charCodeAt(3) < 0x80 &&
        outString.charCodeAt(4) < 0x80 &&
        outString.charCodeAt(5) < 0x80
      ) {
        error(inString, outString, "3 byte sequence converted to 3 ASCII");
      }
      break;
  }
}

function run_test() {
  gConverter = new ScriptableUnicodeConverter();
  gConverter.charset = charset;

  var byte1, byte2, byte3;
  for (byte1 = 1; byte1 < 0x100; ++byte1) {
    for (byte2 = 1; byte2 < 0x100; ++byte2) {
      if (byte1 == 0x8f) {
        for (byte3 = 1; byte3 < 0x100; ++byte3) {
          test(String.fromCharCode(byte1, byte2, byte3) + "foo");
        }
      } else {
        test(String.fromCharCode(byte1, byte2) + " foo");
      }
    }
  }
}
