<?xml version="1.0"?>

<xsl:stylesheet version="1.0"
	xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
	xmlns:doc="http://xsltsl.org/xsl/documentation/1.0"
	xmlns:str="http://xsltsl.org/string"
	extension-element-prefixes="doc str">

  <doc:reference xmlns="">
    <referenceinfo>
      <releaseinfo role="meta">
	$Id: string.xsl 3991 2004-11-10 06:51:55Z balls $
      </releaseinfo>
      <author>
	<surname>Ball</surname>
	<firstname>Steve</firstname>
      </author>
      <copyright>
	<year>2002</year>
	<year>2001</year>
	<holder>Steve Ball</holder>
      </copyright>
    </referenceinfo>

    <title>String Processing</title>

    <partintro>
      <section>
	<title>Introduction</title>

	<para>This module provides templates for manipulating strings.</para>

      </section>
    </partintro>

  </doc:reference>

  <!-- Common string constants and datasets as XSL variables -->

  <!-- str:lower and str:upper contain pairs of lower and upper case
       characters. Below insanely long strings should contain the
       official lower/uppercase pairs, making this stylesheet working
       for every language on earth. Hopefully. -->
  <!-- These values are not enough, however. There are some
       exceptions, dealt with below. -->
  <xsl:variable name="xsltsl-str-lower" select="'&#x0061;&#x0062;&#x0063;&#x0064;&#x0065;&#x0066;&#x0067;&#x0068;&#x0069;&#x006A;&#x006B;&#x006C;&#x006D;&#x006E;&#x006F;&#x0070;&#x0071;&#x0072;&#x0073;&#x0074;&#x0075;&#x0076;&#x0077;&#x0078;&#x0079;&#x007A;&#x00B5;&#x00E0;&#x00E1;&#x00E2;&#x00E3;&#x00E4;&#x00E5;&#x00E6;&#x00E7;&#x00E8;&#x00E9;&#x00EA;&#x00EB;&#x00EC;&#x00ED;&#x00EE;&#x00EF;&#x00F0;&#x00F1;&#x00F2;&#x00F3;&#x00F4;&#x00F5;&#x00F6;&#x00F8;&#x00F9;&#x00FA;&#x00FB;&#x00FC;&#x00FD;&#x00FE;&#x00FF;&#x0101;&#x0103;&#x0105;&#x0107;&#x0109;&#x010B;&#x010D;&#x010F;&#x0111;&#x0113;&#x0115;&#x0117;&#x0119;&#x011B;&#x011D;&#x011F;&#x0121;&#x0123;&#x0125;&#x0127;&#x0129;&#x012B;&#x012D;&#x012F;&#x0131;&#x0133;&#x0135;&#x0137;&#x013A;&#x013C;&#x013E;&#x0140;&#x0142;&#x0144;&#x0146;&#x0148;&#x014B;&#x014D;&#x014F;&#x0151;&#x0153;&#x0155;&#x0157;&#x0159;&#x015B;&#x015D;&#x015F;&#x0161;&#x0163;&#x0165;&#x0167;&#x0169;&#x016B;&#x016D;&#x016F;&#x0171;&#x0173;&#x0175;&#x0177;&#x017A;&#x017C;&#x017E;&#x017F;&#x0183;&#x0185;&#x0188;&#x018C;&#x0192;&#x0195;&#x0199;&#x01A1;&#x01A3;&#x01A5;&#x01A8;&#x01AD;&#x01B0;&#x01B4;&#x01B6;&#x01B9;&#x01BD;&#x01BF;&#x01C5;&#x01C6;&#x01C8;&#x01C9;&#x01CB;&#x01CC;&#x01CE;&#x01D0;&#x01D2;&#x01D4;&#x01D6;&#x01D8;&#x01DA;&#x01DC;&#x01DD;&#x01DF;&#x01E1;&#x01E3;&#x01E5;&#x01E7;&#x01E9;&#x01EB;&#x01ED;&#x01EF;&#x01F2;&#x01F3;&#x01F5;&#x01F9;&#x01FB;&#x01FD;&#x01FF;&#x0201;&#x0203;&#x0205;&#x0207;&#x0209;&#x020B;&#x020D;&#x020F;&#x0211;&#x0213;&#x0215;&#x0217;&#x0219;&#x021B;&#x021D;&#x021F;&#x0223;&#x0225;&#x0227;&#x0229;&#x022B;&#x022D;&#x022F;&#x0231;&#x0233;&#x0253;&#x0254;&#x0256;&#x0257;&#x0259;&#x025B;&#x0260;&#x0263;&#x0268;&#x0269;&#x026F;&#x0272;&#x0275;&#x0280;&#x0283;&#x0288;&#x028A;&#x028B;&#x0292;&#x0345;&#x03AC;&#x03AD;&#x03AE;&#x03AF;&#x03B1;&#x03B2;&#x03B3;&#x03B4;&#x03B5;&#x03B6;&#x03B7;&#x03B8;&#x03B9;&#x03BA;&#x03BB;&#x03BC;&#x03BD;&#x03BE;&#x03BF;&#x03C0;&#x03C1;&#x03C2;&#x03C3;&#x03C4;&#x03C5;&#x03C6;&#x03C7;&#x03C8;&#x03C9;&#x03CA;&#x03CB;&#x03CC;&#x03CD;&#x03CE;&#x03D0;&#x03D1;&#x03D5;&#x03D6;&#x03DB;&#x03DD;&#x03DF;&#x03E1;&#x03E3;&#x03E5;&#x03E7;&#x03E9;&#x03EB;&#x03ED;&#x03EF;&#x03F0;&#x03F1;&#x03F2;&#x03F5;&#x0430;&#x0431;&#x0432;&#x0433;&#x0434;&#x0435;&#x0436;&#x0437;&#x0438;&#x0439;&#x043A;&#x043B;&#x043C;&#x043D;&#x043E;&#x043F;&#x0440;&#x0441;&#x0442;&#x0443;&#x0444;&#x0445;&#x0446;&#x0447;&#x0448;&#x0449;&#x044A;&#x044B;&#x044C;&#x044D;&#x044E;&#x044F;&#x0450;&#x0451;&#x0452;&#x0453;&#x0454;&#x0455;&#x0456;&#x0457;&#x0458;&#x0459;&#x045A;&#x045B;&#x045C;&#x045D;&#x045E;&#x045F;&#x0461;&#x0463;&#x0465;&#x0467;&#x0469;&#x046B;&#x046D;&#x046F;&#x0471;&#x0473;&#x0475;&#x0477;&#x0479;&#x047B;&#x047D;&#x047F;&#x0481;&#x048D;&#x048F;&#x0491;&#x0493;&#x0495;&#x0497;&#x0499;&#x049B;&#x049D;&#x049F;&#x04A1;&#x04A3;&#x04A5;&#x04A7;&#x04A9;&#x04AB;&#x04AD;&#x04AF;&#x04B1;&#x04B3;&#x04B5;&#x04B7;&#x04B9;&#x04BB;&#x04BD;&#x04BF;&#x04C2;&#x04C4;&#x04C8;&#x04CC;&#x04D1;&#x04D3;&#x04D5;&#x04D7;&#x04D9;&#x04DB;&#x04DD;&#x04DF;&#x04E1;&#x04E3;&#x04E5;&#x04E7;&#x04E9;&#x04EB;&#x04ED;&#x04EF;&#x04F1;&#x04F3;&#x04F5;&#x04F9;&#x0561;&#x0562;&#x0563;&#x0564;&#x0565;&#x0566;&#x0567;&#x0568;&#x0569;&#x056A;&#x056B;&#x056C;&#x056D;&#x056E;&#x056F;&#x0570;&#x0571;&#x0572;&#x0573;&#x0574;&#x0575;&#x0576;&#x0577;&#x0578;&#x0579;&#x057A;&#x057B;&#x057C;&#x057D;&#x057E;&#x057F;&#x0580;&#x0581;&#x0582;&#x0583;&#x0584;&#x0585;&#x0586;&#x1E01;&#x1E03;&#x1E05;&#x1E07;&#x1E09;&#x1E0B;&#x1E0D;&#x1E0F;&#x1E11;&#x1E13;&#x1E15;&#x1E17;&#x1E19;&#x1E1B;&#x1E1D;&#x1E1F;&#x1E21;&#x1E23;&#x1E25;&#x1E27;&#x1E29;&#x1E2B;&#x1E2D;&#x1E2F;&#x1E31;&#x1E33;&#x1E35;&#x1E37;&#x1E39;&#x1E3B;&#x1E3D;&#x1E3F;&#x1E41;&#x1E43;&#x1E45;&#x1E47;&#x1E49;&#x1E4B;&#x1E4D;&#x1E4F;&#x1E51;&#x1E53;&#x1E55;&#x1E57;&#x1E59;&#x1E5B;&#x1E5D;&#x1E5F;&#x1E61;&#x1E63;&#x1E65;&#x1E67;&#x1E69;&#x1E6B;&#x1E6D;&#x1E6F;&#x1E71;&#x1E73;&#x1E75;&#x1E77;&#x1E79;&#x1E7B;&#x1E7D;&#x1E7F;&#x1E81;&#x1E83;&#x1E85;&#x1E87;&#x1E89;&#x1E8B;&#x1E8D;&#x1E8F;&#x1E91;&#x1E93;&#x1E95;&#x1E9B;&#x1EA1;&#x1EA3;&#x1EA5;&#x1EA7;&#x1EA9;&#x1EAB;&#x1EAD;&#x1EAF;&#x1EB1;&#x1EB3;&#x1EB5;&#x1EB7;&#x1EB9;&#x1EBB;&#x1EBD;&#x1EBF;&#x1EC1;&#x1EC3;&#x1EC5;&#x1EC7;&#x1EC9;&#x1ECB;&#x1ECD;&#x1ECF;&#x1ED1;&#x1ED3;&#x1ED5;&#x1ED7;&#x1ED9;&#x1EDB;&#x1EDD;&#x1EDF;&#x1EE1;&#x1EE3;&#x1EE5;&#x1EE7;&#x1EE9;&#x1EEB;&#x1EED;&#x1EEF;&#x1EF1;&#x1EF3;&#x1EF5;&#x1EF7;&#x1EF9;&#x1F00;&#x1F01;&#x1F02;&#x1F03;&#x1F04;&#x1F05;&#x1F06;&#x1F07;&#x1F10;&#x1F11;&#x1F12;&#x1F13;&#x1F14;&#x1F15;&#x1F20;&#x1F21;&#x1F22;&#x1F23;&#x1F24;&#x1F25;&#x1F26;&#x1F27;&#x1F30;&#x1F31;&#x1F32;&#x1F33;&#x1F34;&#x1F35;&#x1F36;&#x1F37;&#x1F40;&#x1F41;&#x1F42;&#x1F43;&#x1F44;&#x1F45;&#x1F51;&#x1F53;&#x1F55;&#x1F57;&#x1F60;&#x1F61;&#x1F62;&#x1F63;&#x1F64;&#x1F65;&#x1F66;&#x1F67;&#x1F70;&#x1F71;&#x1F72;&#x1F73;&#x1F74;&#x1F75;&#x1F76;&#x1F77;&#x1F78;&#x1F79;&#x1F7A;&#x1F7B;&#x1F7C;&#x1F7D;&#x1F80;&#x1F81;&#x1F82;&#x1F83;&#x1F84;&#x1F85;&#x1F86;&#x1F87;&#x1F90;&#x1F91;&#x1F92;&#x1F93;&#x1F94;&#x1F95;&#x1F96;&#x1F97;&#x1FA0;&#x1FA1;&#x1FA2;&#x1FA3;&#x1FA4;&#x1FA5;&#x1FA6;&#x1FA7;&#x1FB0;&#x1FB1;&#x1FB3;&#x1FBE;&#x1FC3;&#x1FD0;&#x1FD1;&#x1FE0;&#x1FE1;&#x1FE5;&#x1FF3;&#x2170;&#x2171;&#x2172;&#x2173;&#x2174;&#x2175;&#x2176;&#x2177;&#x2178;&#x2179;&#x217A;&#x217B;&#x217C;&#x217D;&#x217E;&#x217F;&#x24D0;&#x24D1;&#x24D2;&#x24D3;&#x24D4;&#x24D5;&#x24D6;&#x24D7;&#x24D8;&#x24D9;&#x24DA;&#x24DB;&#x24DC;&#x24DD;&#x24DE;&#x24DF;&#x24E0;&#x24E1;&#x24E2;&#x24E3;&#x24E4;&#x24E5;&#x24E6;&#x24E7;&#x24E8;&#x24E9;&#xFF41;&#xFF42;&#xFF43;&#xFF44;&#xFF45;&#xFF46;&#xFF47;&#xFF48;&#xFF49;&#xFF4A;&#xFF4B;&#xFF4C;&#xFF4D;&#xFF4E;&#xFF4F;&#xFF50;&#xFF51;&#xFF52;&#xFF53;&#xFF54;&#xFF55;&#xFF56;&#xFF57;&#xFF58;&#xFF59;&#xFF5A;&#x10428;&#x10429;&#x1042A;&#x1042B;&#x1042C;&#x1042D;&#x1042E;&#x1042F;&#x10430;&#x10431;&#x10432;&#x10433;&#x10434;&#x10435;&#x10436;&#x10437;&#x10438;&#x10439;&#x1043A;&#x1043B;&#x1043C;&#x1043D;&#x1043E;&#x1043F;&#x10440;&#x10441;&#x10442;&#x10443;&#x10444;&#x10445;&#x10446;&#x10447;&#x10448;&#x10449;&#x1044A;&#x1044B;&#x1044C;&#x1044D;'"/>
  <xsl:variable name="xsltsl-str-upper" select="'&#x0041;&#x0042;&#x0043;&#x0044;&#x0045;&#x0046;&#x0047;&#x0048;&#x0049;&#x004A;&#x004B;&#x004C;&#x004D;&#x004E;&#x004F;&#x0050;&#x0051;&#x0052;&#x0053;&#x0054;&#x0055;&#x0056;&#x0057;&#x0058;&#x0059;&#x005A;&#x039C;&#x00C0;&#x00C1;&#x00C2;&#x00C3;&#x00C4;&#x00C5;&#x00C6;&#x00C7;&#x00C8;&#x00C9;&#x00CA;&#x00CB;&#x00CC;&#x00CD;&#x00CE;&#x00CF;&#x00D0;&#x00D1;&#x00D2;&#x00D3;&#x00D4;&#x00D5;&#x00D6;&#x00D8;&#x00D9;&#x00DA;&#x00DB;&#x00DC;&#x00DD;&#x00DE;&#x0178;&#x0100;&#x0102;&#x0104;&#x0106;&#x0108;&#x010A;&#x010C;&#x010E;&#x0110;&#x0112;&#x0114;&#x0116;&#x0118;&#x011A;&#x011C;&#x011E;&#x0120;&#x0122;&#x0124;&#x0126;&#x0128;&#x012A;&#x012C;&#x012E;&#x0049;&#x0132;&#x0134;&#x0136;&#x0139;&#x013B;&#x013D;&#x013F;&#x0141;&#x0143;&#x0145;&#x0147;&#x014A;&#x014C;&#x014E;&#x0150;&#x0152;&#x0154;&#x0156;&#x0158;&#x015A;&#x015C;&#x015E;&#x0160;&#x0162;&#x0164;&#x0166;&#x0168;&#x016A;&#x016C;&#x016E;&#x0170;&#x0172;&#x0174;&#x0176;&#x0179;&#x017B;&#x017D;&#x0053;&#x0182;&#x0184;&#x0187;&#x018B;&#x0191;&#x01F6;&#x0198;&#x01A0;&#x01A2;&#x01A4;&#x01A7;&#x01AC;&#x01AF;&#x01B3;&#x01B5;&#x01B8;&#x01BC;&#x01F7;&#x01C4;&#x01C4;&#x01C7;&#x01C7;&#x01CA;&#x01CA;&#x01CD;&#x01CF;&#x01D1;&#x01D3;&#x01D5;&#x01D7;&#x01D9;&#x01DB;&#x018E;&#x01DE;&#x01E0;&#x01E2;&#x01E4;&#x01E6;&#x01E8;&#x01EA;&#x01EC;&#x01EE;&#x01F1;&#x01F1;&#x01F4;&#x01F8;&#x01FA;&#x01FC;&#x01FE;&#x0200;&#x0202;&#x0204;&#x0206;&#x0208;&#x020A;&#x020C;&#x020E;&#x0210;&#x0212;&#x0214;&#x0216;&#x0218;&#x021A;&#x021C;&#x021E;&#x0222;&#x0224;&#x0226;&#x0228;&#x022A;&#x022C;&#x022E;&#x0230;&#x0232;&#x0181;&#x0186;&#x0189;&#x018A;&#x018F;&#x0190;&#x0193;&#x0194;&#x0197;&#x0196;&#x019C;&#x019D;&#x019F;&#x01A6;&#x01A9;&#x01AE;&#x01B1;&#x01B2;&#x01B7;&#x0399;&#x0386;&#x0388;&#x0389;&#x038A;&#x0391;&#x0392;&#x0393;&#x0394;&#x0395;&#x0396;&#x0397;&#x0398;&#x0399;&#x039A;&#x039B;&#x039C;&#x039D;&#x039E;&#x039F;&#x03A0;&#x03A1;&#x03A3;&#x03A3;&#x03A4;&#x03A5;&#x03A6;&#x03A7;&#x03A8;&#x03A9;&#x03AA;&#x03AB;&#x038C;&#x038E;&#x038F;&#x0392;&#x0398;&#x03A6;&#x03A0;&#x03DA;&#x03DC;&#x03DE;&#x03E0;&#x03E2;&#x03E4;&#x03E6;&#x03E8;&#x03EA;&#x03EC;&#x03EE;&#x039A;&#x03A1;&#x03A3;&#x0395;&#x0410;&#x0411;&#x0412;&#x0413;&#x0414;&#x0415;&#x0416;&#x0417;&#x0418;&#x0419;&#x041A;&#x041B;&#x041C;&#x041D;&#x041E;&#x041F;&#x0420;&#x0421;&#x0422;&#x0423;&#x0424;&#x0425;&#x0426;&#x0427;&#x0428;&#x0429;&#x042A;&#x042B;&#x042C;&#x042D;&#x042E;&#x042F;&#x0400;&#x0401;&#x0402;&#x0403;&#x0404;&#x0405;&#x0406;&#x0407;&#x0408;&#x0409;&#x040A;&#x040B;&#x040C;&#x040D;&#x040E;&#x040F;&#x0460;&#x0462;&#x0464;&#x0466;&#x0468;&#x046A;&#x046C;&#x046E;&#x0470;&#x0472;&#x0474;&#x0476;&#x0478;&#x047A;&#x047C;&#x047E;&#x0480;&#x048C;&#x048E;&#x0490;&#x0492;&#x0494;&#x0496;&#x0498;&#x049A;&#x049C;&#x049E;&#x04A0;&#x04A2;&#x04A4;&#x04A6;&#x04A8;&#x04AA;&#x04AC;&#x04AE;&#x04B0;&#x04B2;&#x04B4;&#x04B6;&#x04B8;&#x04BA;&#x04BC;&#x04BE;&#x04C1;&#x04C3;&#x04C7;&#x04CB;&#x04D0;&#x04D2;&#x04D4;&#x04D6;&#x04D8;&#x04DA;&#x04DC;&#x04DE;&#x04E0;&#x04E2;&#x04E4;&#x04E6;&#x04E8;&#x04EA;&#x04EC;&#x04EE;&#x04F0;&#x04F2;&#x04F4;&#x04F8;&#x0531;&#x0532;&#x0533;&#x0534;&#x0535;&#x0536;&#x0537;&#x0538;&#x0539;&#x053A;&#x053B;&#x053C;&#x053D;&#x053E;&#x053F;&#x0540;&#x0541;&#x0542;&#x0543;&#x0544;&#x0545;&#x0546;&#x0547;&#x0548;&#x0549;&#x054A;&#x054B;&#x054C;&#x054D;&#x054E;&#x054F;&#x0550;&#x0551;&#x0552;&#x0553;&#x0554;&#x0555;&#x0556;&#x1E00;&#x1E02;&#x1E04;&#x1E06;&#x1E08;&#x1E0A;&#x1E0C;&#x1E0E;&#x1E10;&#x1E12;&#x1E14;&#x1E16;&#x1E18;&#x1E1A;&#x1E1C;&#x1E1E;&#x1E20;&#x1E22;&#x1E24;&#x1E26;&#x1E28;&#x1E2A;&#x1E2C;&#x1E2E;&#x1E30;&#x1E32;&#x1E34;&#x1E36;&#x1E38;&#x1E3A;&#x1E3C;&#x1E3E;&#x1E40;&#x1E42;&#x1E44;&#x1E46;&#x1E48;&#x1E4A;&#x1E4C;&#x1E4E;&#x1E50;&#x1E52;&#x1E54;&#x1E56;&#x1E58;&#x1E5A;&#x1E5C;&#x1E5E;&#x1E60;&#x1E62;&#x1E64;&#x1E66;&#x1E68;&#x1E6A;&#x1E6C;&#x1E6E;&#x1E70;&#x1E72;&#x1E74;&#x1E76;&#x1E78;&#x1E7A;&#x1E7C;&#x1E7E;&#x1E80;&#x1E82;&#x1E84;&#x1E86;&#x1E88;&#x1E8A;&#x1E8C;&#x1E8E;&#x1E90;&#x1E92;&#x1E94;&#x1E60;&#x1EA0;&#x1EA2;&#x1EA4;&#x1EA6;&#x1EA8;&#x1EAA;&#x1EAC;&#x1EAE;&#x1EB0;&#x1EB2;&#x1EB4;&#x1EB6;&#x1EB8;&#x1EBA;&#x1EBC;&#x1EBE;&#x1EC0;&#x1EC2;&#x1EC4;&#x1EC6;&#x1EC8;&#x1ECA;&#x1ECC;&#x1ECE;&#x1ED0;&#x1ED2;&#x1ED4;&#x1ED6;&#x1ED8;&#x1EDA;&#x1EDC;&#x1EDE;&#x1EE0;&#x1EE2;&#x1EE4;&#x1EE6;&#x1EE8;&#x1EEA;&#x1EEC;&#x1EEE;&#x1EF0;&#x1EF2;&#x1EF4;&#x1EF6;&#x1EF8;&#x1F08;&#x1F09;&#x1F0A;&#x1F0B;&#x1F0C;&#x1F0D;&#x1F0E;&#x1F0F;&#x1F18;&#x1F19;&#x1F1A;&#x1F1B;&#x1F1C;&#x1F1D;&#x1F28;&#x1F29;&#x1F2A;&#x1F2B;&#x1F2C;&#x1F2D;&#x1F2E;&#x1F2F;&#x1F38;&#x1F39;&#x1F3A;&#x1F3B;&#x1F3C;&#x1F3D;&#x1F3E;&#x1F3F;&#x1F48;&#x1F49;&#x1F4A;&#x1F4B;&#x1F4C;&#x1F4D;&#x1F59;&#x1F5B;&#x1F5D;&#x1F5F;&#x1F68;&#x1F69;&#x1F6A;&#x1F6B;&#x1F6C;&#x1F6D;&#x1F6E;&#x1F6F;&#x1FBA;&#x1FBB;&#x1FC8;&#x1FC9;&#x1FCA;&#x1FCB;&#x1FDA;&#x1FDB;&#x1FF8;&#x1FF9;&#x1FEA;&#x1FEB;&#x1FFA;&#x1FFB;&#x1F88;&#x1F89;&#x1F8A;&#x1F8B;&#x1F8C;&#x1F8D;&#x1F8E;&#x1F8F;&#x1F98;&#x1F99;&#x1F9A;&#x1F9B;&#x1F9C;&#x1F9D;&#x1F9E;&#x1F9F;&#x1FA8;&#x1FA9;&#x1FAA;&#x1FAB;&#x1FAC;&#x1FAD;&#x1FAE;&#x1FAF;&#x1FB8;&#x1FB9;&#x1FBC;&#x0399;&#x1FCC;&#x1FD8;&#x1FD9;&#x1FE8;&#x1FE9;&#x1FEC;&#x1FFC;&#x2160;&#x2161;&#x2162;&#x2163;&#x2164;&#x2165;&#x2166;&#x2167;&#x2168;&#x2169;&#x216A;&#x216B;&#x216C;&#x216D;&#x216E;&#x216F;&#x24B6;&#x24B7;&#x24B8;&#x24B9;&#x24BA;&#x24BB;&#x24BC;&#x24BD;&#x24BE;&#x24BF;&#x24C0;&#x24C1;&#x24C2;&#x24C3;&#x24C4;&#x24C5;&#x24C6;&#x24C7;&#x24C8;&#x24C9;&#x24CA;&#x24CB;&#x24CC;&#x24CD;&#x24CE;&#x24CF;&#xFF21;&#xFF22;&#xFF23;&#xFF24;&#xFF25;&#xFF26;&#xFF27;&#xFF28;&#xFF29;&#xFF2A;&#xFF2B;&#xFF2C;&#xFF2D;&#xFF2E;&#xFF2F;&#xFF30;&#xFF31;&#xFF32;&#xFF33;&#xFF34;&#xFF35;&#xFF36;&#xFF37;&#xFF38;&#xFF39;&#xFF3A;&#x10400;&#x10401;&#x10402;&#x10403;&#x10404;&#x10405;&#x10406;&#x10407;&#x10408;&#x10409;&#x1040A;&#x1040B;&#x1040C;&#x1040D;&#x1040E;&#x1040F;&#x10410;&#x10411;&#x10412;&#x10413;&#x10414;&#x10415;&#x10416;&#x10417;&#x10418;&#x10419;&#x1041A;&#x1041B;&#x1041C;&#x1041D;&#x1041E;&#x1041F;&#x10420;&#x10421;&#x10422;&#x10423;&#x10424;&#x10425;'"/>
  <xsl:variable name="xsltsl-str-digits" select="'0123456789'"/>
  <!-- space (#x20) characters, carriage returns, line feeds, or tabs. -->
  <xsl:variable name="xsltsl-str-ws" select="'&#x20;&#x9;&#xD;&#xA;'"/>

  <doc:template name="str:to-upper" xmlns="">
    <refpurpose>Make string uppercase</refpurpose>

    <refdescription>
      <para>Converts all lowercase letters to uppercase.</para>
    </refdescription>

    <refparameter>
      <variablelist>
	<varlistentry>
	  <term>text</term>
	  <listitem>
	    <para>The string to be converted</para>
	  </listitem>
	</varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns string with all uppercase letters.</para>
    </refreturn>
  </doc:template>

  <xsl:template name="str:to-upper">
    <xsl:param name="text"/>

    <!-- Below exception is extracted from unicode's SpecialCasing.txt
         file. It's the german lowercase "eszett" (the thing looking
         like a greek beta) that's to become "SS" in uppercase (note:
         that are *two* characters, that's why it doesn't fit in the
         list of upper/lowercase characters). There are more
         characters in that file (103, excluding the locale-specific
         ones), but they seemed to be much less used to me and they
         add up to a hellish long stylesheet.... - Reinout -->
    <xsl:param name="modified-text">
      <xsl:call-template name="str:subst">
        <xsl:with-param name="text">
          <xsl:value-of select="$text"/>
        </xsl:with-param>
        <xsl:with-param name="replace">
          <xsl:text>&#x00DF;</xsl:text>
        </xsl:with-param>
        <xsl:with-param name="with">
          <xsl:text>&#x0053;</xsl:text>
          <xsl:text>&#x0053;</xsl:text>
        </xsl:with-param>
      </xsl:call-template>
    </xsl:param>

    <xsl:value-of select="translate($modified-text, $xsltsl-str-lower, $xsltsl-str-upper)"/>
  </xsl:template>

  <doc:template name="str:to-lower" xmlns="">
    <refpurpose>Make string lowercase</refpurpose>

    <refdescription>
      <para>Converts all uppercase letters to lowercase.</para>
    </refdescription>

    <refparameter>
      <variablelist>
	<varlistentry>
	  <term>text</term>
	  <listitem>
	    <para>The string to be converted</para>
	  </listitem>
	</varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns string with all lowercase letters.</para>
    </refreturn>
  </doc:template>

  <xsl:template name="str:to-lower">
    <xsl:param name="text"/>

    <xsl:value-of select="translate($text, $xsltsl-str-upper, $xsltsl-str-lower)"/>
  </xsl:template>

  <doc:template name="str:capitalise" xmlns="">
    <refpurpose>Capitalise string</refpurpose>

    <refdescription>
      <para>Converts first character of string to an uppercase letter.  All remaining characters are converted to lowercase.</para>
    </refdescription>

    <refparameter>
      <variablelist>
	<varlistentry>
	  <term>text</term>
	  <listitem>
	    <para>The string to be capitalised</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>all</term>
	  <listitem>
	    <para>Boolean controlling whether all words in the string are capitalised.</para>
	    <para>Default is true.</para>
	  </listitem>
	</varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns string with first character uppcase and all remaining characters lowercase.</para>
    </refreturn>
  </doc:template>

  <xsl:template name="str:capitalise">
    <xsl:param name="text"/>
    <xsl:param name="all" select="true()"/>

    <xsl:choose>
      <xsl:when test="$all and (contains($text, ' ') or contains($text, '	') or contains($text, '&#10;'))">
	<xsl:variable name="firstword">
	  <xsl:call-template name="str:substring-before-first">
	    <xsl:with-param name="text" select="$text"/>
	    <xsl:with-param name="chars" select="$xsltsl-str-ws"/>
	  </xsl:call-template>
	</xsl:variable>
	<xsl:call-template name="str:capitalise">
	  <xsl:with-param name="text">
	    <xsl:value-of select="$firstword"/>
	  </xsl:with-param>
	  <xsl:with-param name="all" select="false()"/>
	</xsl:call-template>
	<xsl:value-of select="substring($text, string-length($firstword) + 1, 1)"/>
	<xsl:call-template name="str:capitalise">
	  <xsl:with-param name="text">
	    <xsl:value-of select="substring($text, string-length($firstword) + 2)"/>
	  </xsl:with-param>
	</xsl:call-template>
      </xsl:when>

      <xsl:otherwise>
	<xsl:call-template name="str:to-upper">
	  <xsl:with-param name="text" select="substring($text, 1, 1)"/>
	</xsl:call-template>
	<xsl:call-template name="str:to-lower">
	  <xsl:with-param name="text" select="substring($text, 2)"/>
	</xsl:call-template>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <doc:template name="str:to-camelcase" xmlns="">
    <refpurpose>Convert a string to one camelcase word</refpurpose>

    <refdescription>
      <para>Converts a string to one lowerCamelCase or UpperCamelCase
      word, depending on the setting of the "upper"
      parameter. UpperCamelCase is also called MixedCase while
      lowerCamelCase is also called just camelCase. The template
      removes any spaces, tabs and slashes, but doesn't deal with
      other punctuation. It's purpose is to convert strings like
      "hollow timber flush door" to a term suitable as identifier or
      XML tag like "HollowTimberFlushDoor".
      </para>
    </refdescription>

    <refparameter>
      <variablelist>
	<varlistentry>
	  <term>text</term>
	  <listitem>
	    <para>The string to be capitalised</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>upper</term>
	  <listitem>
	    <para>Boolean controlling whether the string becomes an
            UpperCamelCase word or a lowerCamelCase word.</para>
	    <para>Default is true.</para>
	  </listitem>
	</varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns string with first character uppcase and all remaining characters lowercase.</para>
    </refreturn>
  </doc:template>

  <xsl:template name="str:to-camelcase">
    <xsl:param name="text"/>
    <xsl:param name="upper" select="true()"/>
    <!-- First change all 'strange' characters to spaces -->
    <xsl:param name="string-with-only-spaces">
      <xsl:value-of select="translate($text,concat($xsltsl-str-ws,'/'),'     ')"/>
    </xsl:param>
    <!-- Then process them -->
    <xsl:param name="before-space-removal">
      <xsl:variable name="firstword">
        <xsl:call-template name="str:substring-before-first">
          <xsl:with-param name="text" select="$string-with-only-spaces"/>
          <xsl:with-param name="chars" select="$xsltsl-str-ws"/>
        </xsl:call-template>
      </xsl:variable>
      <xsl:choose>
        <xsl:when test="$upper">
          <xsl:call-template name="str:to-upper">
            <xsl:with-param name="text" select="substring($firstword, 1, 1)"/>
          </xsl:call-template>
          <xsl:call-template name="str:to-lower">
            <xsl:with-param name="text" select="substring($firstword, 2)"/>
          </xsl:call-template>
        </xsl:when>
        <xsl:otherwise>
          <xsl:call-template name="str:to-lower">
            <xsl:with-param name="text" select="$firstword"/>
          </xsl:call-template>
        </xsl:otherwise>
      </xsl:choose>

      <xsl:call-template name="str:capitalise">
        <xsl:with-param name="text">
          <xsl:value-of select="substring($string-with-only-spaces, string-length($firstword) + 2)"/>
        </xsl:with-param>
        <xsl:with-param name="all" select="true()"/>            
      </xsl:call-template>
    </xsl:param>
    <xsl:value-of select="translate($before-space-removal,' ','')"/>
  </xsl:template>

  <doc:template name="str:substring-before-first" xmlns="">
    <refpurpose>String extraction</refpurpose>

    <refdescription>
      <para>Extracts the portion of string 'text' which occurs before any of the characters in string 'chars'.</para>
    </refdescription>

    <refparameter>
      <variablelist>
	<varlistentry>
	  <term>text</term>
	  <listitem>
	    <para>The string from which to extract a substring.</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>chars</term>
	  <listitem>
	    <para>The string containing characters to find.</para>
	  </listitem>
	</varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns string.</para>
    </refreturn>
  </doc:template>

  <xsl:template name="str:substring-before-first">
    <xsl:param name="text"/>
    <xsl:param name="chars"/>

    <xsl:choose>

      <xsl:when test="string-length($text) = 0"/>

      <xsl:when test="string-length($chars) = 0">
	<xsl:value-of select="$text"/>
      </xsl:when>

      <xsl:when test="contains($text, substring($chars, 1, 1))">
	<xsl:variable name="this" select="substring-before($text, substring($chars, 1, 1))"/>
	<xsl:variable name="rest">
	  <xsl:call-template name="str:substring-before-first">
	    <xsl:with-param name="text" select="$text"/>
	    <xsl:with-param name="chars" select="substring($chars, 2)"/>
	  </xsl:call-template>
	</xsl:variable>

	<xsl:choose>
	  <xsl:when test="string-length($this) &lt; string-length($rest)">
	    <xsl:value-of select="$this"/>
	  </xsl:when>
	  <xsl:otherwise>
	    <xsl:value-of select="$rest"/>
	  </xsl:otherwise>
	</xsl:choose>
      </xsl:when>

      <xsl:otherwise>
	<xsl:call-template name="str:substring-before-first">
	  <xsl:with-param name="text" select="$text"/>
	  <xsl:with-param name="chars" select="substring($chars, 2)"/>
	</xsl:call-template>
      </xsl:otherwise>

    </xsl:choose>
  </xsl:template>

  <doc:template name="str:substring-after-last" xmlns="">
    <refpurpose>String extraction</refpurpose>

    <refdescription>
      <para>Extracts the portion of string 'text' which occurs after the last of the character in string 'chars'.</para>
    </refdescription>

    <refparameter>
      <variablelist>
	<varlistentry>
	  <term>text</term>
	  <listitem>
	    <para>The string from which to extract a substring.</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>chars</term>
	  <listitem>
	    <para>The string containing characters to find.</para>
	  </listitem>
	</varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns string.</para>
    </refreturn>
  </doc:template>

  <xsl:template name="str:substring-after-last">
    <xsl:param name="text"/>
    <xsl:param name="chars"/>

    <xsl:choose>

      <xsl:when test="contains($text, $chars)">
        <xsl:variable name="last" select="substring-after($text, $chars)"/>

	<xsl:choose>
	  <xsl:when test="contains($last, $chars)">
	    <xsl:call-template name="str:substring-after-last">
	      <xsl:with-param name="text" select="$last"/>
	      <xsl:with-param name="chars" select="$chars"/>
	    </xsl:call-template>
	  </xsl:when>
	  <xsl:otherwise>
	    <xsl:value-of select="$last"/>
	  </xsl:otherwise>
	</xsl:choose>
      </xsl:when>

      <xsl:otherwise>
        <xsl:value-of select="$text"/>
      </xsl:otherwise>

    </xsl:choose>
  </xsl:template>

  <doc:template name="str:substring-before-last" xmlns="">
    <refpurpose>String extraction</refpurpose>

    <refdescription>
      <para>Extracts the portion of string 'text' which occurs before the first character of the last occurance of string 'chars'.</para>
    </refdescription>

    <refparameter>
      <variablelist>
	<varlistentry>
	  <term>text</term>
	  <listitem>
	    <para>The string from which to extract a substring.</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>chars</term>
	  <listitem>
	    <para>The string containing characters to find.</para>
	  </listitem>
	</varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns string.</para>
    </refreturn>
  </doc:template>

  <xsl:template name="str:substring-before-last">
    <xsl:param name="text"/>
    <xsl:param name="chars"/>

    <xsl:choose>

      <xsl:when test="string-length($text) = 0"/>

      <xsl:when test="string-length($chars) = 0">
	<xsl:value-of select="$text"/>
      </xsl:when>

      <xsl:when test="contains($text, $chars)">
	<xsl:call-template name="str:substring-before-last-aux">
	  <xsl:with-param name="text" select="$text"/>
	  <xsl:with-param name="chars" select="$chars"/>
	</xsl:call-template>
      </xsl:when>

      <xsl:otherwise>
        <xsl:value-of select="$text"/>
      </xsl:otherwise>

    </xsl:choose>
  </xsl:template>

  <xsl:template name="str:substring-before-last-aux">
    <xsl:param name="text"/>
    <xsl:param name="chars"/>

    <xsl:choose>
      <xsl:when test="string-length($text) = 0"/>

      <xsl:when test="contains($text, $chars)">
	<xsl:variable name="after">
	  <xsl:call-template name="str:substring-before-last-aux">
	    <xsl:with-param name="text" select="substring-after($text, $chars)"/>
	    <xsl:with-param name="chars" select="$chars"/>
	  </xsl:call-template>
	</xsl:variable>

	<xsl:value-of select="substring-before($text, $chars)"/>
	<xsl:if test="string-length($after) &gt; 0">
	  <xsl:value-of select="$chars"/>
	  <xsl:copy-of select="$after"/>
	</xsl:if>
      </xsl:when>

      <xsl:otherwise/>
    </xsl:choose>
  </xsl:template>

  <doc:template name="str:subst" xmlns="">
    <refpurpose>String substitution</refpurpose>

    <refdescription>
      <para>Substitute 'replace' for 'with' in string 'text'.</para>
    </refdescription>

    <refparameter>
      <variablelist>
	<varlistentry>
	  <term>text</term>
	  <listitem>
	    <para>The string upon which to perform substitution.</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>replace</term>
	  <listitem>
	    <para>The string to substitute.</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>with</term>
	  <listitem>
	    <para>The string to be substituted.</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>disable-output-escaping</term>
	  <listitem>
            <para>A value of <literal>yes</literal> indicates that the result should have output escaping disabled.  Any other value allows normal escaping of text values.  The default is to enable output escaping.</para>
	  </listitem>
	</varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns string.</para>
    </refreturn>
  </doc:template>

  <xsl:template name="str:subst">
    <xsl:param name="text"/>
    <xsl:param name="replace"/>
    <xsl:param name="with"/>
    <xsl:param name='disable-output-escaping'>no</xsl:param>

    <xsl:choose>
      <xsl:when test="string-length($replace) = 0 and $disable-output-escaping = 'yes'">
        <xsl:value-of select="$text" disable-output-escaping='yes'/>
      </xsl:when>
      <xsl:when test="string-length($replace) = 0">
        <xsl:value-of select="$text"/>
      </xsl:when>
      <xsl:when test="contains($text, $replace)">

	<xsl:variable name="before" select="substring-before($text, $replace)"/>
	<xsl:variable name="after" select="substring-after($text, $replace)"/>

        <xsl:choose>
          <xsl:when test='$disable-output-escaping = "yes"'>
            <xsl:value-of select="$before" disable-output-escaping="yes"/>
            <xsl:value-of select="$with" disable-output-escaping="yes"/>
          </xsl:when>
          <xsl:otherwise>
            <xsl:value-of select="$before"/>
            <xsl:value-of select="$with"/>
          </xsl:otherwise>
        </xsl:choose>
        <xsl:call-template name="str:subst">
	  <xsl:with-param name="text" select="$after"/>
	  <xsl:with-param name="replace" select="$replace"/>
	  <xsl:with-param name="with" select="$with"/>
	  <xsl:with-param name="disable-output-escaping" select="$disable-output-escaping"/>
	</xsl:call-template>
      </xsl:when>
      <xsl:when test='$disable-output-escaping = "yes"'>
        <xsl:value-of select="$text" disable-output-escaping="yes"/>
      </xsl:when>
      <xsl:otherwise>
        <xsl:value-of select="$text"/>
      </xsl:otherwise>
    </xsl:choose>            
  </xsl:template>

  <doc:template name="str:count-substring" xmlns="">
    <refpurpose>Count Substrings</refpurpose>

    <refdescription>
      <para>Counts the number of times a substring occurs in a string.  This can also counts the number of times a character occurs in a string, since a character is simply a string of length 1.</para>
    </refdescription>

    <example>
      <title>Counting Lines</title>
      <programlisting><![CDATA[
<xsl:call-template name="str:count-substring">
  <xsl:with-param name="text" select="$mytext"/>
  <xsl:with-param name="chars" select="'&#x0a;'"/>
</xsl:call-template>
]]></programlisting>
    </example>

    <refparameter>
      <variablelist>
	<varlistentry>
	  <term>text</term>
	  <listitem>
	    <para>The source string.</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>chars</term>
	  <listitem>
	    <para>The substring to count.</para>
	  </listitem>
	</varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns a non-negative integer value.</para>
    </refreturn>
  </doc:template>

  <xsl:template name="str:count-substring">
    <xsl:param name="text"/>
    <xsl:param name="chars"/>

    <xsl:choose>
      <xsl:when test="string-length($text) = 0 or string-length($chars) = 0">
	<xsl:text>0</xsl:text>
      </xsl:when>
      <xsl:when test="contains($text, $chars)">
	<xsl:variable name="remaining">
	  <xsl:call-template name="str:count-substring">
	    <xsl:with-param name="text" select="substring-after($text, $chars)"/>
	    <xsl:with-param name="chars" select="$chars"/>
	  </xsl:call-template>
	</xsl:variable>
	<xsl:value-of select="$remaining + 1"/>
      </xsl:when>
      <xsl:otherwise>
	<xsl:text>0</xsl:text>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <doc:template name="str:substring-after-at" xmlns="">
    <refpurpose>String extraction</refpurpose>
    <refdescription>
      <para>Extracts the portion of a 'char' delimited 'text' string "array" at a given 'position'.</para>
    </refdescription>
    <refparameter>
      <variablelist>
        <varlistentry>
          <term>text</term>
          <listitem>
            <para>The string from which to extract a substring.</para>
          </listitem>
        </varlistentry>
        <varlistentry>
          <term>chars</term>
          <listitem>
            <para>delimiters</para>
          </listitem>
        </varlistentry>
        <varlistentry>
          <term>position</term>
          <listitem>
            <para>position of the elements</para>
          </listitem>
        </varlistentry>
        <varlistentry>
          <term>all</term>
          <listitem>
            <para>If true all of the remaining string is returned, otherwise only the element at the given position is returned.  Default: false().</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>
    <refreturn>
      <para>Returns string.</para>
    </refreturn>
  </doc:template>


  <xsl:template name="str:substring-after-at">
    <xsl:param name="text"/>
    <xsl:param name="chars"/>
    <xsl:param name="position"/>
    <xsl:param name="all" select='false()'/>

    <xsl:choose>
      <xsl:when test='number($position) = 0 and $all'>
        <xsl:value-of select='$text'/>
      </xsl:when>
      <xsl:when test='number($position) = 0 and not($chars)'>
        <xsl:value-of select='$text'/>
      </xsl:when>
      <xsl:when test='number($position) = 0 and not(contains($text, $chars))'>
        <xsl:value-of select='$text'/>
      </xsl:when>
      <xsl:when test='not(contains($text, $chars))'>
      </xsl:when>
      <xsl:when test="number($position) = 0">
        <xsl:value-of select="substring-before($text, $chars)"/>
      </xsl:when>
      <xsl:otherwise>
        <xsl:call-template name="str:substring-after-at">
          <xsl:with-param name="text" select="substring-after($text, $chars)"/>
          <xsl:with-param name="chars" select="$chars"/>
          <xsl:with-param name="all" select="$all"/>
          <xsl:with-param name="position" select="$position - 1"/>
        </xsl:call-template>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <doc:template name="str:substring-before-at" xmlns="">
    <refpurpose>String extraction</refpurpose>
    <refdescription>
      <para>Extracts the portion of a 'char' delimited 'text' string "array" at a given 'position' </para>
    </refdescription>
    <refparameter>
      <variablelist>
        <varlistentry>
          <term>text</term>
          <listitem>
            <para>The string from which to extract a substring.</para>
          </listitem>
        </varlistentry>
        <varlistentry>
          <term>chars</term>
          <listitem>
          <para>delimiters</para>
          </listitem>
        </varlistentry>
        <varlistentry>
          <term>position</term>
          <listitem>
            <para>position of the elements</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>
    <refreturn>
      <para>Returns string.</para>
    </refreturn>
  </doc:template>

  <xsl:template name="str:substring-before-at">
    <xsl:param name="text"/>
    <xsl:param name="chars"/>
    <xsl:param name="position"/>

    <xsl:choose>
      <xsl:when test="$position &lt;= 0"/>
      <xsl:when test="not(contains($text, $chars))"/>
      <xsl:otherwise>
        <xsl:value-of select='substring-before($text, $chars)'/>
        <xsl:value-of select='$chars'/>

        <xsl:call-template name="str:substring-before-at">
          <xsl:with-param name="text" select="substring-after($text, $chars)"/>
          <xsl:with-param name="position" select="$position - 1"/>
          <xsl:with-param name="chars" select="$chars"/>
        </xsl:call-template>
        
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <doc:template name="str:insert-at" xmlns="">
    <refpurpose>String insertion</refpurpose>
    <refdescription>
      <para>Insert 'chars' into "text' at any given "position'</para>
    </refdescription>
    <refparameter>
      <variablelist>
        <varlistentry>
          <term>text</term>
          <listitem>
            <para>The string upon which to perform insertion</para>
          </listitem>
        </varlistentry>
        <varlistentry>
          <term>position</term>
          <listitem>
            <para>the position where insertion will be performed</para>
          </listitem>
        </varlistentry>
        <varlistentry>
          <term>with</term>
          <listitem>
            <para>The string to be inserted</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>
    <refreturn>
      <para>Returns string.</para>
    </refreturn>
  </doc:template>

  <xsl:template name="str:insert-at">
    <xsl:param name="text"/>
    <xsl:param name="position"/>
    <xsl:param name="chars"/>

    <xsl:variable name="firstpart" select="substring($text, 0, $position)"/>
    <xsl:variable name="secondpart" select="substring($text, $position, string-length($text))"/>

    <xsl:value-of select="concat($firstpart, $chars, $secondpart)"/>
  </xsl:template>
 

  <doc:template name="str:backward" xmlns="">
    <refpurpose>String reversal</refpurpose>

    <refdescription>
      <para>Reverse the content of a given string</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>text</term>
          <listitem>
            <para>The string to be reversed</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns string.</para>
    </refreturn>
  </doc:template>

  <xsl:template name="str:backward">
    <xsl:param name="text"/>
    <xsl:variable name="mirror">
      <xsl:call-template name="str:build-mirror">
        <xsl:with-param name="text" select="$text"/>
        <xsl:with-param name="position" select="string-length($text)"/>
      </xsl:call-template>
    </xsl:variable>
    <xsl:value-of select="substring($mirror, string-length($text) + 1, string-length($text))"/>
  </xsl:template>

  <xsl:template name="str:build-mirror">
    <xsl:param name="text"/>
    <xsl:param name="position"/>

    <xsl:choose>
      <xsl:when test="$position &gt; 0">
        <xsl:call-template name="str:build-mirror">
          <xsl:with-param name="text" select="concat($text, substring($text, $position, 1))"/>
          <xsl:with-param name="position" select="$position - 1"/>
        </xsl:call-template>
      </xsl:when>
      <xsl:otherwise>
        <xsl:value-of select="$text"/>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <doc:template name="str:justify" xmlns="">
    <refpurpose>Format a string</refpurpose>

    <refdescription>
      <para>Inserts newlines and spaces into a string to format it as a block of text.</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>text</term>
          <listitem>
            <para>String to be formatted.</para>
          </listitem>
        </varlistentry>
        <varlistentry>
          <term>max</term>
          <listitem>
            <para>Maximum line length.</para>
          </listitem>
        </varlistentry>
        <varlistentry>
          <term>indent</term>
          <listitem>
            <para>Number of spaces to insert at the beginning of each line.</para>
          </listitem>
        </varlistentry>
        <varlistentry>
          <term>justify</term>
          <listitem>
            <para>Justify left, right or both.  Not currently implemented (fixed at "left").</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Formatted block of text.</para>
    </refreturn>
  </doc:template>

  <xsl:template name='str:justify'>
    <xsl:param name='text'/>
    <xsl:param name='max' select='"80"'/>
    <xsl:param name='indent' select='"0"'/>
    <xsl:param name='justify' select='"left"'/>

    <xsl:choose>
      <xsl:when test='string-length($text) = 0 or $max &lt;= 0'/>

      <xsl:when test='string-length($text) > $max and contains($text, " ") and string-length(substring-before($text, " ")) > $max'>
        <xsl:call-template name='str:generate-string'>
          <xsl:with-param name='text' select='" "'/>
          <xsl:with-param name='count' select='$indent'/>
        </xsl:call-template>
        <xsl:value-of select='substring-before($text, " ")'/>
        <xsl:text>
