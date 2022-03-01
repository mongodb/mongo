// crash test with invaild parameter (bug 522931)
function run_test() {
  Assert.equal(Services.textToSubURI.UnEscapeAndConvert("UTF-8", null), "");
}
