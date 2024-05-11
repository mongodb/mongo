'use strict';

// Ensure that our implementation of `icuGetStringWidth` (exposed as the `getStringWidth` global
// function) produces correct results.
function testGetStringWidth() {
    assert.eq(getStringWidth('a'), 1);
    assert.eq(getStringWidth(String.fromCharCode(0x0061)), 1);
    assert.eq(getStringWidth('丁'), 2);
    assert.eq(getStringWidth(String.fromCharCode(0x4E01)), 2);
    assert.eq(getStringWidth('\ud83d\udc78\ud83c\udfff'), 4);
    assert.eq(getStringWidth('👅'), 2);
    assert.eq(getStringWidth('\ud83d'), 1);
    assert.eq(getStringWidth('\udc78'), 1);
    assert.eq(getStringWidth('\u0000'), 0);
    assert.eq(getStringWidth(String.fromCharCode(0x0007)), 0);
    assert.eq(getStringWidth('\n'), 0);
    assert.eq(getStringWidth(String.fromCharCode(0x00AD)), 1);
    assert.eq(getStringWidth('\u200Ef\u200F'), 1);
    assert.eq(getStringWidth(String.fromCharCode(0x10FFEF)), 1);
    assert.eq(getStringWidth(String.fromCharCode(0x3FFEF)), 1);
    assert.eq(getStringWidth(String.fromCharCode(0x0301)), 0);
    assert.eq(getStringWidth(String.fromCharCode(0x1B44)), 1);
    assert.eq(getStringWidth(String.fromCharCode(0x20DD)), 0);
    assert.eq(getStringWidth('👩‍👩‍👧‍👧'), 8);
    assert.eq(getStringWidth('❤️'), 1);
    assert.eq(getStringWidth('👩‍❤️‍👩'), 5);
    assert.eq(getStringWidth('❤'), 1);
    assert.eq(getStringWidth('\u01d4'), 1);
    assert.eq(getStringWidth('\u200E\n\u220A\u20D2'), 1);
}

testGetStringWidth();
