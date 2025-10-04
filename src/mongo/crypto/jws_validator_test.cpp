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

#include "mongo/base/data_range.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/base64.h"
#include "mongo/util/hex.h"

#include <string>

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
            this->keyID = std::string{keyID};

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

    class ECKeySignatureVerificationVector {
    public:
        // Signature verification for the EC (Elliptic Curve) key type. The FIPS 186-4 signature
        // verification vectors are specified in hex format but the validate() API expects the
        // input to be in Base64URL-encoded format. Hence, we need to decode the hex blob first,
        // and then re-encode it in Base64URL.
        ECKeySignatureVerificationVector(StringData keyID,
                                         StringData alg,
                                         StringData crv,
                                         StringData msg,
                                         StringData Qx,
                                         StringData Qy,
                                         StringData R,
                                         StringData S,
                                         bool shouldPass) {

            // The key ID
            this->keyID = std::string{keyID};

            // Algorithm
            this->ec_alg = std::string{alg};

            // EC Curve
            this->ec_crv = std::string(crv.data(), crv.length());

            // The message to be verified
            this->msg = hexblob::decode(msg);

            // The x-coordinate of the private key
            std::string strQX = hexblob::decode(Qx);
            std::string base64QX = base64url::encode(StringData(strQX.data(), strQX.length()));
            this->ec_x = base64QX;

            // The y-coordinate of the private key
            std::string strQY = hexblob::decode(Qy);
            std::string base64QY = base64url::encode(StringData(strQY.data(), strQY.length()));
            this->ec_y = base64QY;

            // The R component of the signature
            this->ec_signature_r = hexblob::decode(R);

            // The S component of the signature
            this->ec_signature_s = hexblob::decode(S);

            // Combined R and S signature to a single "signature" field
            this->signature = this->ec_signature_r + this->ec_signature_s;

            this->shouldPass = shouldPass;
        }

        // The EC key ID
        std::string keyID;

        // Specifies the EC algorithm e.g. ES512
        std::string ec_alg;

        // Specifies the elliptic curve used, e.g. P-256
        std::string ec_crv;

        // X-coordinate of public key point on the elliptic curve
        std::string ec_x;
        std::string ec_y;

        // The message to be validated
        std::string msg;

        // r component of EC signature
        std::string ec_signature_r;

        // s component of EC signature
        std::string ec_signature_s;

        // Combined r and s component of EC
        std::string signature;

        bool shouldPass;
    };

    void evaluateRSA(RSAKeySignatureVerificationVector test) {
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

    // Helper function to build the EC key in BSON format
    void buildECKey(BSONObjBuilder& ecKey, ECKeySignatureVerificationVector signatureVector) {
        constexpr auto kKeyType = "EC";
        ecKey.append("kty", kKeyType);
        ecKey.append("kid", signatureVector.keyID);
        ecKey.append("alg", signatureVector.ec_alg);
        ecKey.append("crv", signatureVector.ec_crv);
        ecKey.append("x", signatureVector.ec_x);
        ecKey.append("y", signatureVector.ec_y);

        // ec_signature_r is in Base64URL format
        ecKey.append("r", signatureVector.ec_signature_r);

        // ec_signature_s is in Base64URL format
        ecKey.append("s", signatureVector.ec_signature_s);
    }

    void evaluateEC(ECKeySignatureVerificationVector test, int expectedError = ErrorCodes::OK) {
        constexpr auto kKeyType = "EC";
        BSONObjBuilder ecKey;
        buildECKey(ecKey, test);

        // Don't append the signature directly as it is a concatenation of r and s components
        auto asymmetricKey = uassertStatusOK(JWSValidator::create(kKeyType, ecKey.obj()));
        StringData sMsg(test.msg.data(), test.msg.length());
        Status result = asymmetricKey->validate(
            test.ec_alg,
            sMsg,
            ConstDataRange(test.signature.data(), test.signature.length()).data());
        if (test.shouldPass) {
            ASSERT_OK(result);
        } else {
            ASSERT_NOT_OK(result);
        }
        if (expectedError != ErrorCodes::OK) {
            ASSERT_EQ(result.code(), expectedError);
        }
    };

    // Error cases when the parameters are invalid and we fail to create the EC signature
    // verification vector
    void evaluateFailedEC(ECKeySignatureVerificationVector test, int expectedError) {
        constexpr auto kKeyType = "EC";
        BSONObjBuilder ecKey;

        buildECKey(ecKey, test);

        // Don't append the signature directly as it is a concatenation of r and s components
        StatusWith<std::unique_ptr<JWSValidator>> asymmetricKey =
            JWSValidator::create(kKeyType, ecKey.obj());
        Status result = asymmetricKey.getStatus();
        ASSERT_EQ(result.code(), expectedError);
    }

    // Error case when we create the correct signature verification vector but pass invalid
    // parameters to validate()
    void evaluateInvalidAlgorithmEC(ECKeySignatureVerificationVector test, int expectedError) {
        constexpr auto kKeyType = "EC";
        BSONObjBuilder ecKey;

        buildECKey(ecKey, test);

        // Create the validator with the correct parameters
        auto asymmetricKey = uassertStatusOK(JWSValidator::create(kKeyType, ecKey.obj()));

        // call validate() with an invalid algorithm
        StringData sMsg(test.msg.data(), test.msg.length());
        Status result = asymmetricKey->validate(
            StringData("invalid_algorithm"),
            sMsg,
            ConstDataRange(test.signature.data(), test.signature.length()).data());
        ASSERT_NOT_OK(result);
        ASSERT_EQ(result.code(), expectedError);
    }

    void evaluateEmptyPayload(ECKeySignatureVerificationVector test, int expectedError) {
        constexpr auto kKeyType = "EC";
        BSONObjBuilder ecKey;

        buildECKey(ecKey, test);

        // Create the validator with the correct parameters
        auto asymmetricKey = uassertStatusOK(JWSValidator::create(kKeyType, ecKey.obj()));

        // call validate() with an invalid algorithm
        Status result = asymmetricKey->validate(
            test.ec_crv,
            StringData(""),  // empty payload
            ConstDataRange(test.signature.data(), test.signature.length()).data());
        ASSERT_NOT_OK(result);
        ASSERT_EQ(result.code(), expectedError);
    }
};

// /**
//  * RSA test vectors are otained from FIPS 186-4 RSA:
//  https://csrc.nist.gov/Projects/Cryptographic-Algorithm-Validation-Program/Digital-Signatures#rsa2vs
//  */

