// Tests encoding of unmapped characters
const inString = "\uE5E5";
const expectedString = "?";

function run_test() {
  var failures = false;
  var encodingConverter = CreateScriptableConverter();

  // this list excludes codepages that can represent all Unicode
  var encoders = [
    "Big5",
    "EUC-JP",
    "EUC-KR",
    "GBK",
    "gb18030",
    "IBM866",
    "ISO-2022-JP",
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
    "ISO-8859-2",
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
    "x-mac-cyrillic",
  ];

  var counter = 0;
  while (counter < encoders.length) {
    var charset = encoders[counter++];

    dump("testing " + counter + " " + charset + "\n");
    encodingConverter.charset = charset;
    var codepageString =
      encodingConverter.ConvertFromUnicode(inString) +
      encodingConverter.Finish();
    if (codepageString != expectedString) {
      dump(charset + " encoding failed\n");
      for (var i = 0; i < expectedString.length; ++i) {
        if (i >= codepageString.length) {
          dump(
            "output length " +
              codepageString.length +
              " less than expected length " +
              expectedString.length +
              "\n"
          );
          break;
        }
        if (codepageString.charAt(i) != expectedString.charAt(i)) {
          dump(
            i.toString(16) +
              ": 0x" +
              codepageString.charCodeAt(i).toString(16) +
              " != " +
              expectedString.charCodeAt(i).toString(16) +
              "\n"
          );
        }
      }
      failures = true;
    }
  }
  if (failures) {
    do_throw("test failed\n");
  }
}
