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

function IsASCII(charCode) {
  return charCode <= 0x7e;
}

function IsNotGR(charCode) {
  return charCode < 0xa1 || charCode > 0xfe;
}

function test(inString) {
  var outString = gConverter.ConvertToUnicode(inString) + gConverter.Finish();

  var outLen = outString.length;
  if (
    IsASCII(inString.charCodeAt(1)) &&
    inString.charCodeAt(1) != outString.charCodeAt(outLen - 5)
  ) {
    error(inString, outString, "ASCII second byte eaten");
  } else if (
    IsASCII(inString.charCodeAt(2)) &&
    inString.charCodeAt(2) != outString.charCodeAt(outLen - 4)
  ) {
    error(inString, outString, "ASCII third byte eaten");
  } else if (
    inString.charCodeAt(0) == 0x8f &&
    inString.charCodeAt(1) > 0x7f &&
    IsNotGR(inString.charCodeAt(2)) &&
    !(
      outString.charCodeAt(outLen - 4) == 0xfffd ||
      outString.charCodeAt(outLen - 4) == inString.charCodeAt(2)
    )
  ) {
    error(inString, outString, "non-GR third byte eaten");
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