TEST_F(AsymmetricCryptoTestVectors, RSASignatureVerificationTest1) {
    evaluateRSA(RSAKeySignatureVerificationVector(
        "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
        "49d2a1"_sd,
        "c47abacc2a84d56f3614d92fd62ed36ddde459664b9301dcd1d61781cfcc026bcb2399bee7e75681a80b7bf500e2d08ceae1c42ec0b707927f2b2fe92ae852087d25f1d260cc74905ee5f9b254ed05494a9fe06732c3680992dd6f0dc634568d11542a705f83ae96d2a49763d5fbb24398edf3702bc94bc168190166492b8671de874bb9cecb058c6c8344aa8c93754d6effcd44a41ed7de0a9dcd9144437f212b18881d042d331a4618a9e630ef9bb66305e4fdf8f0391b3b2313fe549f0189ff968b92f33c266a4bc2cffc897d1937eeb9e406f5d0eaa7a14782e76af3fce98f54ed237b4a04a4159a5f6250a296a902880204e61d891c4da29f2d65f34cbb"_sd,
        "95123c8d1b236540b86976a11cea31f8bd4e6c54c235147d20ce722b03a6ad756fbd918c27df8ea9ce3104444c0bbe877305bc02e35535a02a58dcda306e632ad30b3dc3ce0ba97fdf46ec192965dd9cd7f4a71b02b8cba3d442646eeec4af590824ca98d74fbca934d0b6867aa1991f3040b707e806de6e66b5934f05509bea"_sd,
        "51265d96f11ab338762891cb29bf3f1d2b3305107063f5f3245af376dfcc7027d39365de70a31db05e9e10eb6148cb7f6425f0c93c4fb0e2291adbd22c77656afc196858a11e1c670d9eeb592613e69eb4f3aa501730743ac4464486c7ae68fd509e896f63884e9424f69c1c5397959f1e52a368667a598a1fc90125273d9341295d2f8e1cc4969bf228c860e07a3546be2eeda1cde48ee94d062801fe666e4a7ae8cb9cd79262c017b081af874ff00453ca43e34efdb43fffb0bb42a4e2d32a5e5cc9e8546a221fe930250e5f5333e0efe58ffebf19369a3b8ae5a67f6a048bc9ef915bda25160729b508667ada84a0c27e7e26cf2abca413e5e4693f4a9405"_sd,
        true));
}

TEST_F(AsymmetricCryptoTestVectors, RSASignatureVerificationTest2) {
    evaluateRSA(RSAKeySignatureVerificationVector(
        "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
        "49d2a1"_sd,
        "c47abacc2a84d56f3614d92fd62ed36ddde459664b9301dcd1d61781cfcc026bcb2399bee7e75681a80b7bf500e2d08ceae1c42ec0b707927f2b2fe92ae852087d25f1d260cc74905ee5f9b254ed05494a9fe06732c3680992dd6f0dc634568d11542a705f83ae96d2a49763d5fbb24398edf3702bc94bc168190166492b8671de874bb9cecb058c6c8344aa8c93754d6effcd44a41ed7de0a9dcd9144437f212b18881d042d331a4618a9e630ef9bb66305e4fdf8f0391b3b2313fe549f0189ff968b92f33c266a4bc2cffc897d1937eeb9e406f5d0eaa7a14782e76af3fce98f54ed237b4a04a4159a5f6250a296a902880204e61d891c4da29f2d65f34cbb"_sd,
        "f89fd2f6c45a8b5066a651410b8e534bfec0d9a36f3e2b887457afd44dd651d1ec79274db5a455f182572fceea5e9e39c3c7c5d9e599e4fe31c37c34d253b419c3e8fb6b916aef6563f87d4c37224a456e5952698ba3d01b38945d998a795bd285d69478e3131f55117284e27b441f16095dca7ce9c5b68890b09a2bfbb010a5"_sd,
        "ba48538708512d45c0edcac57a9b4fb637e9721f72003c60f13f5c9a36c968cef9be8f54665418141c3d9ecc02a5bf952cfc055fb51e18705e9d8850f4e1f5a344af550de84ffd0805e27e557f6aa50d2645314c64c1c71aa6bb44faf8f29ca6578e2441d4510e36052f46551df341b2dcf43f761f08b946ca0b7081dadbb88e955e820fd7f657c4dd9f4554d167dd7c9a487ed41ced2b40068098deedc951060faf7e15b1f0f80ae67ff2ee28a238d80bf72dd71c8d95c79bc156114ece8ec837573a4b66898d45b45a5eacd0b0e41447d8fa08a367f437645e50c9920b88a16bc0880147acfb9a79de9e351b3fa00b3f4e9f182f45553dffca55e393c5eab6"_sd,
        false));
}

TEST_F(AsymmetricCryptoTestVectors, RSASignatureVerificationTest3) {
    evaluateRSA(RSAKeySignatureVerificationVector(
        "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
        "49d2a1"_sd,
        "c47abacc2a84d56f3614d92fd62ed36ddde459664b9301dcd1d61781cfcc026bcb2399bee7e75681a80b7bf500e2d08ceae1c42ec0b707927f2b2fe92ae852087d25f1d260cc74905ee5f9b254ed05494a9fe06732c3680992dd6f0dc634568d11542a705f83ae96d2a49763d5fbb24398edf3702bc94bc168190166492b8671de874bb9cecb058c6c8344aa8c93754d6effcd44a41ed7de0a9dcd9144437f212b18881d042d331a4618a9e630ef9bb66305e4fdf8f0391b3b2313fe549f0189ff968b92f33c266a4bc2cffc897d1937eeb9e406f5d0eaa7a14782e76af3fce98f54ed237b4a04a4159a5f6250a296a902880204e61d891c4da29f2d65f34cbb"_sd,
        "915c5e4c16acfa0f49de43d6491f0060a944034475ba518572c08366a8d36c7f1e6afc11e5e4649757bf7b9da10a61d57f1d626847871d8a2948e551b54167c79de88d3ebd40a3e35809b996a53348f98a9918c7a7ec606896ed30c271e00c51953dd97aa6a8fe1cd423c3695c83fcf45120ec0a9cd1644642182b60e599a246"_sd,
        "3d57ea5961db8fc144301ca4278f799911229d865ea3e992c7fbc4d03c6551729e26034e95dd71da312340e4051c9dd9b12f7700a821fe3b7c37785d5106350b667ac255a57c13da5842d90bcadea9e6b1f720c607d6893a2caa3c5f3c4074e914451a45380a767c291a67cac3f1cab1fbd05adc37036856a8404e7cea3654019466de449ad6e92b27254f3d25949b1b860065406455a13db7c5fe25d1af7a84cddf7792c64e16260c950d60bd86d005924148ad097c126b84947ab6e89d48f61e711d62522b6e48f16186d1339e6ab3f58c359eb24cb68043737591cd7d9390a468c0022b3b253be52f1a7fc408f84e9ffb4c34fa9e01605851d6583aa13032"_sd,
        false));
}

