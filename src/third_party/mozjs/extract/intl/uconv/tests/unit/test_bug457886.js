// Tests conversion from Unicode to ISO-2022-JP

const inString =
  "\u3042\u3044\u3046\u3048\u304A\u000D\u000A\u304B\u304D\u304F\u3051\u3053";

const expectedString = '\x1B$B$"$$$&$($*\x1B(B\x0D\x0A\x1B$B$+$-$/$1$3\x1B(B';

const charset = "ISO-2022-JP";

function run_test() {
  checkEncode(CreateScriptableConverter(), charset, inString, expectedString);
}
