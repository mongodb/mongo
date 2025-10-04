// SERVER-4656 optimize $sort followed by $limit

let c = db[jsTestName()];
c.drop();

let NUM_OBJS = 100;

let randoms = {};
function generateRandom() {
    // we want unique randoms since $sort isn't guaranteed stable
    let random;
    do {
        random = Math.round(Math.random() * 1000 * NUM_OBJS);
    } while (randoms[random]);
    randoms[random] = true;
    return random;
}

for (let i = 0; i < NUM_OBJS; i++) {
    c.insert({inc: i, dec: NUM_OBJS - i, rnd: generateRandom()});
}

let inc_sorted = c.aggregate({$sort: {inc: 1}}).toArray();
let dec_sorted = c.aggregate({$sort: {dec: 1}}).toArray();
let rnd_sorted = c.aggregate({$sort: {rnd: 1}}).toArray();

function test(limit, direction) {
    try {
        let res_inc = c.aggregate({$sort: {inc: direction}}, {$limit: limit}).toArray();
        let res_dec = c.aggregate({$sort: {dec: direction}}, {$limit: limit}).toArray();
        let res_rnd = c.aggregate({$sort: {rnd: direction}}, {$limit: limit}).toArray();

        let expectedLength = Math.min(limit, NUM_OBJS);

        assert.eq(res_inc.length, expectedLength);
        assert.eq(res_dec.length, expectedLength);
        assert.eq(res_rnd.length, expectedLength);

        if (direction > 0) {
            for (let i = 0; i < expectedLength; i++) {
                assert.eq(res_inc[i], inc_sorted[i]);
                assert.eq(res_dec[i], dec_sorted[i]);
                assert.eq(res_rnd[i], rnd_sorted[i]);
            }
        } else {
            for (let i = 0; i < expectedLength; i++) {
                assert.eq(res_inc[i], inc_sorted[NUM_OBJS - 1 - i]);
                assert.eq(res_dec[i], dec_sorted[NUM_OBJS - 1 - i]);
                assert.eq(res_rnd[i], rnd_sorted[NUM_OBJS - 1 - i]);
            }
        }
    } catch (e) {
        print("failed with limit=" + limit + " direction= " + direction);
        throw e;
    }
}

test(1, 1);
test(1, -1);
test(10, 1);
test(10, -1);
test(50, 1);
test(50, -1);
test(NUM_OBJS, 1);
test(NUM_OBJS, -1);
test(NUM_OBJS + 10, 1);
test(NUM_OBJS + 10, -1);
