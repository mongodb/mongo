const charset = "Big5-HKSCS";

function dumpStrings(inString, outString) {
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
}

function error(inString, outString, msg) {
  dumpStrings(inString, outString);
  do_throw("security risk: " + msg);
}

function run_test() {
  var ScriptableUnicodeConverter = Components.Constructor(
    "@mozilla.org/intl/scriptableunicodeconverter",
    "nsIScriptableUnicodeConverter"
  );

  var converter = new ScriptableUnicodeConverter();
  converter.charset = charset;

  var leadByte, trailByte;
  var inString;
  for (leadByte = 1; leadByte < 0x100; ++leadByte) {
    for (trailByte = 1; trailByte < 0x100; ++trailByte) {
      inString = String.fromCharCode(leadByte, trailByte, 65);
      var outString = converter.ConvertToUnicode(inString) + converter.Finish();
      switch (outString.length) {
        case 1:
          error(inString, outString, "2 byte sequence eaten");
          break;
        case 2:
          if (
            outString.charCodeAt(0) < 0x80 &&
            outString.charCodeAt(1) < 0x80
          ) {
            error(inString, outString, "2 byte sequence converted to 1 ASCII");
          }
          break;
        case 3:
          if (
            outString != inString &&
            outString.charCodeAt(0) < 0x80 &&
            outString.charCodeAt(1) < 0x80
          ) {
            error(inString, outString, "2 byte sequence converted to 2 ASCII");
          }
          break;
      }
    }
  }
}