TEST_F(AsymmetricCryptoTestVectors, RSASignatureVerificationTest4) {
    evaluateRSA(RSAKeySignatureVerificationVector(
        "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
        "7485b2"_sd,
        "c47abacc2a84d56f3614d92fd62ed36ddde459664b9301dcd1d61781cfcc026bcb2399bee7e75681a80b7bf500e2d08ceae1c42ec0b707927f2b2fe92ae852087d25f1d260cc74905ee5f9b254ed05494a9fe06732c3680992dd6f0dc634568d11542a705f83ae96d2a49763d5fbb24398edf3702bc94bc168190166492b8671de874bb9cecb058c6c8344aa8c93754d6effcd44a41ed7de0a9dcd9144437f212b18881d042d331a4618a9e630ef9bb66305e4fdf8f0391b3b2313fe549f0189ff968b92f33c266a4bc2cffc897d1937eeb9e406f5d0eaa7a14782e76af3fce98f54ed237b4a04a4159a5f6250a296a902880204e61d891c4da29f2d65f34cbb"_sd,
        "3d2f0693517cffb2b724c1f30502c5359c051c1bcd88dc1dd54b89e6981009d275a813b2bf016b74d0f6ed0d91e62d0884785c9afd8fd1fb7e99246cd4005cdda71a39cb649197a996d8ad2d23fdfb6bb015f24ec3d7f88af64fb83b4b525eb06607d133eec834cf7d6c9ab817b4c0dda370459d9cfba05ad0c1adc86a909fe1"_sd,
        "511abd82218cab344979b2887b02600d2427f1eb12ac01d97684c2a443a9272834c3f79cded07a39dbee3770dde827a74dc994b17bfd8a26d07b239d26d58c42f79d560264c31b7e1c3dddef6d7556f228c394414f4cec561c3da2686a8eebec7702f32850809a93deeb84b2a02fcdba224d2fd9efb8e056e796f49b57d56e9f3e90d0b49b08bdee93a2e12e676fb4d4fa838c5bd88eda008f1b592a72465587be0ae17d9b156b904f44a7e04d3b58d24ad67b71b0f4c699fa51639546b62b9f83597ff03d465f1bb396ae15e92d0e92e85647d5df113e2c7518d0e3ad2e7aa7dac720c98347aa151e4f37fea081dbed350cc9c93f606b38f21a3e5de6d140d2"_sd,
        false));
}

TEST_F(AsymmetricCryptoTestVectors, RSASignatureVerificationTest5) {
    evaluateRSA(RSAKeySignatureVerificationVector(
        "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
        "49d2a1"_sd,
        "c47abacc2a84d56f3614d92fd62ed36ddde459664b9301dcd1d61781cfcc026bcb2399bee7e75681a80b7bf500e2d08ceae1c42ec0b707927f2b2fe92ae852087d25f1d260cc74905ee5f9b254ed05494a9fe06732c3680992dd6f0dc634568d11542a705f83ae96d2a49763d5fbb24398edf3702bc94bc168190166492b8671de874bb9cecb058c6c8344aa8c93754d6effcd44a41ed7de0a9dcd9144437f212b18881d042d331a4618a9e630ef9bb66305e4fdf8f0391b3b2313fe549f0189ff968b92f33c266a4bc2cffc897d1937eeb9e406f5d0eaa7a14782e76af3fce98f54ed237b4a04a4159a5f6250a296a902880204e61d891c4da29f2d65f34cbb"_sd,
        "dffe42bfda886e1a73fe8a8dfcf71c9fb44deb054588a9bb9199d554aecce08f2ff88f2aa6f8a0fb675fb03c8e685c27432ca7c33c189bfd849d34fa7b2979ac1f57eca389632426bae0b98398ad60a3342557e14e96041c1bf4d90b46cf7ad1348322d28caf43c4f7e86c0924ae703c109ec50a84ea2a43df078c3015a52b28"_sd,
        "8f4dd479239f2d08dc05d7d40539288b67c4d77210ecb16be76f0b1925e8b088570831e361a1ca57893135f8af64b8e2996b8d635899da4e04c68acb9b1b3813697d57da90c57f18509e0ab6705c704feb448cca5c07d258ecd884ab93f508cefdb25f2bc3061c4006099e2e33b27972c3edb0a0a33114d381c82ab506d041ff680af595ef3400a8bb6774030d2e38dd304272092bd32a553017f7bda4b998b27aa8aca12def327b1f11063a5342b0d55738183417d321c5682fc4ab64e79174216feebb989521e1e3d827647068003be34fe1d093964d28f4877c49b4065672448597a89b91919cfb55ca13836e7e6f3b3fd04f417cf1c16d9872538bf4e87a"_sd,
        false));
}

TEST_F(AsymmetricCryptoTestVectors, RSASignatureVerificationTest6) {
    evaluateRSA(RSAKeySignatureVerificationVector(
        "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
        "49d2a1"_sd,
        "c47abacc2a84d56f3614d92fd62ed36ddde459664b9301dcd1d61781cfcc026bcb2399bee7e75681a80b7bf500e2d08ceae1c42ec0b707927f2b2fe92ae852087d25f1d260cc74905ee5f9b254ed05494a9fe06732c3680992dd6f0dc634568d11542a705f83ae96d2a49763d5fbb24398edf3702bc94bc168190166492b8671de874bb9cecb058c6c8344aa8c93754d6effcd44a41ed7de0a9dcd9144437f212b18881d042d331a4618a9e630ef9bb66305e4fdf8f0391b3b2313fe549f0189ff968b92f33c266a4bc2cffc897d1937eeb9e406f5d0eaa7a14782e76af3fce98f54ed237b4a04a4159a5f6250a296a902880204e61d891c4da29f2d65f34cbb"_sd,
        "cfe99788f55ec6944942bd0a187d51b80fd8bd4051bd4f07c73e614eb75a8b9f997b176b2642b5f1b1877061ba9ce142c1d2a311583f072b7cbe08ed253681191c209d7b0d438fcdddc284d93d59d6dd80e48333a921dd31c9b6834f88768f8701e01102d3e8bdf074fbe0b8c93d9951f41545ef6eeb3be35530babc079f1fb3"_sd,
        "9fd6f6107e838107f906c26cb2910704599f175b6a84db485fbc30776eb7fd53bfe20c38c537b154a3e519b662bd9fdc8e3045e21f6e5ae97d0ff6a9d8632825544525d84f99f80e3ed4e69dc5e219d59ccfbb37c23c84fe3b3e6fb22f402f94e5225c6387fdf8bcdb3508f8832908fe05771521e92234348004e8fe19a8f24bebcab9f074327c88d066bc12081748d696be6135c6aea32220ea786ebd7800e6936365ff25831c28cb6c8a59237ff84f5cf89036cff188ee0f9a6195f2b1aca2e4442af8369f1b49322fa2f891b83a14a97b60c6aeafd6c2928047affda9c8d869ff5294bb5943ad14a6d64e784d126c469d51e292b9ce33e1d8371ba5f467b3"_sd,
        false));
}


