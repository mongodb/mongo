// Tests conversion from Unicode to ISO-2022-JP with Hankaku characters

const inStrings = [
  // ｡｢｣､･ｦｧｨｩｪｫｬｭｮｯｰｱｲｳｴｵｶｷｸｹｺｻｼｽｾｿﾀﾁﾂﾃﾄﾅﾆﾇﾈﾉﾊﾋﾌﾍﾎﾏﾐﾑﾒﾓﾔﾕﾖﾗﾘﾙﾚﾛﾜﾝﾞﾟ
  "\uFF61\uFF62\uFF63\uFF64\uFF65\uFF66\uFF67\uFF68\uFF69\uFF6A\uFF6B\uFF6C\uFF6D\uFF6E\uFF6F\uFF70\uFF71\uFF72\uFF73\uFF74\uFF75\uFF76\uFF77\uFF78\uFF79\uFF7A\uFF7B\uFF7C\uFF7D\uFF7E\uFF7F\uFF80\uFF81\uFF82\uFF83\uFF84\uFF85\uFF86\uFF87\uFF88\uFF89\uFF8A\uFF8B\uFF8C\uFF8D\uFF8E\uFF8F\uFF90\uFF91\uFF92\uFF93\uFF94\uFF95\uFF96\uFF97\uFF98\uFF99\uFF9A\uFF9B\uFF9C\uFF9D\uFF9E\uFF9F",
  // equivalent to
  // 。「」、・ヲァィゥェォャュョッーアイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホマミムメモヤユヨラリルレロワン゛゜
  // \u3002\u300c\u300d\u3001\u30fb\u30f2\u30a1\u30a3\u30a5\u30a7\u30a9\u30e3\u30e5\u30e7\u30c3\u30fc\u30a2\u30a4\u30a6\u30a8\u30aa\u30ab\u30ad\u30af\u30b1\u30b3\u30b5\u30b7\u30b9\u30bb\u30bd\u30bf\u30c1\u30c4\u30c6\u30c8\u30ca\u30cb\u30cc\u30cd\u30ce\u30cf\u30d2\u30d5\u30d8\u30db\u30de\u30df\u30e0\u30e1\u30e2\u30e4\u30e6\u30e8\u30e9\u30ea\u30eb\u30ec\u30ed\u30ef\u30f3\u309b\u309c"

  // ｶﾞｷﾞｸﾞｹﾞｺﾞｻﾞｼﾞｽﾞｾﾞｿﾞﾀﾞﾁﾞﾂﾞﾃﾞﾄﾞﾊﾞﾋﾞﾌﾞﾍﾞﾎﾞ
  "\uFF76\uFF9E\uFF77\uFF9E\uFF78\uFF9E\uFF79\uFF9E\uFF7A\uFF9E\uFF7B\uFF9E\uFF7C\uFF9E\uFF7D\uFF9E\uFF7E\uFF9E\uFF7F\uFF9E\uFF80\uFF9E\uFF81\uFF9E\uFF82\uFF9E\uFF83\uFF9E\uFF84\uFF9E\uFF8A\uFF9E\uFF8B\uFF9E\uFF8C\uFF9E\uFF8D\uFF9E\uFF8E\uFF9E",
  // equivalent to
  // カ゛キ゛ク゛ケ゛コ゛サ゛シ゛ス゛セ゛ソ゛タ゛チ゛ツ゛テ゛ト゛ハ゛ヒ゛フ゛ヘ゛ホ゛
  // \u30AB\u309B\u30AD\u309B\u30AF\u309B\u30B1\u309B\u30B3\u309B\u30B5\u309B\u30B7\u309B\u30B9\u309B\u30BB\u309B\u30BD\u309B\u30BF\u309B\u30C1\u309B\u30C4\u309B\u30C6\u309B\u30C8\u309B\u30CF\u309B\u30D2\u309B\u30D5\u309B\u30D8\u309B\u30DB\u309B

  // ﾊﾟﾋﾟﾌﾟﾍﾟﾎﾟ
  "\uFF8A\uFF9F\uFF8B\uFF9F\uFF8C\uFF9F\uFF8D\uFF9F\uFF8E\uFF9F",
  // equivalent to
  // ハ゜ヒ゜フ゜ヘ゜ホ゜
  // \u30CF\u309C\u30D2\u309C\u30D5\u309C\u30D8\u309C\u30DB\u309C"

  // Hankaku preceded and followed by regular Katakana (no change of charset)
  // フランツ・ﾖｰｾﾞﾌ・ハイドン
  "\u30D5\u30E9\u30F3\u30C4\u30FB\uFF96\uFF70\uFF7E\uFF9E\uFF8C\u30FB\u30CF\u30A4\u30C9\u30F3",

  // Hankaku preceded and followed by Roman (charset change)
  // Mozilla (ﾓｼﾞﾗ) Foundation
  "Mozilla (\uFF93\uFF7C\uFF9E\uFF97) Foundation",

  // Hankaku preceded and followed by unencodable characters
  // दिल्ली･ﾃﾞﾘｰ･ਦਿੱਲੀ
  "\u0926\u093F\u0932\u094D\u0932\u0940\uFF65\uFF83\uFF9E\uFF98\uFF70\uFF65\u0A26\u0A3F\u0A71\u0A32\u0A40",
];

const expectedStrings = [
  "\x1B$B!#!V!W!\x22!&%r%!%#%%%'%)%c%e%g%C!<%\x22%$%&%(%*%+%-%/%1%3%5%7%9%;%=%?%A%D%F%H%J%K%L%M%N%O%R%U%X%[%^%_%`%a%b%d%f%h%i%j%k%l%m%o%s!+!,\x1B(B",
  "\x1B$B%+!+%-!+%/!+%1!+%3!+%5!+%7!+%9!+%;!+%=!+%?!+%A!+%D!+%F!+%H!+%O!+%R!+%U!+%X!+%[!+\x1B(B",
  "\x1B$B%O!,%R!,%U!,%X!,%[!,\x1B(B",
  "\x1B$B%U%i%s%D!&%h!<%;!+%U!&%O%$%I%s\x1B(B",
  "Mozilla (\x1B$B%b%7!+%i\x1B(B) Foundation",
  "??????\x1B$B!&%F!+%j!<!&\x1B(B?????",
];

function run_test() {
  for (var i = 0; i < inStrings.length; ++i) {
    checkEncode(
      CreateScriptableConverter(),
      "ISO-2022-JP",
      inStrings[i],
      expectedStrings[i]
    );
  }
}
