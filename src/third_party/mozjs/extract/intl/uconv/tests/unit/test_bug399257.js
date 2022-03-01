// Tests encoding of characters below U+0020
const inString = "Hello\u000aWorld";
const expectedString = "Hello\nWorld";

function run_test() {
  var failures = false;
  var encodingConverter = CreateScriptableConverter();

  var encoders = [
    "Big5",
    "Big5-HKSCS",
    "EUC-JP",
    "EUC-KR",
    "gb18030",
    "gbk",
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