// EC test vectors are otained from FIPS 186-4 RSA SigVer.rsp:
// https://csrc.nist.gov/CSRC/media/Projects/Cryptographic-Algorithm-Validation-Program/documents/dss/186-4ecdsatestvectors.zip
// We initialize these test vectors to
// ECKeySignatureVerificationVector(StringData keyID, StringData crv, StringData msg, StringData Qx,
// StringData Qy, StringData R, StringData S, bool shouldPass) {

/* P-256, SHA-256 */
TEST_F(AsymmetricCryptoTestVectors, ECSignatureVerificationP256Test1) {
    evaluateEC(ECKeySignatureVerificationVector(
                   "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
                   "ES256"_sd,
                   "P-256"_sd,
                   "e4796db5f785f207aa30d311693b3702821dff1168fd2e04c0836825aefd850d9aa60326d88cde1"
                   "a23c7745351"
                   "392ca2288d632c264f197d05cd424a30336c19fd09bb229654f0222fcb881a4b35c290a093ac159"
                   "ce13409111f"
                   "f0358411133c24f5b8e2090d6db6558afc36f06ca1f6ef779785adba68db27a409859fc4c4a0",
                   "87f8f2b218f49845f6f10eec3877136269f5c1a54736dbdf69f89940cad41555",
                   "e15f369036f49842fac7a86c8a2b0557609776814448b8f5e84aa9f4395205e9",
                   "d19ff48b324915576416097d2544f7cbdf8768b1454ad20e0baac50e211f23b0",
                   "a3e81e59311cdfff2d4784949f7a2cb50ba6c3a91fa54710568e61aca3e847c6",
                   false),
               ErrorCodes::InvalidSignature);
}

/* P-256, SHA-256 */
TEST_F(AsymmetricCryptoTestVectors, ECSignatureVerificationP256Test2) {
    evaluateEC(ECKeySignatureVerificationVector(
        "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
        "ES256"_sd,
        "P-256"_sd,
        "e1130af6a38ccb412a9c8d13e15dbfc9e69a16385af3c3f1e5da954fd5e7c45fd75e2b8c36699228e92840c056"
        "2fbf3772f07e17f1add56588dd45f7450e1217ad239922dd9c32695dc71ff2424ca0dec1321aa47064a044b7fe"
        "3c2b97d03ce470a592304c5ef21eed9f93da56bb232d1eeb0035f9bf0dfafdcc4606272b20a3",
        "e424dc61d4bb3cb7ef4344a7f8957a0c5134e16f7a67c074f82e6e12f49abf3c",
        "970eed7aa2bc48651545949de1dddaf0127e5965ac85d1243d6f60e7dfaee927",
        "bf96b99aa49c705c910be33142017c642ff540c76349b9dab72f981fd9347f4f",
        "17c55095819089c2e03b9cd415abdf12444e323075d98f31920b9e0f57ec871c",
        true));
}

/* P-256, SHA-256 */
TEST_F(AsymmetricCryptoTestVectors, ECSignatureVerificationP256Test3) {
    evaluateEC(ECKeySignatureVerificationVector(
                   "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
                   "ES256"_sd,
                   "P-256"_sd,
                   "069a6e6b93dfee6df6ef6997cd80dd2182c36653cef10c655d524585655462d683877f95ecc6d6c"
                   "81623d8fac4"
                   "e900ed0019964094e7de91f1481989ae1873004565789cbf5dc56c62aedc63f62f3b894c9c6f778"
                   "8c8ecaadc9b"
                   "d0e81ad91b2b3569ea12260e93924fdddd3972af5273198f5efda0746219475017557616170e",
                   "5cf02a00d205bdfee2016f7421807fc38ae69e6b7ccd064ee689fc1a94a9f7d2",
                   "ec530ce3cc5c9d1af463f264d685afe2b4db4b5828d7e61b748930f3ce622a85",
                   "dc23d130c6117fb5751201455e99f36f59aba1a6a21cf2d0e7481a97451d6693",
                   "d6ce7708c18dbf35d4f8aa7240922dc6823f2e7058cbc1484fcad1599db5018c",
                   false),
               ErrorCodes::InvalidSignature);
};

/* P-256, SHA-256 */
TEST_F(AsymmetricCryptoTestVectors, ECSignatureVerificationP256Test4) {
    evaluateEC(ECKeySignatureVerificationVector(
                   "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
                   "ES256"_sd,
                   "P-256"_sd,
                   "df04a346cf4d0e331a6db78cca2d456d31b0a000aa51441defdb97bbeb20b94d8d746429a393ba8"
                   "8840d661615"
                   "e07def615a342abedfa4ce912e562af714959896858af817317a840dcff85a057bb91a3c2bf9010"
                   "5500362754a"
                   "6dd321cdd86128cfc5f04667b57aa78c112411e42da304f1012d48cd6a7052d7de44ebcc01de",
                   "2ddfd145767883ffbb0ac003ab4a44346d08fa2570b3120dcce94562422244cb",
                   "5f70c7d11ac2b7a435ccfbbae02c3df1ea6b532cc0e9db74f93fffca7c6f9a64",
                   "9913111cff6f20c5bf453a99cd2c2019a4e749a49724a08774d14e4c113edda8",
                   "9467cd4cd21ecb56b0cab0a9a453b43386845459127a952421f5c6382866c5cc",
                   false),
               ErrorCodes::InvalidSignature);
};

