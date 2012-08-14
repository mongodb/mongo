/*
 * city-test.c - cityhash-c
 * CityHash on C
 * Copyright (c) 2011-2012, Alexander Nusov
 *
 * - original copyright notice -
 * Copyright (c) 2011 Google, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <string.h>
#include <stdio.h>

#include "wt_internal.h"
#include "hash.h"

static const uint64_t k0 = 0xc3a5c85c97cb3127ULL;
static const int kDataSize = 1 << 20;
static const int kTestSize = 300;

static char data[1 << 20];

static int errors = 0;  /* global error count */

/* Initialize data to pseudorandom values. */
void setup() {
	uint64_t a = 9;
	uint64_t b = 777;
	int i;
	for (i = 0; i < kDataSize; i++) {
		a = (a ^ (a >> 41)) * k0 + b;
		b = (b ^ (b >> 41)) * k0 + i;
		uint8_t u = b >> 37;
		memcpy(data + i, &u, 1);  /* uint8_t -> char */
	}
}

#define	C(x) 0x ## x ## ULL
static const uint64_t testdata[300] = {
    C(9ae16a3b2f90404f),
    C(75e9dee28ded761d),
    C(75de892fdc5ba914),
    C(69cfe9fca1cc683a),
    C(675b04c582a34966),
    C(46fa817397ea8b68),
    C(406e959cdffadec7),
    C(46663908b4169b95),
    C(f214b86cffeab596),
    C(eba670441d1a4f7d),
    C(172c17ff21dbf88d),
    C(5a0838df8a019b8c),
    C(8f42b1fbb2fc0302),
    C(72085e82d70dcea9),
    C(32b75fc2223b5032),
    C(e1dd010487d2d647),
    C(2994f9245194a7e2),
    C(32e2ed6fa03e5b22),
    C(37a72b6e89410c9f),
    C(10836563cb8ff3a1),
    C(4dabcb5c1d382e5c),
    C(296afb509046d945),
    C(f7c0257efde772ea),
    C(61e021c8da344ba1),
    C(c0a86ed83908560b),
    C(35c9cf87e4accbf3),
    C(e74c366b3091e275),
    C(a3f2ca45089ad1a6),
    C(e5181466d8e60e26),
    C(fb528a8dd1e48ad7),
    C(da6d2b7ea9d5f9b6),
    C(61d95225bc2293e),
    C(81247c01ab6a9cc1),
    C(c17f3ebd3257cb8b),
    C(9802438969c3043b),
    C(3dd8ed248a03d754),
    C(c5bf48d7d3e9a5a3),
    C(bc4a21d00cf52288),
    C(172c8674913ff413),
    C(17a361dbdaaa7294),
    C(5cc268bac4bd55f),
    C(db04969cc06547f1),
    C(25bd8d3ca1b375b2),
    C(166c11fbcbc89fd8),
    C(3565bcc4ca4ce807),
    C(b7897fd2f274307d),
    C(aba98113ab0e4a16),
    C(17f7796e0d4b636c),
    C(33c0128e62122440),
    C(988bc5d290b97aef),
    C(23c8c25c2ab72381),
    C(450fe4acc4ad3749),
    C(48e1eff032d90c50),
    C(c048604ba8b6c753),
    C(67ff1cbe469ebf84),
    C(b45c7536bd7a5416),
    C(215c2eaacdb48f6f),
    C(241baf16d80e0fe8),
    C(d10a9743b5b1c4d1),
    C(919ef9e209f2edd1),
    C(b5f9519b6c9280b),
    C(77a75e89679e6757),
    C(9d709e1b086aabe2),
    C(91c89971b3c20a8a),
    C(16468c55a1b3f2b4),
    C(1a2bd6641870b0e4),
    C(1d2f92f23d3e811a),
    C(a47c08255da30ca8),
    C(efb3b0262c9cd0c),
    C(5029700a7773c3a4),
    C(71c8287225d96c9a),
    C(4e8b9ad9347d7277),
    C(1d5218d6ee2e52ab),
    C(162360be6c293c8b),
    C(31459914f13c8867),
    C(6b4e8fca9b3aecff),
    C(dd3271a46c7aec5d),
    C(109b226238347d6e),
    C(cc920608aa94cce4),
    C(901ff46f22283dbe),
    C(11b3bdda68b0725d),
    C(9f5f03e84a40d232),
    C(39eeff4f60f439be),
    C(9b9e0126fe4b8b04),
    C(3ec4b8462b36df47),
    C(5e3fd9298fe7009f),
    C(7504ecb4727b274e),
    C(4bdedc104d5eaed5),
    C(da0b4a6fb26a4748),
    C(bad6dd64d1b18672),
    C(98da3fe388d5860e),
    C(ef243a576431d7ac),
    C(97854de6f22c97b6),
    C(d26ce17bfc1851d), 
    C(97477bac0ba4c7f1),
    C(f6bbcba92b11f5c8),
    C(1ac8b67c1c82132),
    C(9cd716ca0eee52fa),
    C(1909f39123d9ad44),
    C(1d206f99f535efeb),
    C(b38c3a83042eb802),
    C(488d6b45d927161b),
    C(3d6aaa43af5d4f86),
    C(e5c40a6381e43845),
    C(86fb323e5a4b710b),
    C(7930c09adaf6e62e),
    C(e505e86f0eff4ecd),
    C(dedccb12011e857),
    C(4d679bda26f5555f),
    C(e47cd22995a75a51),
    C(92ba8e12e0204f05),
    C(bb3a8427c64f8939),
    C(998988f7d6dacc43),
    C(3f1049221dd72b98),
    C(419e4ff78c3e06f3),
    C(9ba090af14171317),
    C(6ad739e4ada9a340),
    C(8ecff07fd67e4abd),
    C(497ca8dbfee8b3a7),
    C(a929cd66daa65b0a),
    C(4107c4156bc8d4bc),
    C(15b38dc0e40459d1),
    C(e5e5370ed3186f6c),
    C(ea2785c8f873e28f),
    C(e7bf98235fc8a4a8),
    C(b94e261a90888396),
    C(4b0226e5f5cdc9c),
    C(e07edbe7325c718c),
    C(f4b56421eae4c4e7),
    C(c07fcb8ae7b4e480),
    C(3edad9568a9aaab),
    C(fcabde8700de91e8),
    C(362fc5ba93e8eb31),
    C(e323b1c5b55a4dfb),
    C(9461913a153530ef),
    C(ec2332acc6df0c41),
    C(aae00b3a289bc82),
    C(4c842e732fcd25f),
    C(40e23ca5817a91f3),
    C(2564527fad710b8d),
    C(6bf70df9d15a2bf6),
    C(45e6f446eb6bbcf5),
    C(620d3fe4b8849c9e),
    C(535aa7340b3c168f),
    C(dd3e761d4eada300),
    C(4ac1902ccde06545),
    C(aee339a23bb00a5e),
    C(627599e91148df4f),
    C(cfb04cf00cfed6b3),
    C(942631c47a26889),
    C(4c9eb4e09726b47e),
    C(cf7f208ef20e887a),
    C(a8dc4687bcf27f4),
    C(daed638536ed19df),
    C(90a04b11ee1e4af3),
    C(511f29e1b732617d),
    C(95567f03939e651f),
    C(248a32ad10e76bc3),
    C(f9f05a2a892242eb),
    C(3a015cea8c3f5bdf),
    C(670e43506aa1f71b),
    C(977bb79b58bbd984),
    C(e6264fbccd93a530),
    C(54e4d0cab7ec5c27),
    C(882a598e98cf5416),
    C(7c37a5376c056d55),
    C(21c5e9616f24be97),
    C(a6eaefe74fa7d62b),
    C(764af88ed4b0b828),
    C(5f55a694fb173ea3),
    C(86d086738b0a7701),
    C(acfc8be97115847b),
    C(dc5ad3c016024d4),
    C(a0e91182f03277f7),
    C(767f1dbd1dba673b),
    C(a5e30221500dcd53),
    C(ebaed82e48e18ce4),
    C(ffa50913362b118d),
    C(256186bcda057f54),
    C(382b15315e84f28b),
    C(9983a9cc5576d967),
    C(c2c58a843f733bdb),
    C(648ff27fabf93521),
    C(99be55475dcb3461),
    C(e16fbb9303dd6d92),
    C(4a57a4899834e4c0),
    C(f658958cdf49f149),
    C(ae1befc65d3ea04d),
    C(2fc8f9fc5946296d),
    C(efdb4dc26e84dce4),
    C(e84a123b3e1b0c91),
    C(686c22d2863c48a6),
    C(2c5c1c9fa0ecfde0),
    C(7a0ac8dd441c9a9d),
    C(c6f9a31dfc91537c),
    C(2d12010eaf7d8d3d),
    C(f46de22b2d79a5bd),
    C(2ce2f3e89fa141fe),
    C(8aa75419d95555dd),
    C(1fbf19dd9207e6aa),
    C(6f11f9c1131f1182),
    C(92e896d8bfdebdb5),
    C(369ba54df3c534d1),
    C(bcc4229e083e940e),
    C(17c648f416fb15ca),
    C(8b752dfa2f9fa592),
    C(3edda5262a7869bf),
    C(b5776f9ceaf0dba2),
    C(313161f3ce61ec83),
    C(6e23080c57f4bcb),
    C(a194c120f440fd48),
    C(3c78f67ee87b62fe),
    C(b7d2aee81871e3ac),
    C(41ec0382933e8f72),
    C(7fe4e777602797f0),
    C(aa71127fd91bd68a),
    C(677b53782a8af152),
    C(8f97bb03473c860f),
    C(fe50ea9f58e4de6f),
    C(56eb0fcb9852bd27),
    C(6be2903d8f07af90),
    C(58668066da6bfc4),
    C(2d04d1fbab341106),
    C(d366b37bcd83805b),
    C(bbb9bc819ab65946),
    C(8e994eac99bbc61),
    C(364cabf6585e2f7d),
    C(e878e2200893d775),
    C(6f6a014b9a861926),
    C(da52b64212e8149b),
    C(1100f797ce53a629),
    C(4f00655b76e9cfda),
    C(d970198b6ca854db),
    C(76c850754f28803),
    C(9c98da9763f0d691),
    C(22f8f6869a5f325),
    C(9ae7575fc14256bb),
    C(540040e79752b619),
    C(34cbf85722d897b1),
    C(46afa66366bf5989),
    C(e15074b077c6e560),
    C(9740b2b2d6d06c6),
    C(f1431889f2db1bff),
    C(e2dd273a8d28c52d),
    C(e0f79029834cc6ac),
    C(9dc0a29830bcbec1),
    C(d10b70dde60270a6),
    C(58d2459f094d075c),
    C(68c600cdd42d9f65),
    C(9fc0bd876cb975da),
    C(7b3e8c267146c361),
    C(6c49c6b3c9dd340f),
    C(47324327a4cf1732),
    C(1e984ef53c5f6aae),
    C(b88f5c9b8b854cc6),
    C(9fcb3fdb09e7a63a),
    C(d58438d62089243), 
    C(7ea73b6b74c8cd0b),
    C(e42ffdf6278bb21),
    C(9ad37fcd49fe4aa0),
    C(27b7ff391136696e),
    C(76bd077e42692ddf),
    C(81c672225442e053),
    C(37b9e308747448ca),
    C(dab71695950a51d4),
    C(28630288285c747b),
    C(4b591ad5160b6c1b),
    C(7e02f7a5a97dd3df),
    C(ff66660873521fb2),
    C(ef3c4dd60fa37412),
    C(a7b07bcb1a1333ba),
    C(89858fc94ee25469),
    C(e6344d83aafdca2e),
    C(3ddbb400198d3d4d),
    C(79d4c20cf83a7732),
    C(5aaf0d7b669173b5),
    C(35d6cc883840170c),
    C(b07eead7a57ff2fe),
    C(20fba36c76380b18),
    C(c7e9e0c45afeab41),
    C(69a8e59c1577145d),
    C(6638191337226509),
    C(c44e8c33ff6c5e13),
    C(68125009158bded7),
    C(e0dd644db35a62d6),
    C(21ce9eab220aaf87),
    C(83b54046fdca7c1e),
    C(dd3b2ce7dabb22fb),
    C(9ee08fc7a86b0ea6),
    C(4e2a10f1c409dfa5),
    C(bdf3383d7216ee3c),
    C(a22e8f6681f0267d),
    C(6997121cae0ce362),
    C(9bfc2c3e050a3c27),
    C(a3f37e415aedf14),
    C(cef3c748045e7618),
    C(fe2b841766a4d212),
    C(d6131e688593a181),
    C(91288884ebfcf145),
    C(754c7e98ae3495ea),
};

void Check(uint64_t expected, uint64_t actual) {
	if (expected != actual) {
		fprintf(stderr,
		    "ERROR: expected %llx, but got %llx", expected, actual);
		++errors;
	}
}

void Test(const uint64_t expected, int offset, int len) {
	Check(expected, __wt_hash_city(data + offset, len));
}

int city_hash_main() {
	int i;
	setup();
	for (i = 0 ; i < kTestSize - 1; i++) {
		Test(testdata[i], i * i, i);
	}
	Test(testdata[i], 0, kDataSize);
	return (errors > 0);
}