</xsl:text>
        <xsl:call-template name='str:justify'>
          <xsl:with-param name='text' select='substring-after($text, " ")'/>
          <xsl:with-param name='max' select='$max'/>
          <xsl:with-param name='indent' select='$indent'/>
          <xsl:with-param name='justify' select='$justify'/>
        </xsl:call-template>
      </xsl:when>

      <xsl:when test='string-length($text) > $max and contains($text, " ")'>
        <xsl:variable name='first'>
          <xsl:call-template name='str:substring-before-last'>
            <xsl:with-param name='text' select='substring($text, 1, $max)'/>
            <xsl:with-param name='chars' select='" "'/>
          </xsl:call-template>
        </xsl:variable>

        <xsl:call-template name='str:generate-string'>
          <xsl:with-param name='text' select='" "'/>
          <xsl:with-param name='count' select='$indent'/>
        </xsl:call-template>
        <xsl:value-of select='$first'/>
        <xsl:text>
</xsl:text>
        <xsl:call-template name='str:justify'>
          <xsl:with-param name='text' select='substring($text, string-length($first) + 2)'/>
          <xsl:with-param name='max' select='$max'/>
          <xsl:with-param name='indent' select='$indent'/>
          <xsl:with-param name='justify' select='$justify'/>
        </xsl:call-template>
      </xsl:when>

      <xsl:otherwise>
        <xsl:call-template name='str:generate-string'>
          <xsl:with-param name='text' select='" "'/>
          <xsl:with-param name='count' select='$indent'/>
        </xsl:call-template>
        <xsl:value-of select='$text'/>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <doc:template name="str:character-first" xmlns="">
    <refpurpose>Find first occurring character in a string</refpurpose>

    <refdescription>
      <para>Finds which of the given characters occurs first in a string.</para>
    </refdescription>

    <refparameter>
      <variablelist>
	<varlistentry>
	  <term>text</term>
	  <listitem>
	    <para>The source string.</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>chars</term>
	  <listitem>
	    <para>The characters to search for.</para>
	  </listitem>
	</varlistentry>
      </variablelist>
    </refparameter>
  </doc:template>

  <xsl:template name="str:character-first">
    <xsl:param name="text"/>
    <xsl:param name="chars"/>

    <xsl:choose>
      <xsl:when test="string-length($text) = 0 or string-length($chars) = 0"/>

      <xsl:when test="contains($text, substring($chars, 1, 1))">
	<xsl:variable name="next-character">
	  <xsl:call-template name="str:character-first">
	    <xsl:with-param name="text" select="$text"/>
	    <xsl:with-param name="chars" select="substring($chars, 2)"/>
	  </xsl:call-template>
	</xsl:variable>

	<xsl:choose>
	  <xsl:when test="string-length($next-character)">
	    <xsl:variable name="first-character-position" select="string-length(substring-before($text, substring($chars, 1, 1)))"/>
	    <xsl:variable name="next-character-position" select="string-length(substring-before($text, $next-character))"/>

	    <xsl:choose>
	      <xsl:when test="$first-character-position &lt; $next-character-position">
		<xsl:value-of select="substring($chars, 1, 1)"/>
	      </xsl:when>
	      <xsl:otherwise>
		<xsl:value-of select="$next-character"/>
	      </xsl:otherwise>
	    </xsl:choose>
	  </xsl:when>
	  <xsl:otherwise>
	    <xsl:value-of select="substring($chars, 1, 1)"/>
	  </xsl:otherwise>
	</xsl:choose>
      </xsl:when>
      <xsl:otherwise>
	<xsl:call-template name="str:character-first">
	  <xsl:with-param name="text" select="$text"/>
	  <xsl:with-param name="chars" select="substring($chars, 2)"/>
	</xsl:call-template>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <doc:template name="str:string-match" xmlns="">
    <refpurpose>Match A String To A Pattern</refpurpose>

    <refdescription>
      <para>Performs globbing-style pattern matching on a string.</para>
    </refdescription>

    <example>
      <title>Match Pattern</title>
      <programlisting><![CDATA[
<xsl:call-template name="str:string-match">
  <xsl:with-param name="text" select="$mytext"/>
  <xsl:with-param name="pattern" select="'abc*def?g'"/>
</xsl:call-template>
]]></programlisting>
    </example>

    <refparameter>
      <variablelist>
	<varlistentry>
	  <term>text</term>
	  <listitem>
	    <para>The source string.</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>pattern</term>
	  <listitem>
	    <para>The pattern to match against.  Certain characters have special meaning:</para>
	    <variablelist>
	      <varlistentry>
		<term>*</term>
		<listitem>
		  <para>Matches zero or more characters.</para>
		</listitem>
	      </varlistentry>
	      <varlistentry>
		<term>?</term>
		<listitem>
		  <para>Matches a single character.</para>
		</listitem>
	      </varlistentry>
	      <varlistentry>
		<term>\</term>
		<listitem>
		  <para>Character escape.  The next character is taken as a literal character.</para>
		</listitem>
	      </varlistentry>
	    </variablelist>
	  </listitem>
	</varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns "1" if the string matches the pattern, "0" otherwise.</para>
    </refreturn>
  </doc:template>

  <xsl:template name="str:string-match">
    <xsl:param name="text"/>
    <xsl:param name="pattern"/>

    <xsl:choose>
      <xsl:when test="$pattern = '*'">
	<!-- Special case: always matches -->
	<xsl:text>1</xsl:text>
      </xsl:when>
      <xsl:when test="string-length($text) = 0 and string-length($pattern) = 0">
	<xsl:text>1</xsl:text>
      </xsl:when>
      <xsl:when test="string-length($text) = 0 or string-length($pattern) = 0">
	<xsl:text>0</xsl:text>
      </xsl:when>
      <xsl:otherwise>
	<xsl:variable name='before-special'>
	  <xsl:call-template name='str:substring-before-first'>
	    <xsl:with-param name='text' select='$pattern'/>
	    <xsl:with-param name='chars' select='"*?\"'/>
	  </xsl:call-template>
	</xsl:variable>
	<xsl:variable name='special'>
	  <xsl:call-template name='str:character-first'>
	    <xsl:with-param name='text' select='$pattern'/>
	    <xsl:with-param name='chars' select='"*?\"'/>
	  </xsl:call-template>
	</xsl:variable>

	<xsl:variable name='new-text' select='substring($text, string-length($before-special) + 1)'/>
	<xsl:variable name='new-pattern' select='substring($pattern, string-length($before-special) + 1)'/>

	<xsl:choose>
	  <xsl:when test="not(starts-with($text, $before-special))">
	    <!-- Verbatim characters don't match -->
	    <xsl:text>0</xsl:text>
	  </xsl:when>

	  <xsl:when test="$special = '*' and string-length($new-pattern) = 1">
	    <xsl:text>1</xsl:text>
	  </xsl:when>
	  <xsl:when test="$special = '*'">
	    <xsl:call-template name='str:match-postfix'>
	      <xsl:with-param name='text' select='$new-text'/>
	      <xsl:with-param name='pattern' select='substring($new-pattern, 2)'/>
	    </xsl:call-template>
	  </xsl:when>

	  <xsl:when test="$special = '?'">
	    <xsl:call-template name="str:string-match">
	      <xsl:with-param name='text' select='substring($new-text, 2)'/>
	      <xsl:with-param name='pattern' select='substring($new-pattern, 2)'/>
	    </xsl:call-template>
	  </xsl:when>

	  <xsl:when test="$special = '\' and substring($new-text, 1, 1) = substring($new-pattern, 2, 1)">
	    <xsl:call-template name="str:string-match">
	      <xsl:with-param name='text' select='substring($new-text, 2)'/>
	      <xsl:with-param name='pattern' select='substring($new-pattern, 3)'/>
	    </xsl:call-template>
	  </xsl:when>
	  <xsl:when test="$special = '\' and substring($new-text, 1, 1) != substring($new-pattern, 2, 1)">
	    <xsl:text>0</xsl:text>
	  </xsl:when>

	  <xsl:otherwise>
	    <!-- There were no special characters in the pattern -->
	    <xsl:choose>
	      <xsl:when test='$text = $pattern'>
		<xsl:text>1</xsl:text>
	      </xsl:when>
	      <xsl:otherwise>
		<xsl:text>0</xsl:text>
	      </xsl:otherwise>
	    </xsl:choose>
	  </xsl:otherwise>
	</xsl:choose>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <xsl:template name="str:match-postfix">
    <xsl:param name="text"/>
    <xsl:param name="pattern"/>

    <xsl:variable name='result'>
      <xsl:call-template name='str:string-match'>
	<xsl:with-param name='text' select='$text'/>
	<xsl:with-param name='pattern' select='$pattern'/>
      </xsl:call-template>
    </xsl:variable>

    <xsl:choose>
      <xsl:when test='$result = "1"'>
	<xsl:value-of select='$result'/>
      </xsl:when>
      <xsl:when test='string-length($text) = 0'>
	<xsl:text>0</xsl:text>
      </xsl:when>
      <xsl:otherwise>
	<xsl:call-template name='str:match-postfix'>
	  <xsl:with-param name='text' select='substring($text, 2)'/>
	  <xsl:with-param name='pattern' select='$pattern'/>
	</xsl:call-template>
      </xsl:otherwise>
    </xsl:choose>

  </xsl:template>

  <doc:template name="str:generate-string" xmlns="">
    <refpurpose>Create A Repeating Sequence of Characters</refpurpose>

    <refdescription>
      <para>Repeats a string a given number of times.</para>
    </refdescription>

    <refparameter>
      <variablelist>
	<varlistentry>
	  <term>text</term>
	  <listitem>
	    <para>The string to repeat.</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>count</term>
	  <listitem>
	    <para>The number of times to repeat the string.</para>
	  </listitem>
	</varlistentry>
      </variablelist>
    </refparameter>
  </doc:template>

  <xsl:template name="str:generate-string">
    <xsl:param name="text"/>
    <xsl:param name="count"/>

    <xsl:choose>
      <xsl:when test="string-length($text) = 0 or $count &lt;= 0"/>
      <xsl:otherwise>
	<xsl:value-of select="$text"/>
	<xsl:call-template name="str:generate-string">
	  <xsl:with-param name="text" select="$text"/>
	  <xsl:with-param name="count" select="$count - 1"/>
	</xsl:call-template>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

</xsl:stylesheet>

