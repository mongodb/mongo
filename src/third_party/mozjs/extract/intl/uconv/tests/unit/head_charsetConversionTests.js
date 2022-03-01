const { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");

var CC = Components.Constructor;

function CreateScriptableConverter() {
  var ScriptableUnicodeConverter = CC(
    "@mozilla.org/intl/scriptableunicodeconverter",
    "nsIScriptableUnicodeConverter"
  );

  return new ScriptableUnicodeConverter();
}

function checkDecode(converter, charset, inText, expectedText) {
  try {
    converter.charset = charset;
  } catch (e) {
    converter.charset = "iso-8859-1";
  }

  dump("testing decoding from " + charset + " to Unicode.\n");
  try {
    var outText = converter.ConvertToUnicode(inText);
  } catch (e) {
    outText = "\ufffd";
  }

  if (outText != expectedText) {
    for (var i = 0; i < inText.length; ++i) {
      var inn = inText[i];
      var out = outText[i];
      var expected = expectedText[i];
      if (out != expected) {
        dump(
          "Decoding error at position " +
            i +
            ": for input " +
            escape(inn) +
            " expected " +
            escape(expected) +
            " but got " +
            escape(out) +
            "\n"
        );
      }
    }
  }
  Assert.equal(outText, expectedText);
}

function checkEncode(converter, charset, inText, expectedText) {
  try {
    converter.charset = charset;
  } catch (e) {
    converter.charset = "iso-8859-1";
  }

  dump("testing encoding from Unicode to " + charset + "\n");
  var outText = converter.ConvertFromUnicode(inText) + converter.Finish();

  if (outText != expectedText) {
    for (var i = 0; i < inText.length; ++i) {
      var inn = inText[i];
      var out = outText[i];
      var expected = expectedText[i];
      if (out != expected) {
        dump(
          "Encoding error at position " +
            i +
            ": for input " +
            escape(inn) +
            " expected " +
            escape(expected) +
            " but got " +
            escape(out) +
            "\n"
        );
      }
    }
  }
  Assert.equal(outText, expectedText);
}

function testDecodeAliases(aliases, inString, expectedString) {
  var converter = CreateScriptableConverter();
  for (var i = 0; i < aliases.length; ++i) {
    checkDecode(converter, aliases[i], inString, expectedString);
  }
}

function testEncodeAliases(aliases, inString, expectedString) {
  var converter = CreateScriptableConverter();
  for (var i = 0; i < aliases.length; ++i) {
    checkEncode(converter, aliases[i], inString, expectedString);
  }
}

function testDecodeAliasesInternal(aliases, inString, expectedString) {
  var converter = CreateScriptableConverter();
  converter.isInternal = true;
  for (var i = 0; i < aliases.length; ++i) {
    checkDecode(converter, aliases[i], inString, expectedString);
  }
}

function testEncodeAliasesInternal(aliases, inString, expectedString) {
  var converter = CreateScriptableConverter();
  converter.isInternal = true;
  for (var i = 0; i < aliases.length; ++i) {
    checkEncode(converter, aliases[i], inString, expectedString);
  }
}
