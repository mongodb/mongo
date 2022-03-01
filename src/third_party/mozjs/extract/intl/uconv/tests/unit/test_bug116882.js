/* Tests conversion of undefined and illegal sequences from Shift-JIS
 *  to Unicode (bug 116882)
 */

const inText = "\xfd\xfe\xff\x81\x20\x81\x3f\x86\x3c";
const expectedText = "\ufffd\ufffd\ufffd\ufffd \ufffd?\ufffd<";
const charset = "Shift_JIS";

function run_test() {
  checkDecode(CreateScriptableConverter(), charset, inText, expectedText);
}