/* P-256, SHA-256 */
TEST_F(AsymmetricCryptoTestVectors, ECSignatureVerificationP256Test5) {
    evaluateEC(ECKeySignatureVerificationVector(
        "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
        "ES256"_sd,
        "P-256"_sd,
        "73c5f6a67456ae48209b5f85d1e7de7758bf235300c6ae2bdceb1dcb27a7730fb68c950b7fcada0ecc4661d357"
        "8230f225a875e69aaa17f1e71c6be5c831f22663bac63d0c7a9635edb0043ff8c6f26470f02a7bc56556f1437f"
        "06dfa27b487a6c4290d8bad38d4879b334e341ba092dde4e4ae694a9c09302e2dbf443581c08",
        "e0fc6a6f50e1c57475673ee54e3a57f9a49f3328e743bf52f335e3eeaa3d2864",
        "7f59d689c91e463607d9194d99faf316e25432870816dde63f5d4b373f12f22a",
        "1d75830cd36f4c9aa181b2c4221e87f176b7f05b7c87824e82e396c88315c407",
        "cb2acb01dac96efc53a32d4a0d85d0c2e48955214783ecf50a4f0414a319c05a",
        true));
};

TEST_F(AsymmetricCryptoTestVectors, ECSignatureVerificationP256Test6) {
    evaluateEC(ECKeySignatureVerificationVector(
                   "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
                   "ES256"_sd,
                   "P-256"_sd,
                   "666036d9b4a2426ed6585a4e0fd931a8761451d29ab04bd7dc6d0c5b9e38e6c2b263ff6cb837bd0"
                   "4399de3d757"
                   "c6c7005f6d7a987063cf6d7e8cb38a4bf0d74a282572bd01d0f41e3fd066e3021575f0fa04f27b7"
                   "00d5b7ddddf"
                   "50965993c3f9c7118ed78888da7cb221849b3260592b8e632d7c51e935a0ceae15207bedd548",
                   "a849bef575cac3c6920fbce675c3b787136209f855de19ffe2e8d29b31a5ad86",
                   "bf5fe4f7858f9b805bd8dcc05ad5e7fb889de2f822f3d8b41694e6c55c16b471",
                   "25acc3aa9d9e84c7abf08f73fa4195acc506491d6fc37cb9074528a7db87b9d6",
                   "9b21d5b5259ed3f2ef07dfec6cc90d3a37855d1ce122a85ba6a333f307d31537",
                   false),
               ErrorCodes::InvalidSignature);
};

/* P-256, SHA-256 */
TEST_F(AsymmetricCryptoTestVectors, ECSignatureVerificationP256Test7) {
    evaluateEC(ECKeySignatureVerificationVector(
                   "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
                   "ES256"_sd,
                   "P-256"_sd,
                   "7e80436bce57339ce8da1b5660149a20240b146d108deef3ec5da4ae256f8f894edcbbc57b34ce3"
                   "7089c0daa17"
                   "f0c46cd82b5a1599314fd79d2fd2f446bd5a25b8e32fcf05b76d644573a6df4ad1dfea707b479d9"
                   "7237a346f1e"
                   "c632ea5660efb57e8717a8628d7f82af50a4e84b11f21bdff6839196a880ae20b2a0918d58cd",
                   "3dfb6f40f2471b29b77fdccba72d37c21bba019efa40c1c8f91ec405d7dcc5df",
                   "f22f953f1e395a52ead7f3ae3fc47451b438117b1e04d613bc8555b7d6e6d1bb",
                   "548886278e5ec26bed811dbb72db1e154b6f17be70deb1b210107decb1ec2a5a",
                   "e93bfebd2f14f3d827ca32b464be6e69187f5edbd52def4f96599c37d58eee75",
                   false),
               ErrorCodes::InvalidSignature);
};

TEST_F(AsymmetricCryptoTestVectors, ECSignatureVerificationP256Test8) {
    evaluateEC(ECKeySignatureVerificationVector(
        "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
        "ES256"_sd,
        "P-256"_sd,
        "60cd64b2cd2be6c33859b94875120361a24085f3765cb8b2bf11e026fa9d8855dbe435acf7882e84f3c7857f96"
        "e2baab4d9afe4588e4a82e17a78827bfdb5ddbd1c211fbc2e6d884cddd7cb9d90d5bf4a7311b83f35250803381"
        "2c776a0e00c003c7e0d628e50736c7512df0acfa9f2320bd102229f46495ae6d0857cc452a84",
        "2d98ea01f754d34bbc3003df5050200abf445ec728556d7ed7d5c54c55552b6d",
        "9b52672742d637a32add056dfd6d8792f2a33c2e69dafabea09b960bc61e230a",
        "06108e525f845d0155bf60193222b3219c98e3d49424c2fb2a0987f825c17959",
        "62b5cdd591e5b507e560167ba8f6f7cda74673eb315680cb89ccbc4eec477dce",
        true));
};

/* P-384, SHA-384 */
TEST_F(AsymmetricCryptoTestVectors, ECSignatureVerificationP384Test1) {
    evaluateEC(ECKeySignatureVerificationVector(
                   "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
                   "ES384"_sd,
                   "P-384"_sd,
                   "4132833a525aecc8a1a6dea9f4075f44feefce810c4668423b38580417f7bdca5b21061a45eaa3c"
                   "be2a7035ed1"
                   "89523af8002d65c2899e65735e4d93a16503c145059f365c32b3acc6270e29a09131299181c98b3"
                   "c76769a18fa"
                   "f21f6b4a8f271e6bf908e238afe8002e27c63417bda758f846e1e3b8e62d7f05ebd98f1f9154",
                   "1f94eb6f439a3806f8054dd79124847d138d14d4f52bac93b042f2ee3cdb7dc9e09925c2a5fee70"
                   "d4ce08c61e3"
                   "b19160",
                   "1c4fd111f6e33303069421deb31e873126be35eeb436fe2034856a3ed1e897f26c846ee3233cd16"
                   "240989a7990"
                   "c19d8c",
                   "3c15c3cedf2a6fbff2f906e661f5932f2542f0ce68e2a8182e5ed3858f33bd3c5666f17ac39e52c"
                   "b004b80a0d4"
                   "ba73cd",
                   "9de879083cbb0a97973c94f1963d84f581e4c6541b7d000f9850deb25154b23a37dd72267bdd726"
                   "65cc7027f88"
                   "164fab",
                   false),
               ErrorCodes::InvalidSignature);
};

