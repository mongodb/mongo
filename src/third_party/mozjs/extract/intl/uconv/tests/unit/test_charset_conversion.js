const NS_ERROR_ILLEGAL_VALUE = Cr.NS_ERROR_ILLEGAL_VALUE;

var BIS, BOS, _Pipe, COS, FIS, _SS, CIS;

var dataDir;

function run_test() {
  BIS = Components.Constructor(
    "@mozilla.org/binaryinputstream;1",
    "nsIBinaryInputStream",
    "setInputStream"
  );
  BOS = Components.Constructor(
    "@mozilla.org/binaryoutputstream;1",
    "nsIBinaryOutputStream",
    "setOutputStream"
  );
  _Pipe = Components.Constructor("@mozilla.org/pipe;1", "nsIPipe", "init");
  COS = Components.Constructor(
    "@mozilla.org/intl/converter-output-stream;1",
    "nsIConverterOutputStream",
    "init"
  );
  FIS = Components.Constructor(
    "@mozilla.org/network/file-input-stream;1",
    "nsIFileInputStream",
    "init"
  );
  _SS = Components.Constructor(
    "@mozilla.org/storagestream;1",
    "nsIStorageStream",
    "init"
  );
  CIS = Components.Constructor(
    "@mozilla.org/intl/converter-input-stream;1",
    "nsIConverterInputStream",
    "init"
  );

  dataDir = do_get_file("data/");

  test_utf8_1();
  test_cross_conversion();
}

const UNICODE_STRINGS = [
  "\u00BD + \u00BE == \u00BD\u00B2 + \u00BC + \u00BE",

  "AZaz09 \u007F " + // U+000000 to U+00007F
  "\u0080 \u0398 \u03BB \u0725 " + // U+000080 to U+0007FF
    "\u0964 \u0F5F \u20AC \uFFFB", // U+000800 to U+00FFFF

  // there would be strings containing non-BMP code points here, but
  // unfortunately JS strings are UCS-2 (and worse yet are treated as
  // 16-bit values by the spec), so we have to do gymnastics to work
  // with non-BMP -- manual surrogate decoding doesn't work because
  // String.prototype.charCodeAt() ignores surrogate pairs and only
  // returns 16-bit values
];

// test conversion equality -- keys are names of files containing equivalent
// Unicode data, values are the encoding of the file in the format expected by
// nsIConverter(In|Out)putStream.init
const UNICODE_FILES = {
  "unicode-conversion.utf8.txt": "UTF-8",
  "unicode-conversion.utf16.txt": "UTF-16",
  "unicode-conversion.utf16le.txt": "UTF-16LE",
  "unicode-conversion.utf16be.txt": "UTF-16BE",
};

function test_utf8_1() {
  for (var i = 0; i < UNICODE_STRINGS.length; i++) {
    var pipe = Pipe();
    var conv = new COS(pipe.outputStream, "UTF-8");
    Assert.ok(conv.writeString(UNICODE_STRINGS[i]));
    conv.close();

    if (
      !equalStreams(
        new UTF8(pipe.inputStream),
        stringToCodePoints(UNICODE_STRINGS[i])
      )
    ) {
      do_throw("UNICODE_STRINGS[" + i + "] not handled correctly");
    }
  }
}

function test_cross_conversion() {
  for (var fn1 in UNICODE_FILES) {
    var fin = getBinaryInputStream(fn1);
    var ss = StorageStream();

    var bos = new BOS(ss.getOutputStream(0));
    var av;
    while ((av = fin.available()) > 0) {
      var data = fin.readByteArray(av);
      bos.writeByteArray(data);
    }
    fin.close();
    bos.close();

    for (var fn2 in UNICODE_FILES) {
      var fin2 = getUnicharInputStream(fn2, UNICODE_FILES[fn2]);
      var unichar = new CIS(
        ss.newInputStream(0),
        UNICODE_FILES[fn1],
        8192,
        0x0
      );

      if (!equalUnicharStreams(unichar, fin2)) {
        do_throw(
          "unequal streams: " + UNICODE_FILES[fn1] + ", " + UNICODE_FILES[fn2]
        );
      }
    }
  }
}

// utility functions

function StorageStream() {
  return new _SS(8192, Math.pow(2, 32) - 1, null);
}

function getUnicharInputStream(filename, encoding) {
  var file = dataDir.clone();
  file.append(filename);

  const PR_RDONLY = 0x1;
  var fis = new FIS(
    file,
    PR_RDONLY,
    "0644",
    Ci.nsIFileInputStream.CLOSE_ON_EOF
  );
  return new CIS(fis, encoding, 8192, 0x0);
}

function getBinaryInputStream(filename, encoding) {
  var file = dataDir.clone();
  file.append(filename);

  const PR_RDONLY = 0x1;
  var fis = new FIS(
    file,
    PR_RDONLY,
    "0644",
    Ci.nsIFileInputStream.CLOSE_ON_EOF
  );
  return new BIS(fis);
}

function equalStreams(stream, codePoints) {
  var currIndex = 0;
  while (true) {
    var unit = stream.readUnit();
    if (unit < 0) {
      return currIndex == codePoints.length;
    }
    if (unit !== codePoints[currIndex++]) {
      return false;
    }
  }
  // eslint-disable-next-line no-unreachable
  do_throw("not reached");
  return false;
}

function equalUnicharStreams(s1, s2) {
  var r1, r2;
  var str1 = {},
    str2 = {};
  while (true) {
    r1 = s1.readString(1024, str1);
    r2 = s2.readString(1024, str2);

    if (r1 != r2 || str1.value != str2.value) {
      print("r1: " + r1 + ", r2: " + r2);
      print(str1.value.length);
      print(str2.value.length);
      return false;
    }
    if (r1 == 0 && r2 == 0) {
      return true;
    }
  }

  // not reached
  // eslint-disable-next-line no-unreachable
  return false;
}

