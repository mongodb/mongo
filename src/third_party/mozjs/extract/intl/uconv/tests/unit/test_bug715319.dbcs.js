// 2-byte charsets:
const charsets = ["Big5", "EUC-KR"];
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

  if (
    IsASCII(inString.charCodeAt(1)) &&
    (outLen < 4 || outString.charCodeAt(outLen - 4) == 0xfffd)
  ) {
    error(inString, outString, "ASCII input eaten in " + gConverter.charset);
  }
}

function run_test() {
  gConverter = new ScriptableUnicodeConverter();
  for (var i = 0; i < charsets.length; ++i) {
    gConverter.charset = charsets[i];

    var byte1, byte2;
    for (byte1 = 1; byte1 < 0x100; ++byte1) {
      for (byte2 = 1; byte2 < 0x100; ++byte2) {
        test(String.fromCharCode(byte1, byte2) + "foo");
      }
    }
  }
}
