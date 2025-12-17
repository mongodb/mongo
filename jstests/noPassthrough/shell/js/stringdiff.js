import {describe, it} from "jstests/libs/mochalite.js";
import {stringdiff, colorize} from "src/mongo/shell/stringdiff.js";

describe("diff strings", () => {
    function difftest(oldStr, newStr, expectedDiff) {
        let diff = stringdiff(oldStr, newStr);
        assert.eq(diff, expectedDiff);
    }

    it("no diff", () => {
        const oldStr = "aaa\nbbb\nccc";
        const newStr = "aaa\nbbb\nccc";
        const expectedDiff = "";
        difftest(oldStr, newStr, expectedDiff);
    });

    it("middle diff", () => {
        const oldStr = "aaa\nbbb\nccc";
        const newStr = "aaa\nxxx\nccc";
        const expectedDiff = `\
 aaa
-bbb
+xxx
 ccc`;
        difftest(oldStr, newStr, expectedDiff);
    });

    it("prefix diff", () => {
        const oldStr = "aaa\nbbb\nccc";
        const newStr = "xxx\nbbb\nccc";
        const expectedDiff = `\
-aaa
+xxx
 bbb
 ccc`;
        difftest(oldStr, newStr, expectedDiff);
    });

    it("suffix diff", () => {
        const oldStr = "aaa\nbbb\nccc";
        const newStr = "aaa\nbbb\nxxx";
        const expectedDiff = `\
 aaa
 bbb
-ccc
+xxx`;
        difftest(oldStr, newStr, expectedDiff);
    });

    it("oneliner diff", () => {
        const oldStr = "aaa";
        const newStr = "axa";
        // don't do character by character diffing, just line by line
        const expectedDiff = `\
-aaa
+axa`;
        difftest(oldStr, newStr, expectedDiff);
    });

    it("completely different", () => {
        const oldStr = "aaa\nbbb\nccc";
        const newStr = "xxx\nyyy\nzzz";
        const expectedDiff = `\
-aaa
-bbb
-ccc
+xxx
+yyy
+zzz`;
        difftest(oldStr, newStr, expectedDiff);
    });

    it("context window", () => {
        const oldStr = "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl\nm\nn\no\np\nq\nr\ns\nt\nu\nv\nw\nx\ny\nz";
        const newStr = "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl\nX\nn\no\np\nq\nr\ns\nt\nu\nv\nw\nx\ny\nz";
        const expectedDiff = `\
 i
 j
 k
 l
-m
+X
 n
 o
 p
 q`;
        difftest(oldStr, newStr, expectedDiff);
    });

    it("overlapping context window", () => {
        const oldStr = "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl\nm\nn\no\np\nq\nr\ns\nt\nu\nv\nw\nx\ny\nz";
        const newStr = "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nX\nl\nm\nn\nY\np\nq\nr\ns\nt\nu\nv\nw\nx\ny\nz";
        const expectedDiff = `\
 g
 h
 i
 j
-k
+X
 l
 m
 n
-o
+Y
 p
 q
 r
 s`;
        difftest(oldStr, newStr, expectedDiff);
    });

    it("separate chunks", () => {
        const oldStr = "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl\nm\nn\no\np\nq\nr\ns\nt\nu\nv\nw\nx\ny\nz";
        const newStr = "a\nb\nX\nd\ne\nf\ng\nh\ni\nj\nk\nl\nm\nn\no\np\nq\nr\ns\nt\nu\nv\nw\nY\ny\nz";
        const expectedDiff = `\
 a
 b
-c
+X
 d
 e
 f
 g
---
 t
 u
 v
 w
-x
+Y
 y
 z`;
        difftest(oldStr, newStr, expectedDiff);

        let highlighted = colorize(stringdiff(oldStr, newStr));
        const expectedHighlighted = `\
 a
 b
\u001b[31m-c\u001b[0m
\u001b[32m+X\u001b[0m
 d
 e
 f
 g
---
 t
 u
 v
 w
\u001b[31m-x\u001b[0m
\u001b[32m+Y\u001b[0m
 y
 z`;
        assert.eq(highlighted, expectedHighlighted);
    });

    it("compound diff", () => {
        // https://www.nathaniel.ai/myers-diff/
        const oldStr = `\
Empty Bottles - Colin Morton (1981)
---
line up all the empty bottles
the long-necked beer bottles from the antique stores
the wine bottles and pop bottles left on beaches
steam off the labels and line the bottles up the green ones with
the brown black yellow and clear ones
line up
the beer bottles whose labels have been torn off by
neurotic fingers
and the bottles sent back by the breweries because they have
cockroaches or dead mice at the bottom
line up
the bottles afloat on all the seas those with messages in
them and those without any
and the bottles with methyl hydrate-soaked cotton in them
used by schoolkids for killing insects
line up the bottle that killed Malcolm Lowry with the bottle that...`;

        const newStr = `\
Monkey Stops Whistling - David Morgan (2011)
---
Stand to attention all the empty bottles
the long-necked beer bottles from the antique stores
the wine bottles and pop bottles left on beaches
steam off the labels and line the bottles up the green ones with
the brown black yellow and clear ones
Stand to attention all the empty bottles
the beer bottles whose labels have been torn off by
neurotic fingers
and the bottles sent back by the breweries because they have
cockroaches or dead bluebottles at the bottom
Stand to attention all the empty bottles
the bottles afloat on all the seas those with messages in
them and those without any
line up the bottle that killed Malcolm Lowry with the bottle that...`;

        const expectedDiff = `\
-Empty Bottles - Colin Morton (1981)
+Monkey Stops Whistling - David Morgan (2011)
 ---
-line up all the empty bottles
+Stand to attention all the empty bottles
 the long-necked beer bottles from the antique stores
 the wine bottles and pop bottles left on beaches
 steam off the labels and line the bottles up the green ones with
 the brown black yellow and clear ones
-line up
+Stand to attention all the empty bottles
 the beer bottles whose labels have been torn off by
 neurotic fingers
 and the bottles sent back by the breweries because they have
-cockroaches or dead mice at the bottom
-line up
+cockroaches or dead bluebottles at the bottom
+Stand to attention all the empty bottles
 the bottles afloat on all the seas those with messages in
 them and those without any
-and the bottles with methyl hydrate-soaked cotton in them
-used by schoolkids for killing insects
 line up the bottle that killed Malcolm Lowry with the bottle that...`;

        difftest(oldStr, newStr, expectedDiff);
    });
});
