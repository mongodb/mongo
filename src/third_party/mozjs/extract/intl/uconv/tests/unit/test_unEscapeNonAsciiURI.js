// Tests for nsITextToSubURI.unEscapeNonAsciiURI
function run_test() {
  // Tests whether nsTextToSubURI does UTF-16 unescaping (it shouldn't)
  const testURI = "data:text/html,%FE%FF";
  Assert.equal(
    Services.textToSubURI.unEscapeNonAsciiURI("UTF-16", testURI),
    testURI
  );

  // Tests whether incomplete multibyte sequences throw.
  const tests = [
    {
      input: "http://example.com/?p=%E9",
      throws: Cr.NS_ERROR_ILLEGAL_INPUT,
    },
    {
      input: "http://example.com/?p=%E9%80",
      throws: Cr.NS_ERROR_ILLEGAL_INPUT,
    },
    {
      input: "http://example.com/?p=%E9%80%80",
      expected: "http://example.com/?p=\u9000",
    },
    {
      input: "http://example.com/?p=%E9e",
      throws: Cr.NS_ERROR_ILLEGAL_INPUT,
    },
    {
      input: "http://example.com/?p=%E9%E9",
      throws: Cr.NS_ERROR_ILLEGAL_INPUT,
    },
    {
      input: "http://example.com/?name=M%FCller/",
      throws: Cr.NS_ERROR_ILLEGAL_INPUT,
    },
    {
      input: "http://example.com/?name=M%C3%BCller/",
      expected: "http://example.com/?name=Müller/",
    },
  ];

  for (const t of tests) {
    if (t.throws !== undefined) {
      let thrown = undefined;
      try {
        Services.textToSubURI.unEscapeNonAsciiURI("UTF-8", t.input);
      } catch (e) {
        thrown = e.result;
      }
      Assert.equal(thrown, t.throws);
    } else {
      Assert.equal(
        Services.textToSubURI.unEscapeNonAsciiURI("UTF-8", t.input),
        t.expected
      );
    }
  }
}