/* P-384, SHA-384 */
TEST_F(AsymmetricCryptoTestVectors, ECSignatureVerificationP384Test2) {
    evaluateEC(ECKeySignatureVerificationVector(
        "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
        "ES384"_sd,
        "P-384"_sd,
        "9dd789ea25c04745d57a381f22de01fb0abd3c72dbdefd44e43213c189583eef85ba662044da3de2dd8670e632"
        "5154480155bbeebb702c75781ac32e13941860cb576fe37a05b757da5b5b418f6dd7c30b042e40f4395a342ae4"
        "dce05634c33625e2bc524345481f7e253d9551266823771b251705b4a85166022a37ac28f1bd",
        "cb908b1fd516a57b8ee1e14383579b33cb154fece20c5035e2b3765195d1951d75bd78fb23e00fef37d7d064fd"
        "9af144",
        "cd99c46b5857401ddcff2cf7cf822121faf1cbad9a011bed8c551f6f59b2c360f79bfbe32adbcaa09583bdfdf7"
        "c374bb",
        "33f64fb65cd6a8918523f23aea0bbcf56bba1daca7aff817c8791dc92428d605ac629de2e847d43cee55ba9e4a"
        "0e83ba",
        "4428bb478a43ac73ecd6de51ddf7c28ff3c2441625a081714337dd44fea8011bae71959a10947b6ea33f77e128"
        "d3c6ae",
        true));
};

/* P-384, SHA-384 */
TEST_F(AsymmetricCryptoTestVectors, ECSignatureVerificationP384Test3) {
    evaluateEC(ECKeySignatureVerificationVector(
                   "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
                   "ES384"_sd,
                   "P-384"_sd,
                   "9c4479977ed377e75f5cc047edfa689ef232799513a2e70280e9b124b6c8d166e107f5494b40685"
                   "3aec4cff0f2"
                   "ca00c6f89f0f4a2d4ab0267f44512dfff110d1b1b2e5e78832022c14ac06a493ab789e696f7f0f0"
                   "60877029c27"
                   "157ce40f81258729caa4d9778bae489d3ab0259f673308ae1ec1b1948ad2845f863b36aedffb",
                   "9b3c48d924194146eca4172b6d7d618423682686f43e1dbc54ed909053d075ca53b68ae12f0f16a"
                   "1633d5d9cb1"
                   "7011ec",
                   "695039f837b68e59330ee95d11d5315a8fb5602a7b60c15142dbba6e93b5e4aba8ae4469eac39fa"
                   "6436323eccc"
                   "60dcb6",
                   "202da4e4e9632bcb6bf0f6dafb7e348528d0b469d77e46b9f939e2fa946a608dd1f166bcbcde96c"
                   "fad551701da"
                   "69f6c2",
                   "db595b49983882c48df8a396884cd98893a469c4d590e56c6a59b6150d9a0acdf142cf921510526"
                   "44702ed857a"
                   "5b7981",
                   false),
               ErrorCodes::InvalidSignature);
};

/* P-384, SHA-384 */
TEST_F(AsymmetricCryptoTestVectors, ECSignatureVerificationP384Test4) {
    evaluateEC(ECKeySignatureVerificationVector(
                   "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
                   "ES384"_sd,
                   "P-384"_sd,
                   "137b215c0150ee95e8494b79173d7ae3c3e71efcc7c75ad92f75659ce1b2d7eb555aad8026277ae"
                   "3709f46e896"
                   "963964486946b9fe269df444a6ea289ec2285e7946db57ff18f722a583194a9644e863ae452d145"
                   "7dc5db72ee2"
                   "0c486475f358dc575c621b5ab865c662e483258c7191b4cc218e1f9afeeb3e1cb978ce9657dc",
                   "cd887c65c01a1f0880bf58611bf360a8435573bc6704bfb249f1192793f6d3283637cd50f3911e5"
                   "134b0d6130a"
                   "1db60e",
                   "f2b3cbf4fe475fd15a7897561e5c898f10caa6d9d73fef10d4345917b527ce30caeaef138e21ac6"
                   "d0a49ef2fef"
                   "14bee6",
                   "addfa475b998f391144156c418561d323bdfd0c4f416a2f71a946712c349bb79ba1334c3de5b86c"
                   "2567b8657fe"
                   "4ca1f1",
                   "1c314b1339f73545ff457323470695e0474c4b6860b35d703784fbf66e9c665de6ca3acb60283df"
                   "61413e07409"
                   "06f19e",
                   false),
               ErrorCodes::InvalidSignature);
};

/* P-384, SHA-384 */
TEST_F(AsymmetricCryptoTestVectors, ECSignatureVerificationP384Test5) {
    evaluateEC(ECKeySignatureVerificationVector(
        "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
        "ES384"_sd,
        "P-384"_sd,
        "93e7e75cfaf3fa4e71df80f7f8c0ef6672a630d2dbeba1d61349acbaaa476f5f0e34dccbd85b9a815d90820331"
        "3a22fe3e919504cb222d623ad95662ea4a90099742c048341fe3a7a51110d30ad3a48a777c6347ea8b71749316"
        "e0dd1902facb304a76324b71f3882e6e70319e13fc2bb9f3f5dbb9bd2cc7265f52dfc0a3bb91",
        "a370cdbef95d1df5bf68ec487122514a107db87df3f8852068fd4694abcadb9b14302c72491a76a64442fc07bd"
        "99f02c",
        "d397c25dc1a5781573d039f2520cf329bf65120fdbe964b6b80101160e533d5570e62125b9f3276c49244b8d0f"
        "3e44ec",
        "c6c7bb516cc3f37a304328d136b2f44bb89d3dac78f1f5bcd36b412a8b4d879f6cdb75175292c696b58bfa9c91"
        "fe6391",
        "6b711425e1b14f7224cd4b96717a84d65a60ec9951a30152ea1dd3b6ea66a0088d1fd3e9a1ef069804b7d96914"
        "8c37a0",
        true));
};

/* P-384, SHA-384 */
TEST_F(AsymmetricCryptoTestVectors, ECSignatureVerificationP384Test6) {
    evaluateEC(ECKeySignatureVerificationVector(
                   "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
                   "ES384"_sd,
                   "P-384"_sd,
                   "15493aa10cfb804b3d80703ca02af7e2cfdc671447d9a171b418ecf6ca48b450414a28e7a058a78"
                   "ab0946186ad"
                   "2fe297e1b7e20e40547c74f94887a00f27dde7f78a3c15eb1115d704972b35a27caf8f7cdcce02b"
                   "96f8a72d77f"
                   "36a20d3f829e915cd3bb81f9c2997787a73616ed5cb0e864231959e0b623f12a18f779599d65",
                   "d1cf635ca04f09b58879d29012f2025479a002bda590020e6a238bccc764478131cac7e6980c670"
                   "27d92ece947"
                   "fea5a6",
                   "21f7675c2be60c0a5b7d6df2bcc89b56212a2849ec0210c59316200c59864fd86b9a19e1641d206"
                   "fd8b29af776"
                   "8b61d3",
                   "6101d26e76690634b7294b6b162dcc1a5e6233813ba09edf8567fb57a8f707e024abe0eb3ce9486"
                   "75cd518bb3b"
                   "fd4383",
                   "4e2a30f71c8f18b74184837f981a90485cd5943c7a184aba9ac787d179f170114a96ddbb8720860"
                   "a213cc289ae"
                   "340f1f",
                   false),
               ErrorCodes::InvalidSignature);
};