function stringToCodePoints(str) {
  return str.split("").map(function(v) {
    return v.charCodeAt(0);
  });
}

function lowbits(n) {
  return Math.pow(2, n) - 1;
}

function Pipe() {
  return new _Pipe(false, false, 1024, 10, null);
}

// complex charset readers

/**
 * Wraps a UTF-8 stream to allow access to the Unicode code points in it.
 *
 * @param stream
 *   the stream to wrap
 */
function UTF8(stream) {
  this._stream = new BIS(stream);
}
UTF8.prototype = {
  // returns numeric code point at front of stream encoded in UTF-8, -1 if at
  // end of stream, or throws if valid (and properly encoded!) code point not
  // found
  readUnit() {
    var str = this._stream;

    var c, c2, c3, c4, rv;

    // if at end of stream, must distinguish failure to read any bytes
    // (correct behavior) from failure to read some byte after the first
    // in the character
    try {
      c = str.read8();
    } catch (e) {
      return -1;
    }

    if (c < 0x80) {
      return c;
    }

    if (c < 0xc0) {
      // c < 11000000
      // byte doesn't have enough leading ones (must be at least two)
      throw NS_ERROR_ILLEGAL_VALUE;
    }

    c2 = str.read8();
    if (c2 >= 0xc0 || c2 < 0x80) {
      throw NS_ERROR_ILLEGAL_VALUE;
    } // not 10xxxxxx

    if (c < 0xe0) {
      // c < 11100000
      // two-byte between U+000080 and U+0007FF
      rv = ((lowbits(5) & c) << 6) + (lowbits(6) & c2);
      // no upper bounds-check needed, by previous lines
      if (rv >= 0x80) {
        return rv;
      }
      throw NS_ERROR_ILLEGAL_VALUE;
    }

    c3 = str.read8();
    if (c3 >= 0xc0 || c3 < 0x80) {
      throw NS_ERROR_ILLEGAL_VALUE;
    } // not 10xxxxxx

    if (c < 0xf0) {
      // c < 11110000
      // three-byte between U+000800 and U+00FFFF
      rv =
        ((lowbits(4) & c) << 12) + ((lowbits(6) & c2) << 6) + (lowbits(6) & c3);
      // no upper bounds-check needed, by previous lines
      if (rv >= 0xe000 || (rv >= 0x800 && rv <= 0xd7ff)) {
        return rv;
      }
      throw NS_ERROR_ILLEGAL_VALUE;
    }

    c4 = str.read8();
    if (c4 >= 0xc0 || c4 < 0x80) {
      throw NS_ERROR_ILLEGAL_VALUE;
    } // not 10xxxxxx

    if (c < 0xf8) {
      // c < 11111000
      // four-byte between U+010000 and U+10FFFF
      rv =
        ((lowbits(3) & c) << 18) +
        ((lowbits(6) & c2) << 12) +
        ((lowbits(6) & c3) << 6) +
        (lowbits(6) & c4);
      // need an upper bounds-check since 0x10FFFF isn't (2**n - 1)
      if (rv >= 0x10000 && rv <= 0x10ffff) {
        return rv;
      }
      throw NS_ERROR_ILLEGAL_VALUE;
    }

    // 11111000 or greater -- no UTF-8 mapping
    throw NS_ERROR_ILLEGAL_VALUE;
  },
};

/**
 * Wraps a UTF-16 stream to allow access to the Unicode code points in it.
 *
 * @param stream
 *   the stream to wrap
 * @param bigEndian
 *   true for UTF-16BE, false for UTF-16LE, not present at all for UTF-16 with
 *   a byte-order mark
 */
function UTF16(stream, bigEndian) {
  this._stream = new BIS(stream);
  if (arguments.length > 1) {
    this._bigEndian = bigEndian;
  } else {
    var bom = this._stream.read16();
    if (bom == 0xfeff) {
      this._bigEndian = true;
    } else if (bom == 0xfffe) {
      this._bigEndian = false;
    } else {
      do_throw("missing BOM: " + bom.toString(16).toUpperCase());
    }
  }
}
UTF16.prototype = {
  // returns numeric code point at front of stream encoded in UTF-16,
  // -1 if at end of stream, or throws if UTF-16 code point not found
  readUnit() {
    var str = this._stream;

    // if at end of stream, must distinguish failure to read any bytes
    // (correct behavior) from failure to read some byte after the first
    // in the character
    try {
      var b1 = str.read8();
    } catch (e) {
      return -1;
    }

    var b2 = str.read8();

    var w1 = this._bigEndian ? (b1 << 8) + b2 : (b2 << 8) + b1;

    if (w1 > 0xdbff && w1 < 0xe000) {
      // second surrogate, but expecting none or first
      throw NS_ERROR_ILLEGAL_VALUE;
    }

    if (w1 > 0xd7ff && w1 < 0xdc00) {
      // non-BMP, use surrogate pair
      b1 = str.read8();
      b2 = str.read8();
      var w2 = this._bigEndian ? (b1 << 8) + b2 : (b2 << 8) + b1;
      if (w2 < 0xdc00 || w2 > 0xdfff) {
        throw NS_ERROR_ILLEGAL_VALUE;
      }

      var rv = 0x100000 + ((lowbits(10) & w2) << 10) + (lowbits(10) & w1);
      if (rv <= 0x10ffff) {
        return rv;
      }
      throw NS_ERROR_ILLEGAL_VALUE;
    }

    // non-surrogate
    return w1;
  },
};
