// Tests conversion of a single byte from UTF-16 to Unicode

const inString = "A";
const expectedString = "";
const charset = "UTF-16BE";

function run_test() {
  checkDecode(CreateScriptableConverter(), charset, inString, expectedString);
}