/* P-384, SHA-384 */
TEST_F(AsymmetricCryptoTestVectors, ECSignatureVerificationP384Test7) {
    evaluateEC(ECKeySignatureVerificationVector(
        "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
        "ES384"_sd,
        "P-384"_sd,
        "bc5582967888a425fb757bd4965900f01e6695d1547ed967c1d4f67b1b1de365d203f407698761699fec5f5a61"
        "4c21e36a9f57a8aaf852e95538f5615785534568811a9a9ccc349843f6c16dc90a4ac96a8f72c33d9589a860f4"
        "981d7b4ee7173d1db5d49c4361368504c9a6cbbaedc2c9bff2b12884379ba90433698ceb881d",
        "d15ca4b2d944d5539658a19be8ef85874f0c363b870f1cd1f2dc9cb68b2a43a10d37064697c84543e60982ab62"
        "bb32c8",
        "062fb7dfc379fc6465302ac5d8d11d3b957b594c9ef445cfe856765dd59e6f10f11809e115ac64969baa23543f"
        "2e5661",
        "e2cf123ce15ca4edad5f087778d483d9536e4a37d2d55599541c06f878e60354aa31df250b2fc4ed252b802195"
        "52c958",
        "696707a7e3f9a4b918e7c994e7332103d8e816bbe6d0d1cf72877318e087ed0e230b0d1269902f369acb432b9e"
        "97a389",
        true));
};

/* P-384, SHA-384 */
TEST_F(AsymmetricCryptoTestVectors, ECSignatureVerificationP384Test8) {
    evaluateEC(ECKeySignatureVerificationVector(
                   "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
                   "ES384"_sd,
                   "P-384"_sd,
                   "4f31331e20a3273da8fce6b03f2a86712ed5df41120a81e994d2b2f370e98ef35b847f3047d3cf5"
                   "7e88350e27b"
                   "9ac3f02073ac1838db25b5ad477aee68930882304fc052f273821056df7500dc9eab037ed3ac3c7"
                   "5396e313bf0"
                   "f4b89b26675af55f3378cf099d9d9a25a4887c1cfd2448f5b2188c41d6fa26045c5e974bf3e4",
                   "c83d30de9c4e18167cb41c990781b34b9fceb52793b4627e696796c5803515dbc4d142977d914bc"
                   "04c153261cc"
                   "5b537f",
                   "42318e5c15d65c3f545189781619267d899250d80acc611fe7ed0943a0f5bfc9d4328ff7ccf675a"
                   "e0aac069ccb"
                   "4b4d6e",
                   "b567c37f7c84107ef72639e52065486c2e5bf4125b861d37ea3b44fc0b75bcd96dcea3e4dbb9e8f"
                   "4f45923240b"
                   "2b9e44",
                   "d06266e0f27cfe4be1c6210734a8fa689a6cd1d63240cb19127961365e35890a5f1b464dcb4305f"
                   "3e8295c6f84"
                   "2ef344",
                   false),
               ErrorCodes::InvalidSignature);
};


/* P-256, SHA-256 */
TEST_F(AsymmetricCryptoTestVectors, ECSignatureVerificationInvalidECCurveTest1) {
    evaluateFailedEC(
        ECKeySignatureVerificationVector(
            "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
            "XXX"_sd,
            "XXX"_sd,
            "e1130af6a38ccb412a9c8d13e15dbfc9e69a16385af3c3f1e5da954fd5e7c45fd75e2b8c3669922"
            "8e92840c056"
            "2fbf3772f07e17f1add56588dd45f7450e1217ad239922dd9c32695dc71ff2424ca0dec1321aa47"
            "064a044b7fe"
            "3c2b97d03ce470a592304c5ef21eed9f93da56bb232d1eeb0035f9bf0dfafdcc4606272b20a3",
            "e424dc61d4bb3cb7ef4344a7f8957a0c5134e16f7a67c074f82e6e12f49abf3c",
            "970eed7aa2bc48651545949de1dddaf0127e5965ac85d1243d6f60e7dfaee927",
            "bf96b99aa49c705c910be33142017c642ff540c76349b9dab72f981fd9347f4f",
            "17c55095819089c2e03b9cd415abdf12444e323075d98f31920b9e0f57ec871c",
            false),
        ErrorCodes::OperationFailed);
}

TEST_F(AsymmetricCryptoTestVectors, ECSignatureVerificationInvalidXCoordinateTest2) {
    evaluateFailedEC(
        ECKeySignatureVerificationVector(
            "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
            "ES256"_sd,
            "P-256"_sd,
            "60cd64b2cd2be6c33859b94875120361a24085f3765cb8b2bf11e026fa9d8855dbe435acf7882e8"
            "4f3c7857f96"
            "e2baab4d9afe4588e4a82e17a78827bfdb5ddbd1c211fbc2e6d884cddd7cb9d90d5bf4a7311b83f"
            "35250803381"
            "2c776a0e00c003c7e0d628e50736c7512df0acfa9f2320bd102229f46495ae6d0857cc452a84",
            "2d98ea01f754d34bbc3003df5050200abf445ec728556d7ed7d5c54c55552b6d2d98",
            "9b52672742d637a32add056dfd6d8792f2a33c2e69dafabea09b960bc61e230a",
            "06108e525f845d0155bf60193222b3219c98e3d49424c2fb2a0987f825c17959",
            "62b5cdd591e5b507e560167ba8f6f7cda74673eb315680cb89ccbc4eec477dce",
            false),
        ErrorCodes::OperationFailed);
};

