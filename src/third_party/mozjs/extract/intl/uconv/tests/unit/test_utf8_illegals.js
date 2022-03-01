// Tests illegal UTF-8 sequences

var Cc = Components.Constructor;

const { NetUtil } = ChromeUtils.import("resource://gre/modules/NetUtil.jsm");

const tests = [
  {
    inStrings: [
      "%80", // Illegal or incomplete sequences
      "%8f",
      "%90",
      "%9f",
      "%a0",
      "%bf",
      "%c0",
      "%c1",
      "%c2",
      "%df",
      "%e0",
      "%e0%a0",
      "%e0%bf",
      "%ed%80",
      "%ed%9f",
      "%ef",
      "%ef%bf",
      "%f0",
      "%f0%90",
      "%f0%90%80",
      "%f0%90%bf",
      "%f0%bf",
      "%f0%bf%80",
      "%f0%bf%bf",
      "%f4",
      "%f4%80",
      "%f4%80%80",
      "%f4%80%bf",
      "%f4%8f",
      "%f4%8f%80",
      "%f4%8f%bf",
      "%f5",
      "%f7",
      "%f8",
      "%fb",
      "%fc",
      "%fd",
    ],
    expected: "ABC\ufffdXYZ",
  },

  {
    inStrings: [
      "%c0%af", // Illegal bytes in 2-octet
      "%c1%af",
    ], //  sequences
    expected: "ABC\ufffd\ufffdXYZ",
  },

  {
    inStrings: [
      "%e0%80%80", // Illegal bytes in 3-octet
      "%e0%80%af", //  sequences
      "%e0%9f%bf",
      // long surrogates
      "%ed%a0%80", // D800
      "%ed%ad%bf", // DB7F
      "%ed%ae%80", // DB80
      "%ed%af%bf", // DBFF
      "%ed%b0%80", // DC00
      "%ed%be%80", // DF80
      "%ed%bf%bf",
    ], // DFFF
    expected: "ABC\ufffd\ufffd\ufffdXYZ",
  },

  {
    inStrings: [
      "%f0%80%80%80", // Illegal bytes in 4-octet
      "%f0%80%80%af", //  sequences
      "%f0%8f%bf%bf",
      "%f4%90%80%80",
      "%f4%bf%bf%bf",
      "%f5%80%80%80",
      "%f7%bf%bf%bf",
    ],
    expected: "ABC\ufffd\ufffd\ufffd\ufffdXYZ",
  },

  {
    inStrings: [
      "%f8%80%80%80%80", // Illegal bytes in 5-octet
      "%f8%80%80%80%af", //  sequences
      "%fb%bf%bf%bf%bf",
    ],
    expected: "ABC\ufffd\ufffd\ufffd\ufffd\ufffdXYZ",
  },

  // Surrogate pairs
  {
    inStrings: [
      "%ed%a0%80%ed%b0%80", // D800 DC00
      "%ed%a0%80%ed%bf%bf", // D800 DFFF
      "%ed%ad%bf%ed%b0%80", // DB7F DC00
      "%ed%ad%bf%ed%bf%bf", // DB7F DFFF
      "%ed%ae%80%ed%b0%80", // DB80 DC00
      "%ed%ae%80%ed%bf%bf", // DB80 DFFF
      "%ed%af%bf%ed%b0%80", // DBFF DC00
      "%ed%ad%bf%ed%bf%bf", // DBFF DFFF
      "%fc%80%80%80%80%80", // Illegal bytes in 6-octet
      "%fc%80%80%80%80%af", //  sequences
      "%fd%bf%bf%bf%bf%bf",
    ],
    expected: "ABC\ufffd\ufffd\ufffd\ufffd\ufffd\ufffdXYZ",
  },
];

function testCaseInputStream(inStr, expected) {
  var dataURI = "data:text/plain; charset=UTF-8,ABC" + inStr + "XYZ";
  dump(inStr + "==>");

  var ConverterInputStream = Cc(
    "@mozilla.org/intl/converter-input-stream;1",
    "nsIConverterInputStream",
    "init"
  );
  var channel = NetUtil.newChannel({
    uri: dataURI,
    loadUsingSystemPrincipal: true,
  });
  var testInputStream = channel.open();
  var testConverter = new ConverterInputStream(
    testInputStream,
    "UTF-8",
    16,
    0xfffd
  );

  if (!(testConverter instanceof Ci.nsIUnicharLineInputStream)) {
    throw new Error("not line input stream");
  }

  var outStr = "";
  var more;
  do {
    // read the line and check for eof
    var line = {};
    more = testConverter.readLine(line);
    outStr += line.value;
  } while (more);

  dump(outStr + "; expected=" + expected + "\n");
  Assert.equal(outStr, expected);
  Assert.equal(outStr.length, expected.length);
}

function run_test() {
  for (var t of tests) {
    for (var inStr of t.inStrings) {
      testCaseInputStream(inStr, t.expected);
    }
  }
}
