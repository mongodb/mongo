/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/crypto/jws_validator.h"

#include <iostream>
#include <string>
#include <vector>

#include "mongo/base/data_range.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/config.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/base64.h"
#include "mongo/util/hex.h"

#if MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_OPENSSL

namespace mongo::crypto {

class AsymmetricCryptoTestVectors : public unittest::Test {
public:
    class RSAKeySignatureVerificationVector {
    public:
        RSAKeySignatureVerificationVector(StringData keyID,
                                          StringData e,
                                          StringData n,
                                          StringData msg,
                                          StringData signature,
                                          bool shouldPass) {
            this->keyID = keyID.toString();

            std::string strE = hexblob::decode(e);
            std::string base64E = base64url::encode(StringData(strE.data(), strE.length()));
            this->e = base64E;

            std::string strN = hexblob::decode(n);
            std::string base64N = base64url::encode(StringData(strN.data(), strN.length()));
            this->n = base64N;

            this->msg = hexblob::decode(msg);
            this->signature = hexblob::decode(signature);
            this->shouldPass = shouldPass;
        }

        std::string keyID;
        std::string e;
        std::string n;
        std::string msg;
        std::string signature;
        bool shouldPass;
    };

    void evaluate(RSAKeySignatureVerificationVector test) {
        constexpr auto kKeyType = "RSA";
        constexpr auto kAlgorithm = "RS256";
        BSONObjBuilder rsaKey;

        rsaKey.append("kty", kKeyType);
        rsaKey.append("kid", test.keyID);
        rsaKey.append("e", test.e);
        rsaKey.append("n", test.n);

        auto asymmetricKey = uassertStatusOK(JWSValidator::create(kKeyType, rsaKey.obj()));
        Status result = asymmetricKey->validate(
            kAlgorithm,
            ConstDataRange(test.msg.data(), test.msg.length()).data(),
            ConstDataRange(test.signature.data(), test.signature.length()).data());
        if (test.shouldPass) {
            ASSERT_OK(result);
        } else {
            ASSERT_NOT_OK(result);
        }
    }
};

// /**
//  * RSA test vectors are otained from FIPS 186-4 RSA:
//  https://csrc.nist.gov/Projects/Cryptographic-Algorithm-Validation-Program/Digital-Signatures#rsa2vs
//  */

TEST_F(AsymmetricCryptoTestVectors, RSASignatureVerificationTest1) {
    evaluate(RSAKeySignatureVerificationVector(
        "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
        "49d2a1"_sd,
        "c47abacc2a84d56f3614d92fd62ed36ddde459664b9301dcd1d61781cfcc026bcb2399bee7e75681a80b7bf500e2d08ceae1c42ec0b707927f2b2fe92ae852087d25f1d260cc74905ee5f9b254ed05494a9fe06732c3680992dd6f0dc634568d11542a705f83ae96d2a49763d5fbb24398edf3702bc94bc168190166492b8671de874bb9cecb058c6c8344aa8c93754d6effcd44a41ed7de0a9dcd9144437f212b18881d042d331a4618a9e630ef9bb66305e4fdf8f0391b3b2313fe549f0189ff968b92f33c266a4bc2cffc897d1937eeb9e406f5d0eaa7a14782e76af3fce98f54ed237b4a04a4159a5f6250a296a902880204e61d891c4da29f2d65f34cbb"_sd,
        "95123c8d1b236540b86976a11cea31f8bd4e6c54c235147d20ce722b03a6ad756fbd918c27df8ea9ce3104444c0bbe877305bc02e35535a02a58dcda306e632ad30b3dc3ce0ba97fdf46ec192965dd9cd7f4a71b02b8cba3d442646eeec4af590824ca98d74fbca934d0b6867aa1991f3040b707e806de6e66b5934f05509bea"_sd,
        "51265d96f11ab338762891cb29bf3f1d2b3305107063f5f3245af376dfcc7027d39365de70a31db05e9e10eb6148cb7f6425f0c93c4fb0e2291adbd22c77656afc196858a11e1c670d9eeb592613e69eb4f3aa501730743ac4464486c7ae68fd509e896f63884e9424f69c1c5397959f1e52a368667a598a1fc90125273d9341295d2f8e1cc4969bf228c860e07a3546be2eeda1cde48ee94d062801fe666e4a7ae8cb9cd79262c017b081af874ff00453ca43e34efdb43fffb0bb42a4e2d32a5e5cc9e8546a221fe930250e5f5333e0efe58ffebf19369a3b8ae5a67f6a048bc9ef915bda25160729b508667ada84a0c27e7e26cf2abca413e5e4693f4a9405"_sd,
        true));
}

TEST_F(AsymmetricCryptoTestVectors, RSASignatureVerificationTest2) {
    evaluate(RSAKeySignatureVerificationVector(
        "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
        "49d2a1"_sd,
        "c47abacc2a84d56f3614d92fd62ed36ddde459664b9301dcd1d61781cfcc026bcb2399bee7e75681a80b7bf500e2d08ceae1c42ec0b707927f2b2fe92ae852087d25f1d260cc74905ee5f9b254ed05494a9fe06732c3680992dd6f0dc634568d11542a705f83ae96d2a49763d5fbb24398edf3702bc94bc168190166492b8671de874bb9cecb058c6c8344aa8c93754d6effcd44a41ed7de0a9dcd9144437f212b18881d042d331a4618a9e630ef9bb66305e4fdf8f0391b3b2313fe549f0189ff968b92f33c266a4bc2cffc897d1937eeb9e406f5d0eaa7a14782e76af3fce98f54ed237b4a04a4159a5f6250a296a902880204e61d891c4da29f2d65f34cbb"_sd,
        "f89fd2f6c45a8b5066a651410b8e534bfec0d9a36f3e2b887457afd44dd651d1ec79274db5a455f182572fceea5e9e39c3c7c5d9e599e4fe31c37c34d253b419c3e8fb6b916aef6563f87d4c37224a456e5952698ba3d01b38945d998a795bd285d69478e3131f55117284e27b441f16095dca7ce9c5b68890b09a2bfbb010a5"_sd,
        "ba48538708512d45c0edcac57a9b4fb637e9721f72003c60f13f5c9a36c968cef9be8f54665418141c3d9ecc02a5bf952cfc055fb51e18705e9d8850f4e1f5a344af550de84ffd0805e27e557f6aa50d2645314c64c1c71aa6bb44faf8f29ca6578e2441d4510e36052f46551df341b2dcf43f761f08b946ca0b7081dadbb88e955e820fd7f657c4dd9f4554d167dd7c9a487ed41ced2b40068098deedc951060faf7e15b1f0f80ae67ff2ee28a238d80bf72dd71c8d95c79bc156114ece8ec837573a4b66898d45b45a5eacd0b0e41447d8fa08a367f437645e50c9920b88a16bc0880147acfb9a79de9e351b3fa00b3f4e9f182f45553dffca55e393c5eab6"_sd,
        false));
}

TEST_F(AsymmetricCryptoTestVectors, RSASignatureVerificationTest3) {
    evaluate(RSAKeySignatureVerificationVector(
        "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
        "49d2a1"_sd,
        "c47abacc2a84d56f3614d92fd62ed36ddde459664b9301dcd1d61781cfcc026bcb2399bee7e75681a80b7bf500e2d08ceae1c42ec0b707927f2b2fe92ae852087d25f1d260cc74905ee5f9b254ed05494a9fe06732c3680992dd6f0dc634568d11542a705f83ae96d2a49763d5fbb24398edf3702bc94bc168190166492b8671de874bb9cecb058c6c8344aa8c93754d6effcd44a41ed7de0a9dcd9144437f212b18881d042d331a4618a9e630ef9bb66305e4fdf8f0391b3b2313fe549f0189ff968b92f33c266a4bc2cffc897d1937eeb9e406f5d0eaa7a14782e76af3fce98f54ed237b4a04a4159a5f6250a296a902880204e61d891c4da29f2d65f34cbb"_sd,
        "915c5e4c16acfa0f49de43d6491f0060a944034475ba518572c08366a8d36c7f1e6afc11e5e4649757bf7b9da10a61d57f1d626847871d8a2948e551b54167c79de88d3ebd40a3e35809b996a53348f98a9918c7a7ec606896ed30c271e00c51953dd97aa6a8fe1cd423c3695c83fcf45120ec0a9cd1644642182b60e599a246"_sd,
        "3d57ea5961db8fc144301ca4278f799911229d865ea3e992c7fbc4d03c6551729e26034e95dd71da312340e4051c9dd9b12f7700a821fe3b7c37785d5106350b667ac255a57c13da5842d90bcadea9e6b1f720c607d6893a2caa3c5f3c4074e914451a45380a767c291a67cac3f1cab1fbd05adc37036856a8404e7cea3654019466de449ad6e92b27254f3d25949b1b860065406455a13db7c5fe25d1af7a84cddf7792c64e16260c950d60bd86d005924148ad097c126b84947ab6e89d48f61e711d62522b6e48f16186d1339e6ab3f58c359eb24cb68043737591cd7d9390a468c0022b3b253be52f1a7fc408f84e9ffb4c34fa9e01605851d6583aa13032"_sd,
        false));
}

TEST_F(AsymmetricCryptoTestVectors, RSASignatureVerificationTest4) {
    evaluate(RSAKeySignatureVerificationVector(
        "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
        "7485b2"_sd,
        "c47abacc2a84d56f3614d92fd62ed36ddde459664b9301dcd1d61781cfcc026bcb2399bee7e75681a80b7bf500e2d08ceae1c42ec0b707927f2b2fe92ae852087d25f1d260cc74905ee5f9b254ed05494a9fe06732c3680992dd6f0dc634568d11542a705f83ae96d2a49763d5fbb24398edf3702bc94bc168190166492b8671de874bb9cecb058c6c8344aa8c93754d6effcd44a41ed7de0a9dcd9144437f212b18881d042d331a4618a9e630ef9bb66305e4fdf8f0391b3b2313fe549f0189ff968b92f33c266a4bc2cffc897d1937eeb9e406f5d0eaa7a14782e76af3fce98f54ed237b4a04a4159a5f6250a296a902880204e61d891c4da29f2d65f34cbb"_sd,
        "3d2f0693517cffb2b724c1f30502c5359c051c1bcd88dc1dd54b89e6981009d275a813b2bf016b74d0f6ed0d91e62d0884785c9afd8fd1fb7e99246cd4005cdda71a39cb649197a996d8ad2d23fdfb6bb015f24ec3d7f88af64fb83b4b525eb06607d133eec834cf7d6c9ab817b4c0dda370459d9cfba05ad0c1adc86a909fe1"_sd,
        "511abd82218cab344979b2887b02600d2427f1eb12ac01d97684c2a443a9272834c3f79cded07a39dbee3770dde827a74dc994b17bfd8a26d07b239d26d58c42f79d560264c31b7e1c3dddef6d7556f228c394414f4cec561c3da2686a8eebec7702f32850809a93deeb84b2a02fcdba224d2fd9efb8e056e796f49b57d56e9f3e90d0b49b08bdee93a2e12e676fb4d4fa838c5bd88eda008f1b592a72465587be0ae17d9b156b904f44a7e04d3b58d24ad67b71b0f4c699fa51639546b62b9f83597ff03d465f1bb396ae15e92d0e92e85647d5df113e2c7518d0e3ad2e7aa7dac720c98347aa151e4f37fea081dbed350cc9c93f606b38f21a3e5de6d140d2"_sd,
        false));
}

TEST_F(AsymmetricCryptoTestVectors, RSASignatureVerificationTest5) {
    evaluate(RSAKeySignatureVerificationVector(
        "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
        "49d2a1"_sd,
        "c47abacc2a84d56f3614d92fd62ed36ddde459664b9301dcd1d61781cfcc026bcb2399bee7e75681a80b7bf500e2d08ceae1c42ec0b707927f2b2fe92ae852087d25f1d260cc74905ee5f9b254ed05494a9fe06732c3680992dd6f0dc634568d11542a705f83ae96d2a49763d5fbb24398edf3702bc94bc168190166492b8671de874bb9cecb058c6c8344aa8c93754d6effcd44a41ed7de0a9dcd9144437f212b18881d042d331a4618a9e630ef9bb66305e4fdf8f0391b3b2313fe549f0189ff968b92f33c266a4bc2cffc897d1937eeb9e406f5d0eaa7a14782e76af3fce98f54ed237b4a04a4159a5f6250a296a902880204e61d891c4da29f2d65f34cbb"_sd,
        "dffe42bfda886e1a73fe8a8dfcf71c9fb44deb054588a9bb9199d554aecce08f2ff88f2aa6f8a0fb675fb03c8e685c27432ca7c33c189bfd849d34fa7b2979ac1f57eca389632426bae0b98398ad60a3342557e14e96041c1bf4d90b46cf7ad1348322d28caf43c4f7e86c0924ae703c109ec50a84ea2a43df078c3015a52b28"_sd,
        "8f4dd479239f2d08dc05d7d40539288b67c4d77210ecb16be76f0b1925e8b088570831e361a1ca57893135f8af64b8e2996b8d635899da4e04c68acb9b1b3813697d57da90c57f18509e0ab6705c704feb448cca5c07d258ecd884ab93f508cefdb25f2bc3061c4006099e2e33b27972c3edb0a0a33114d381c82ab506d041ff680af595ef3400a8bb6774030d2e38dd304272092bd32a553017f7bda4b998b27aa8aca12def327b1f11063a5342b0d55738183417d321c5682fc4ab64e79174216feebb989521e1e3d827647068003be34fe1d093964d28f4877c49b4065672448597a89b91919cfb55ca13836e7e6f3b3fd04f417cf1c16d9872538bf4e87a"_sd,
        false));
}

TEST_F(AsymmetricCryptoTestVectors, RSASignatureVerificationTest6) {
    evaluate(RSAKeySignatureVerificationVector(
        "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
        "49d2a1"_sd,
        "c47abacc2a84d56f3614d92fd62ed36ddde459664b9301dcd1d61781cfcc026bcb2399bee7e75681a80b7bf500e2d08ceae1c42ec0b707927f2b2fe92ae852087d25f1d260cc74905ee5f9b254ed05494a9fe06732c3680992dd6f0dc634568d11542a705f83ae96d2a49763d5fbb24398edf3702bc94bc168190166492b8671de874bb9cecb058c6c8344aa8c93754d6effcd44a41ed7de0a9dcd9144437f212b18881d042d331a4618a9e630ef9bb66305e4fdf8f0391b3b2313fe549f0189ff968b92f33c266a4bc2cffc897d1937eeb9e406f5d0eaa7a14782e76af3fce98f54ed237b4a04a4159a5f6250a296a902880204e61d891c4da29f2d65f34cbb"_sd,
        "cfe99788f55ec6944942bd0a187d51b80fd8bd4051bd4f07c73e614eb75a8b9f997b176b2642b5f1b1877061ba9ce142c1d2a311583f072b7cbe08ed253681191c209d7b0d438fcdddc284d93d59d6dd80e48333a921dd31c9b6834f88768f8701e01102d3e8bdf074fbe0b8c93d9951f41545ef6eeb3be35530babc079f1fb3"_sd,
        "9fd6f6107e838107f906c26cb2910704599f175b6a84db485fbc30776eb7fd53bfe20c38c537b154a3e519b662bd9fdc8e3045e21f6e5ae97d0ff6a9d8632825544525d84f99f80e3ed4e69dc5e219d59ccfbb37c23c84fe3b3e6fb22f402f94e5225c6387fdf8bcdb3508f8832908fe05771521e92234348004e8fe19a8f24bebcab9f074327c88d066bc12081748d696be6135c6aea32220ea786ebd7800e6936365ff25831c28cb6c8a59237ff84f5cf89036cff188ee0f9a6195f2b1aca2e4442af8369f1b49322fa2f891b83a14a97b60c6aeafd6c2928047affda9c8d869ff5294bb5943ad14a6d64e784d126c469d51e292b9ce33e1d8371ba5f467b3"_sd,
        false));
}

}  // namespace mongo::crypto

#endif