TEST_F(AsymmetricCryptoTestVectors, ECSignatureVerificationInvalidYCoordinateTest3) {
    evaluateFailedEC(
        ECKeySignatureVerificationVector(
            "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
            "ES384"_sd,
            "P-384"_sd,
            "93e7e75cfaf3fa4e71df80f7f8c0ef6672a630d2dbeba1d61349acbaaa476f5f0e34dccbd85b9a815d9082"
            "03313a22fe3e919504cb222d623ad95662ea4a90099742c048341fe3a7a51110d30ad3a48a777c6347ea8b"
            "71749316e0dd1902facb304a76324b71f3882e6e70319e13fc2bb9f3f5dbb9bd2cc7265f52dfc0a3bb91",
            "a370cdbef95d1df5bf68ec487122514a107db87df3f8852068fd4694abcadb9b14302c72491a76a64442fc"
            "07bd99f02c",
            "d397c25dc1a5781573d039f2520cf329bf65120fdbe964b6b80101160e533d5570e6212549244b8d0f3e44"
            "ec",
            "c6c7bb516cc3f37a304328d136b2f44bb89d3dac78f1f5bcd36b412a8b4d879f6cdb75175292c696b58bfa"
            "9c91fe6391",
            "6b711425e1b14f7224cd4b96717a84d65a60ec9951a30152ea1dd3b6ea66a0088d1fd3e9a1ef069804b7d9"
            "69148c37a0",
            false),
        ErrorCodes::OperationFailed);
};

TEST_F(AsymmetricCryptoTestVectors, ECSignatureVerificationInvalidRLength) {
    evaluateEC(
        ECKeySignatureVerificationVector(
            "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
            "ES256"_sd,
            "P-256"_sd,
            "73c5f6a67456ae48209b5f85d1e7de7758bf235300c6ae2bdceb1dcb27a7730fb68c950b7fcada0ecc4661"
            "d357"
            "8230f225a875e69aaa17f1e71c6be5c831f22663bac63d0c7a9635edb0043ff8c6f26470f02a7bc56556f1"
            "437f"
            "06dfa27b487a6c4290d8bad38d4879b334e341ba092dde4e4ae694a9c09302e2dbf443581c08",
            "e0fc6a6f50e1c57475673ee54e3a57f9a49f3328e743bf52f335e3eeaa3d2864",
            "7f59d689c91e463607d9194d99faf316e25432870816dde63f5d4b373f12f22a",
            "1d75830cd36f4c9aa181b2c4221e87f176b7f05b7c87824e82e396c88317",
            "cb2acb01dac96efc53a32d4a0d85d0c2e48955214783ecf50a4f0414a319c05a",
            false),
        ErrorCodes::InvalidSignature);
};

TEST_F(AsymmetricCryptoTestVectors, ECSignatureVerificationInvalidSLength) {
    evaluateEC(
        ECKeySignatureVerificationVector(
            "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
            "ES256"_sd,
            "P-256"_sd,
            "73c5f6a67456ae48209b5f85d1e7de7758bf235300c6ae2bdceb1dcb27a7730fb68c950b7fcada0ecc4661"
            "d357"
            "8230f225a875e69aaa17f1e71c6be5c831f22663bac63d0c7a9635edb0043ff8c6f26470f02a7bc56556f1"
            "437f"
            "06dfa27b487a6c4290d8bad38d4879b334e341ba092dde4e4ae694a9c09302e2dbf443581c08",
            "e0fc6a6f50e1c57475673ee54e3a57f9a49f3328e743bf52f335e3eeaa3d2864",
            "7f59d689c91e463607d9194d99faf316e25432870816dde63f5d4b373f12f22a",
            "1d75830cd36f4c9aa181b2c4221e87f176b7f05b7c87824e82e396c88315c407",
            "cb2acb01dac96efc53a32d4a0d85d0c2e48955214783ecf50a44a319c05a",
            false),
        ErrorCodes::InvalidSignature);
};

TEST_F(AsymmetricCryptoTestVectors, ECSignatureVerificationUnsupportedAlgorithm) {
    BSONObj key;
    auto asymmetricKey = JWSValidator::create("XX"_sd, key);
    Status status = asymmetricKey.getStatus();
    ASSERT_EQ(status.code(), ErrorCodes::UnsupportedFormat);
}

TEST_F(AsymmetricCryptoTestVectors, ECSignatureVerificationInvalidAlgorithmAtValidate) {
    evaluateInvalidAlgorithmEC(
        ECKeySignatureVerificationVector(
            "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
            "ES256"_sd,
            "P-256"_sd,
            "73c5f6a67456ae48209b5f85d1e7de7758bf235300c6ae2bdceb1dcb27a7730fb68c950b7fcada0ecc4661"
            "d357"
            "8230f225a875e69aaa17f1e71c6be5c831f22663bac63d0c7a9635edb0043ff8c6f26470f02a7bc56556f1"
            "437f"
            "06dfa27b487a6c4290d8bad38d4879b334e341ba092dde4e4ae694a9c09302e2dbf443581c08",
            "e0fc6a6f50e1c57475673ee54e3a57f9a49f3328e743bf52f335e3eeaa3d2864",
            "7f59d689c91e463607d9194d99faf316e25432870816dde63f5d4b373f12f22a",
            "1d75830cd36f4c9aa181b2c4221e87f176b7f05b7c87824e82e396c88315c407",
            "cb2acb01dac96efc53a32d4a0d85d0c2e48955214783ecf50a4f0414a319c05a",
            false),
        ErrorCodes::UnsupportedFormat);
};

TEST_F(AsymmetricCryptoTestVectors, ECSignatureVerificationEmptyPayloadAtValidate) {
    evaluateEmptyPayload(
        ECKeySignatureVerificationVector(
            "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd,
            "ES256"_sd,
            "P-256"_sd,
            "73c5f6a67456ae48209b5f85d1e7de7758bf235300c6ae2bdceb1dcb27a7730fb68c950b7fcada0ecc4661"
            "d357"
            "8230f225a875e69aaa17f1e71c6be5c831f22663bac63d0c7a9635edb0043ff8c6f26470f02a7bc56556f1"
            "437f"
            "06dfa27b487a6c4290d8bad38d4879b334e341ba092dde4e4ae694a9c09302e2dbf443581c08",
            "e0fc6a6f50e1c57475673ee54e3a57f9a49f3328e743bf52f335e3eeaa3d2864",
            "7f59d689c91e463607d9194d99faf316e25432870816dde63f5d4b373f12f22a",
            "1d75830cd36f4c9aa181b2c4221e87f176b7f05b7c87824e82e396c88315c407",
            "cb2acb01dac96efc53a32d4a0d85d0c2e48955214783ecf50a4f0414a319c05a",
            false),
        ErrorCodes::InvalidSignature);
};

}  // namespace mongo::crypto
#endif
