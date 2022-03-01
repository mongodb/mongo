// Tests conversion from Unicode to windows-1258

const inString =
  " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~\u20ac\u0081\u201a\u0192\u201e\u2026\u2020\u2021\u02c6\u2030\u008a\u2039\u0152\u008d\u008e\u008f\u0090\u2018\u2019\u201c\u201d\u2022\u2013\u2014\u02dc\u2122\u009a\u203a\u0153\u009d\u009e\u0178\u00a0\u00a1\u00a2\u00a3\u00a4\u00a5\u00a6\u00a7\u00a8\u00a9\u00aa\u00ab\u00ac\u00ad\u00ae\u00af\u00b0\u00b1\u00b2\u00b3\u00b4\u00b5\u00b6\u00b7\u00b8\u00b9\u00ba\u00bb\u00bc\u00bd\u00be\u00bf\u00c0\u00c1\u00c2\u0102\u00c4\u00c5\u00c6\u00c7\u00c8\u00c9\u00ca\u00cb\u0300\u00cd\u00ce\u00cf\u0110\u00d1\u0309\u00d3\u00d4\u01a0\u00d6\u00d7\u00d8\u00d9\u00da\u00db\u00dc\u01af\u0303\u00df\u00e0\u00e1\u00e2\u0103\u00e4\u00e5\u00e6\u00e7\u00e8\u00e9\u00ea\u00eb\u0301\u00ed\u00ee\u00ef\u0111\u00f1\u0323\u00f3\u00f4\u01a1\u00f6\u00f7\u00f8\u00f9\u00fa\u00fb\u00fc\u01b0\u20ab\u00ff";

const expectedString =
  " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~\x80\x81\x82\x83\x84\x85\x86\x87\x88\x89\x8a\x8b\x8c\x8d\x8e\x8f\x90\x91\x92\x93\x94\x95\x96\x97\x98\x99\x9a\x9b\x9c\x9d\x9e\x9f\xa0\xa1\xa2\xa3\xa4\xa5\xa6\xa7\xa8\xa9\xaa\xab\xac\xad\xae\xaf\xb0\xb1\xb2\xb3\xb4\xb5\xb6\xb7\xb8\xb9\xba\xbb\xbc\xbd\xbe\xbf\xc0\xc1\xc2\xc3\xc4\xc5\xc6\xc7\xc8\xc9\xca\xcb\xcc\xcd\xce\xcf\xd0\xd1\xd2\xd3\xd4\xd5\xd6\xd7\xd8\xd9\xda\xdb\xdc\xdd\xde\xdf\xe0\xe1\xe2\xe3\xe4\xe5\xe6\xe7\xe8\xe9\xea\xeb\xec\xed\xee\xef\xf0\xf1\xf2\xf3\xf4\xf5\xf6\xf7\xf8\xf9\xfa\xfb\xfc\xfd\xfe\xff";

const aliases = ["windows-1258", "x-cp1258", "cp1258"];

function run_test() {
  testEncodeAliases(aliases, inString, expectedString);
}
