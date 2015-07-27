/******************************************************************************
  Copyright (c) 2007-2011, Intel Corp.
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of Intel Corporation nor the names of its contributors
      may be used to endorse or promote products derived from this software
      without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
  THE POSSIBILITY OF SUCH DAMAGE.
******************************************************************************/

#include "dpml_private.h"


#ifndef DEFINES

    const unsigned __int64 __four_over_pi[] = { 

        0x0000000000000000ull, 0x0000000000000000ull, 0x0028be60db939105ull,
        0x4a7f09d5f47d4d37ull, 0x7036d8a5664f10e4ull, 0x107f9458eaf7aef1ull,
        0x586dc91b8e909374ull, 0xb801924bba827464ull, 0x873f877ac72c4a69ull,
        0xcfba208d7d4baed1ull, 0x213a671c09ad17dfull, 0x904e64758e60d4ceull,
        0x7d272117e2ef7e4aull, 0x0ec7fe25fff78166ull, 0x03fbcbc462d6829bull,
        0x47db4d9fb3c9f2c2ull, 0x6dd3d18fd9a797faull, 0x8b5d49eeb1faf97cull,
        0x5ecf41ce7de294a4ull, 0xba9afed7ec47e357ull, 0x421580cc11bf1edaull,
        0xeafc33ef0826bd0dull, 0x876a78e45857b986ull, 0xc219666157c5281aull,
        0x10237ff620135cc9ull, 0xcc41818555b29ceaull, 0x3258389ef0231ad1ull,
        0xf10670d9f3773a02ull, 0x4aa0d6711da2e587ull, 0x29b76bd13455c641ull,
        0x4fa97fc1c14fdf8cull, 0xfa0cb0b793e60c9full, 0x6ef0cf49bbdac797ull,
        0xbe27ce87cd72bc9full, 0xc761fc48641f1f09ull, 0x1abe9bb55dcb4c10ull,
        0xcec571852d674670ull, 0xf0b12b50534b1740ull, 0x03119f618b5c78e6ull,
        0xb1a6c0188cdf34adull, 0x25e9ed35554dfd8full, 0xb5c60428ff1d934aull,
        0xa7592af5dc3e1f18ull, 0xd5ec1eb9c545d592ull, 0x7036758ece2129f2ull,
        0xc8c91de2b588d516ull, 0xae47c006c2bc77f3ull, 0x867fcc67da879998ull,
        0x55e651feeb361fdfull, 0xadd948a27a0c982full, 0xf9b3713bc24d9b35ull,
        0x0fd775f785b78ed6ull, 0x24a6f78a08b4ba21ull, 0x8a1356388cb2b185ull,
        0xb8c232df78143005ull, 0xe9c77cd6f8060d04ull, 0xcb9884a0c05220d6ull,
        0xe3bd5fec2b7cba47ull, 0x90d29234d9c43637ull, 0x6a9097ebb3985aa9ull,
        0x0a02ad2674fca981ull, 0x9fddd720f0a8e20full, 0x185e1ce296a32befull,
        0x75dbd8e98b72effdull, 0x3be06359f0499172ull, 0x954db672b4aa0a23ull,
        0x58709df244850981ull, 0x26d184b116711131ull, 0x72246c937cc5c02bull,
        0x50f539524a44357full, 0x7f2f80332507bbb3ull, 0x9c3d4f84e03c7b30ull,
        0xf9ecca3e31e50164ull, 0xcf9c706cc24bbcd1ull, 0x42e704a21ec82ae7ull,
        0xed4bb0a491cbcc9eull, 0xdb55432429dc87f9ull, 0xdae5b2cc52859e78ull,
        0x9e506277fd25e53aull, 0x2139b8a5cc665afbull, 0x620d97d7c3bf6eedull,
        0x26921b2919d09c9cull, 0x4c97636e0567c279ull, 0x6f094c634e5d3dc7ull,
        0x014c0043035a0212ull, 0xd63b8b242a91c0b9ull, 0xdd0935af699f7ddcull,
        0x921bbbc5a7e9a523ull, 0xbda46d1454f47c82ull, 0xb3cce6081f92fd5aull,
        0x18ec97cfb740d750ull, 0x1fe2614a54957019ull, 0x0dc4361b4c920c9dull,
        0x5316f51c539b9511ull, 0x704242da7d4ab559ull, 0x852741c9d4011776ull,
        0xceed315dba85fe61ull, 0xdf5ad26e89c74a5aull, 0x65ab333195052b5aull,
        0xb8a4227662141c8bull, 0x2fa9012501dddc0cull, 0x3cc9ff002a1c7a92ull,
        0x70998f781920f765ull, 0xe5cfe8ff6510e321ull, 0x8377904c674e64a3ull,
        0x1c3779edc5cef7c2ull, 0x0acdc568201724e0ull, 0x16a48444363a03ebull,
        0xe01b12fff6c3e40eull, 0x1d8616456958aef2ull, 0xd86e6271ef500401ull,
        0x3cb489dd527dadbaull, 0xeec8b6ea85028bc9ull, 0xa25da0d90ccec246ull,
        0xa503aa8e9470a8c7ull, 0x6bbb6bc489971370ull, 0x9b671e8b65d5b020ull,
        0xcfc0fdbc0263100aull, 0xe64c5b41ed0e4548ull, 0x0316f0f63124bd52ull,
        0xeb71a97293b34de9ull, 0xcdaa79a524aada10ull, 0xb77798c67be31d94ull,
        0xa2da0df6ff2ae86bull, 0x8c4577e86b8036beull, 0xc31993592dc17b4cull,
        0x194a6fd595cebfd1ull, 0xee7e5abcef9d77e4ull, 0xca0c202afda31985ull,
        0x72c10188be877936ull, 0x692ccf63c6d5c273ull, 0x4dba5093a92f84edull,
        0x48ccc6aabc2a1953ull, 0xe9707483cfc2f35eull, 0x16ddbe48c122dedcull,
        0x85e254e9b1b89b9bull, 0xc03afbd612a6edf6ull, 0xb12e99aab3f3dd87ull,
        0x40b44b7c6c706663ull, 0x1deb70f69221a817ull, 0x7dfd20318bfc2b26ull,
        0xbb376f170fdb77b4ull, 0x07f1e42db6ca8e89ull, 0x68e6abc024d4eb41ull,
        0x15edad0b4a5fa012ull, 0xe9c1f683aa9da856ull, 0x5eca84858b6df73full,
        0x797ebfb6e27f6fa2ull, 0x5b1db93f2a419c20ull, 0x0f855ba17fe1ff41ull,
        0xcf8a0cd9d861860aull, 0xbaaf536bf9ecdb9bull, 0x63ce59e556efcc52ull,
        0x35e105b7cc10cb71ull, 0xcd5849739c326e32ull, 0xcc3f5b2fe8802939ull,
        0x1b0168375691dbc8ull, 0x748498a1172e5258ull, 0x5c38159ac054a64dull,
        0xd5542df547b13c4cull, 0xd7db84f90c176a4bull, 0xa170ec874d8ca869ull,
        0x2dc2352c7a887dc5ull, 0xb91a63ddffc9e000ull, 0xc30b5023683353e6ull,
        0x694834e8acc2974bull, 0xd0be6d32f684742full, 0x9f7076e6ef45eae0ull,
        0x68b2971a8205d54bull, 0x954009fc051fe181ull, 0xf85902c5235065b7ull,
        0xafa1cabf76ad895aull, 0xcd225effbcc167afull, 0xee53da9a2a0a9296ull,
        0xb113ef3e0b6616b5ull, 0xe571fd235343698eull, 0x8817d5e92c4fc525ull,
        0x4e2000483321b75cull, 0x6db7b27d582fc459ull, 0x535ac1c06b2c2334ull,
        0x302c92155443bec7ull, 0xb0dca54ec1a8cd50ull, 0x301ef701b311783eull,
        0x8a53b232b5907cfaull, 0x37991f361926cc6full, 0xb670e5e935161df1ull,
        0x78da44f6bc0f0eaeull, 0x91861197dd557d6full, 0x74b1a49b974bab3bull,
        0x5103908f8721f118ull, 0x7a7f4a7cf5b9f29full, 0x088d645bf1780223ull,
        0x75fff89a9bb1bf6cull, 0x304224dd175f2cabull, 0x5ae75bb35edc8f9aull,
        0x8471aa73fdf7dccaull, 0x6eb26d54402dc36cull, 0xb8892e9d181f7962ull,
        0xb61d0b0543430620ull, 0x65199f858a405d9eull, 0xa7efbf7f7bd1558dull,
        0x9fb644f67b2e6ea2ull, 0xff25f109ea0c70dbull, 0xbc4db16515aa362dull,
        0x6a2d03b333cb6244ull, 0x8d15dbe2558b38f3ull, 0xa66e4835aa979ae7ull,
        0x0a8fb317c45282ffull, 0x7efd385b4ee38b21ull, 0xb8a1353a6a6d3f34ull,
        0x7bbbf24d4b984e4bull, 0xd1084e323646c2bfull, 0x205a92bef6070be1ull,
        0x2d14e32653b30895ull, 0x37154ab5b1b02586ull, 0x42ee1c0699255a58ull,
        0x1689bb948fc3c45full, 0xc46d7d3d72ff0b6full, 0x0d3baf0d33177a18ull,
        0x17b766e399fbcce4ull, 0xae05f266d6186f15ull, 0xf871a0d4440fb612ull,
        0x1c7777470b68462bull, 0xd18b0875fcd6661eull, 0xb6701527bea193ffull,
        0x0195ab9e794d88a2ull, 0x48ab4e3724d9eabaull, 0x154e09a0a6f9f2a9ull,
        0x03546c4ce643b5eaull, 0x52015a7c2c9969e2ull, 0x1fe5d3220db47e6cull,
        0xe48852a09ec873e6ull, 0x3727d01551f70e9dull, 0x3850bad9f7e77f97ull,
        0xf517a919dedeab2eull, 0xa8bd9548e20ad56eull, 0x90421b96618a8860ull,
        0xd1ce79b8e27527b9ull, 0x503ed27a55bff283ull, 0xc72296714afea531ull,
        0x7074f3f143eb96b6ull, 0xe1b151d890e14ee1ull, 0x88651e4b21d8441eull,
        0xd30a868b2004afd0ull, 0xe409a2224f1e3931ull, 0x2a1ef6f9708eb13aull,
        0xbd09a299fdefe483ull, 0x4ae8d96c64cf42dfull, 0x2f77146918f749f7ull,
        0x785a466526a54a6aull, 0x0a339a2d3b424827ull, 0xd132a61398e09c08ull,
        0xdf1f8cae43e3bd69ull, 0xf9d585023c484aa7ull, 0x6d535f9bd446696aull,
        0xfe6d75b7e0987765ull, 0x808d85a7ceb12868ull, 0xa0db7b5c9ea34e6aull,
        0x6e20970c9ad6c9d1ull, 0xbb4d001dc034957dull, 0x3f135640601c7838ull,
        0x4fe26ca57cd92a3cull, 0x6ba9d2ce3f133aacull,
	};


#else

    /* Describe the trig_reduce interface */
#   define NUM_INDEX_BITS		7
#   define NUM_OCTANT_BITS		10
#   define VOC			0
#   define BIX			1
#   define MIN_OVERHANG		6

    /* Describe the table */
#   define FOUR_OV_PI_ZERO_PAD_LEN	138
#   define BITS_PER_DIGIT		64
#   define DIGIT_TYPE			U_INT_64
#   define SIGNED_DIGIT_TYPE		INT_64
#   undef  FOUR_OVER_PI_TABLE_NAME
#   define FOUR_OVER_PI_TABLE_NAME	__four_over_pi
    extern const DIGIT_TYPE FOUR_OVER_PI_TABLE_NAME[];

#endif

