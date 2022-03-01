// Tests whether characters above 0x7F decode to ASCII characters liable to
// expose XSS vulnerabilities

function run_test() {
  var failures = false;
  var decodingConverter = CreateScriptableConverter();

  var decoders = [
    "Big5",
    "Big5-HKSCS",
    "EUC-JP",
    "EUC-KR",
    "gb18030",
    "IBM866",
    "ISO-2022-JP",
    "ISO-8859-1",
    "ISO-8859-2",
    "ISO-8859-3",
    "ISO-8859-4",
    "ISO-8859-5",
    "ISO-8859-6",
    "ISO-8859-7",
    "ISO-8859-8",
    "ISO-8859-8-I",
    "ISO-8859-10",
    "ISO-8859-13",
    "ISO-8859-14",
    "ISO-8859-15",
    "ISO-8859-16",
    "KOI8-R",
    "KOI8-U",
    "Shift_JIS",
    "windows-1250",
    "windows-1251",
    "windows-1252",
    "windows-1253",
    "windows-1254",
    "windows-1255",
    "windows-1256",
    "windows-1257",
    "windows-1258",
    "windows-874",
    "macintosh",
    "x-mac-cyrillic",
    "x-user-defined",
    "UTF-8",
  ];

  var counter = 0;
  while (counter < decoders.length) {
    var charset = decoders[counter++];
    dump("testing " + counter + " " + charset + "\n");

    decodingConverter.charset = charset;
    for (var i = 0x80; i < 0x100; ++i) {
      var inString = String.fromCharCode(i);
      var outString;
      try {
        outString =
          decodingConverter.ConvertToUnicode(inString) +
          decodingConverter.Finish();
      } catch (e) {
        outString = String.fromCharCode(0xfffd);
      }
      for (var n = 0; n < outString.length; ++n) {
        var outChar = outString.charAt(n);
        if (outChar == "<" || outChar == ">" || outChar == "/") {
          dump(
            charset +
              " has a problem: " +
              escape(inString) +
              " decodes to '" +
              outString +
              "'\n"
          );
          failures = true;
        }
      }
    }
  }
  if (failures) {
    do_throw("test failed\n");
  }
}
