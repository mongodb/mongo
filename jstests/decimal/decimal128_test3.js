/**
 * Derived from test cases at https://github.com/mongodb/specifications
 */

(function() {
    var data = [
        {
          "description": "[basx066] strings without E cannot generate E in result",
          "input": "-00345678.5432",
          "expected": "-345678.5432"
        },
        {
          "description": "[basx065] strings without E cannot generate E in result",
          "input": "-0345678.5432",
          "expected": "-345678.5432"
        },
        {
          "description": "[basx064] strings without E cannot generate E in result",
          "input": "-345678.5432"
        },
        {"description": "[basx041] strings without E cannot generate E in result", "input": "-76"},
        {
          "description": "[basx027] conform to rules and exponent will be in permitted range).",
          "input": "-9.999"
        },
        {
          "description": "[basx026] conform to rules and exponent will be in permitted range).",
          "input": "-9.119"
        },
        {
          "description": "[basx025] conform to rules and exponent will be in permitted range).",
          "input": "-9.11"
        },
        {
          "description": "[basx024] conform to rules and exponent will be in permitted range).",
          "input": "-9.1"
        },
        {
          "description": "[dqbsr531] negatives (Rounded)",
          "input": "-1.1111111111111111111111111111123450",
          "expected": "-1.111111111111111111111111111112345"
        },
        {
          "description": "[basx022] conform to rules and exponent will be in permitted range).",
          "input": "-1.0"
        },
        {
          "description": "[basx021] conform to rules and exponent will be in permitted range).",
          "input": "-1"
        },
        {"description": "[basx601] Zeros", "input": "0.000000000", "expected": "0E-9"},
        {"description": "[basx622] Zeros", "input": "-0.000000000", "expected": "-0E-9"},
        {"description": "[basx602] Zeros", "input": "0.00000000", "expected": "0E-8"},
        {"description": "[basx621] Zeros", "input": "-0.00000000", "expected": "-0E-8"},
        {"description": "[basx603] Zeros", "input": "0.0000000", "expected": "0E-7"},
        {"description": "[basx620] Zeros", "input": "-0.0000000", "expected": "-0E-7"},
        {"description": "[basx604] Zeros", "input": "0.000000"},
        {"description": "[basx619] Zeros", "input": "-0.000000"},
        {"description": "[basx605] Zeros", "input": "0.00000"},
        {"description": "[basx618] Zeros", "input": "-0.00000"},
        {"description": "[basx680] Zeros", "input": "000000.", "expected": "0"},
        {"description": "[basx606] Zeros", "input": "0.0000"},
        {"description": "[basx617] Zeros", "input": "-0.0000"},
        {"description": "[basx681] Zeros", "input": "00000.", "expected": "0"},
        {"description": "[basx686] Zeros", "input": "+00000.", "expected": "0"},
        {"description": "[basx687] Zeros", "input": "-00000.", "expected": "-0"},
        {
          "description": "[basx019] conform to rules and exponent will be in permitted range).",
          "input": "-00.00",
          "expected": "-0.00"
        },
        {"description": "[basx607] Zeros", "input": "0.000"},
        {"description": "[basx616] Zeros", "input": "-0.000"},
        {"description": "[basx682] Zeros", "input": "0000.", "expected": "0"},
        {"description": "[basx155] Numbers with E", "input": "0.000e+0", "expected": "0.000"},
        {"description": "[basx130] Numbers with E", "input": "0.000E-1", "expected": "0.0000"},
        {
          "description": "[basx290] some more negative zeros [systematic tests below]",
          "input": "-0.000E-1",
          "expected": "-0.0000"
        },
        {"description": "[basx131] Numbers with E", "input": "0.000E-2", "expected": "0.00000"},
        {
          "description": "[basx291] some more negative zeros [systematic tests below]",
          "input": "-0.000E-2",
          "expected": "-0.00000"
        },
        {"description": "[basx132] Numbers with E", "input": "0.000E-3", "expected": "0.000000"},
        {
          "description": "[basx292] some more negative zeros [systematic tests below]",
          "input": "-0.000E-3",
          "expected": "-0.000000"
        },
        {"description": "[basx133] Numbers with E", "input": "0.000E-4", "expected": "0E-7"},
        {
          "description": "[basx293] some more negative zeros [systematic tests below]",
          "input": "-0.000E-4",
          "expected": "-0E-7"
        },
        {"description": "[basx608] Zeros", "input": "0.00"},
        {"description": "[basx615] Zeros", "input": "-0.00"},
        {"description": "[basx683] Zeros", "input": "000.", "expected": "0"},
        {"description": "[basx630] Zeros", "input": "0.00E+0", "expected": "0.00"},
        {"description": "[basx670] Zeros", "input": "0.00E-0", "expected": "0.00"},
        {"description": "[basx631] Zeros", "input": "0.00E+1", "expected": "0.0"},
        {"description": "[basx671] Zeros", "input": "0.00E-1", "expected": "0.000"},
        {"description": "[basx134] Numbers with E", "input": "0.00E-2", "expected": "0.0000"},
        {
          "description": "[basx294] some more negative zeros [systematic tests below]",
          "input": "-0.00E-2",
          "expected": "-0.0000"
        },
        {"description": "[basx632] Zeros", "input": "0.00E+2", "expected": "0"},
        {"description": "[basx672] Zeros", "input": "0.00E-2", "expected": "0.0000"},
        {"description": "[basx135] Numbers with E", "input": "0.00E-3", "expected": "0.00000"},
        {
          "description": "[basx295] some more negative zeros [systematic tests below]",
          "input": "-0.00E-3",
          "expected": "-0.00000"
        },
        {"description": "[basx633] Zeros", "input": "0.00E+3", "expected": "0E+1"},
        {"description": "[basx673] Zeros", "input": "0.00E-3", "expected": "0.00000"},
        {"description": "[basx136] Numbers with E", "input": "0.00E-4", "expected": "0.000000"},
        {"description": "[basx674] Zeros", "input": "0.00E-4", "expected": "0.000000"},
        {"description": "[basx634] Zeros", "input": "0.00E+4", "expected": "0E+2"},
        {"description": "[basx137] Numbers with E", "input": "0.00E-5", "expected": "0E-7"},
        {"description": "[basx635] Zeros", "input": "0.00E+5", "expected": "0E+3"},
        {"description": "[basx675] Zeros", "input": "0.00E-5", "expected": "0E-7"},
        {"description": "[basx636] Zeros", "input": "0.00E+6", "expected": "0E+4"},
        {"description": "[basx676] Zeros", "input": "0.00E-6", "expected": "0E-8"},
        {"description": "[basx637] Zeros", "input": "0.00E+7", "expected": "0E+5"},
        {"description": "[basx677] Zeros", "input": "0.00E-7", "expected": "0E-9"},
        {"description": "[basx638] Zeros", "input": "0.00E+8", "expected": "0E+6"},
        {"description": "[basx678] Zeros", "input": "0.00E-8", "expected": "0E-10"},
        {"description": "[basx149] Numbers with E", "input": "000E+9", "expected": "0E+9"},
        {"description": "[basx639] Zeros", "input": "0.00E+9", "expected": "0E+7"},
        {"description": "[basx679] Zeros", "input": "0.00E-9", "expected": "0E-11"},
        {
          "description": "[basx063] strings without E cannot generate E in result",
          "input": "+00345678.5432",
          "expected": "345678.5432"
        },
        {
          "description": "[basx018] conform to rules and exponent will be in permitted range).",
          "input": "-0.0"
        },
        {"description": "[basx609] Zeros", "input": "0.0"},
        {"description": "[basx614] Zeros", "input": "-0.0"},
        {"description": "[basx684] Zeros", "input": "00.", "expected": "0"},
        {"description": "[basx640] Zeros", "input": "0.0E+0", "expected": "0.0"},
        {"description": "[basx660] Zeros", "input": "0.0E-0", "expected": "0.0"},
        {"description": "[basx641] Zeros", "input": "0.0E+1", "expected": "0"},
        {"description": "[basx661] Zeros", "input": "0.0E-1", "expected": "0.00"},
        {
          "description": "[basx296] some more negative zeros [systematic tests below]",
          "input": "-0.0E-2",
          "expected": "-0.000"
        },
        {"description": "[basx642] Zeros", "input": "0.0E+2", "expected": "0E+1"},
        {"description": "[basx662] Zeros", "input": "0.0E-2", "expected": "0.000"},
        {
          "description": "[basx297] some more negative zeros [systematic tests below]",
          "input": "-0.0E-3",
          "expected": "-0.0000"
        },
        {"description": "[basx643] Zeros", "input": "0.0E+3", "expected": "0E+2"},
        {"description": "[basx663] Zeros", "input": "0.0E-3", "expected": "0.0000"},
        {"description": "[basx644] Zeros", "input": "0.0E+4", "expected": "0E+3"},
        {"description": "[basx664] Zeros", "input": "0.0E-4", "expected": "0.00000"},
        {"description": "[basx645] Zeros", "input": "0.0E+5", "expected": "0E+4"},
        {"description": "[basx665] Zeros", "input": "0.0E-5", "expected": "0.000000"},
        {"description": "[basx646] Zeros", "input": "0.0E+6", "expected": "0E+5"},
        {"description": "[basx666] Zeros", "input": "0.0E-6", "expected": "0E-7"},
        {"description": "[basx647] Zeros", "input": "0.0E+7", "expected": "0E+6"},
        {"description": "[basx667] Zeros", "input": "0.0E-7", "expected": "0E-8"},
        {"description": "[basx648] Zeros", "input": "0.0E+8", "expected": "0E+7"},
        {"description": "[basx668] Zeros", "input": "0.0E-8", "expected": "0E-9"},
        {"description": "[basx160] Numbers with E", "input": "00E+9", "expected": "0E+9"},
        {"description": "[basx161] Numbers with E", "input": "00E-9", "expected": "0E-9"},
        {"description": "[basx649] Zeros", "input": "0.0E+9", "expected": "0E+8"},
        {"description": "[basx669] Zeros", "input": "0.0E-9", "expected": "0E-10"},
        {
          "description": "[basx062] strings without E cannot generate E in result",
          "input": "+0345678.5432",
          "expected": "345678.5432"
        },
        {
          "description": "[basx001] conform to rules and exponent will be in permitted range).",
          "input": "0"
        },
        {
          "description": "[basx017] conform to rules and exponent will be in permitted range).",
          "input": "-0"
        },
        {"description": "[basx611] Zeros", "input": "0.", "expected": "0"},
        {"description": "[basx613] Zeros", "input": "-0.", "expected": "-0"},
        {"description": "[basx685] Zeros", "input": "0.", "expected": "0"},
        {"description": "[basx688] Zeros", "input": "+0.", "expected": "0"},
        {"description": "[basx689] Zeros", "input": "-0.", "expected": "-0"},
        {"description": "[basx650] Zeros", "input": "0E+0", "expected": "0"},
        {"description": "[basx651] Zeros", "input": "0E+1"},
        {
          "description": "[basx298] some more negative zeros [systematic tests below]",
          "input": "-0E-2",
          "expected": "-0.00"
        },
        {"description": "[basx652] Zeros", "input": "0E+2"},
        {
          "description": "[basx299] some more negative zeros [systematic tests below]",
          "input": "-0E-3",
          "expected": "-0.000"
        },
        {"description": "[basx653] Zeros", "input": "0E+3"},
        {"description": "[basx654] Zeros", "input": "0E+4"},
        {"description": "[basx655] Zeros", "input": "0E+5"},
        {"description": "[basx656] Zeros", "input": "0E+6"},
        {"description": "[basx657] Zeros", "input": "0E+7"},
        {"description": "[basx658] Zeros", "input": "0E+8"},
        {"description": "[basx138] Numbers with E", "input": "+0E+9", "expected": "0E+9"},
        {"description": "[basx139] Numbers with E", "input": "-0E+9"},
        {"description": "[basx144] Numbers with E", "input": "0E+9"},
        {"description": "[basx154] Numbers with E", "input": "0E9", "expected": "0E+9"},
        {"description": "[basx659] Zeros", "input": "0E+9"},
        {
          "description": "[basx042] strings without E cannot generate E in result",
          "input": "+12.76",
          "expected": "12.76"
        },
        {"description": "[basx143] Numbers with E", "input": "+1E+009", "expected": "1E+9"},
        {
          "description": "[basx061] strings without E cannot generate E in result",
          "input": "+345678.5432",
          "expected": "345678.5432"
        },
        {
          "description": "[basx036] conform to rules and exponent will be in permitted range).",
          "input": "0.0000000123456789",
          "expected": "1.23456789E-8"
        },
        {
          "description": "[basx035] conform to rules and exponent will be in permitted range).",
          "input": "0.000000123456789",
          "expected": "1.23456789E-7"
        },
        {
          "description": "[basx034] conform to rules and exponent will be in permitted range).",
          "input": "0.00000123456789"
        },
        {
          "description": "[basx053] strings without E cannot generate E in result",
          "input": "0.0000050"
        },
        {
          "description": "[basx033] conform to rules and exponent will be in permitted range).",
          "input": "0.0000123456789"
        },
        {
          "description": "[basx016] conform to rules and exponent will be in permitted range).",
          "input": "0.012"
        },
        {
          "description": "[basx015] conform to rules and exponent will be in permitted range).",
          "input": "0.123"
        },
        {
          "description": "[basx037] conform to rules and exponent will be in permitted range).",
          "input": "0.123456789012344"
        },
        {
          "description": "[basx038] conform to rules and exponent will be in permitted range).",
          "input": "0.123456789012345"
        },
        {"description": "[basx250] Numbers with E", "input": "0.1265"},
        {"description": "[basx257] Numbers with E", "input": "0.1265E-0", "expected": "0.1265"},
        {"description": "[basx256] Numbers with E", "input": "0.1265E-1", "expected": "0.01265"},
        {"description": "[basx258] Numbers with E", "input": "0.1265E+1", "expected": "1.265"},
        {"description": "[basx251] Numbers with E", "input": "0.1265E-20", "expected": "1.265E-21"},
        {"description": "[basx263] Numbers with E", "input": "0.1265E+20", "expected": "1.265E+19"},
        {"description": "[basx255] Numbers with E", "input": "0.1265E-2", "expected": "0.001265"},
        {"description": "[basx259] Numbers with E", "input": "0.1265E+2", "expected": "12.65"},
        {"description": "[basx254] Numbers with E", "input": "0.1265E-3", "expected": "0.0001265"},
        {"description": "[basx260] Numbers with E", "input": "0.1265E+3", "expected": "126.5"},
        {"description": "[basx253] Numbers with E", "input": "0.1265E-4", "expected": "0.00001265"},
        {"description": "[basx261] Numbers with E", "input": "0.1265E+4", "expected": "1265"},
        {"description": "[basx252] Numbers with E", "input": "0.1265E-8", "expected": "1.265E-9"},
        {"description": "[basx262] Numbers with E", "input": "0.1265E+8", "expected": "1.265E+7"},
        {"description": "[basx159] Numbers with E", "input": "0.73e-7", "expected": "7.3E-8"},
        {
          "description": "[basx004] conform to rules and exponent will be in permitted range).",
          "input": "1.00"
        },
        {
          "description": "[basx003] conform to rules and exponent will be in permitted range).",
          "input": "1.0"
        },
        {
          "description": "[basx002] conform to rules and exponent will be in permitted range).",
          "input": "1"
        },
        {"description": "[basx148] Numbers with E", "input": "1E+009", "expected": "1E+9"},
        {"description": "[basx153] Numbers with E", "input": "1E009", "expected": "1E+9"},
        {"description": "[basx141] Numbers with E", "input": "1e+09", "expected": "1E+9"},
        {"description": "[basx146] Numbers with E", "input": "1E+09", "expected": "1E+9"},
        {"description": "[basx151] Numbers with E", "input": "1e09", "expected": "1E+9"},
        {"description": "[basx142] Numbers with E", "input": "1E+90"},
        {"description": "[basx147] Numbers with E", "input": "1e+90", "expected": "1E+90"},
        {"description": "[basx152] Numbers with E", "input": "1E90", "expected": "1E+90"},
        {"description": "[basx140] Numbers with E", "input": "1E+9"},
        {"description": "[basx150] Numbers with E", "input": "1E9", "expected": "1E+9"},
        {
          "description": "[basx014] conform to rules and exponent will be in permitted range).",
          "input": "1.234"
        },
        {"description": "[basx170] Numbers with E", "input": "1.265"},
        {"description": "[basx177] Numbers with E", "input": "1.265E-0", "expected": "1.265"},
        {"description": "[basx176] Numbers with E", "input": "1.265E-1", "expected": "0.1265"},
        {"description": "[basx178] Numbers with E", "input": "1.265E+1", "expected": "12.65"},
        {"description": "[basx171] Numbers with E", "input": "1.265E-20"},
        {"description": "[basx183] Numbers with E", "input": "1.265E+20"},
        {"description": "[basx175] Numbers with E", "input": "1.265E-2", "expected": "0.01265"},
        {"description": "[basx179] Numbers with E", "input": "1.265E+2", "expected": "126.5"},
        {"description": "[basx174] Numbers with E", "input": "1.265E-3", "expected": "0.001265"},
        {"description": "[basx180] Numbers with E", "input": "1.265E+3", "expected": "1265"},
        {"description": "[basx173] Numbers with E", "input": "1.265E-4", "expected": "0.0001265"},
        {"description": "[basx181] Numbers with E", "input": "1.265E+4"},
        {"description": "[basx172] Numbers with E", "input": "1.265E-8"},
        {"description": "[basx182] Numbers with E", "input": "1.265E+8"},
        {"description": "[basx157] Numbers with E", "input": "4E+9"},
        {"description": "[basx067] examples", "input": "5E-6", "expected": "0.000005"},
        {"description": "[basx069] examples", "input": "5E-7"},
        {"description": "[basx385] Engineering notation tests", "input": "7E0", "expected": "7"},
        {
          "description": "[basx365] Engineering notation tests",
          "input": "7E10",
          "expected": "7E+10"
        },
        {"description": "[basx405] Engineering notation tests", "input": "7E-10"},
        {
          "description": "[basx363] Engineering notation tests",
          "input": "7E11",
          "expected": "7E+11"
        },
        {"description": "[basx407] Engineering notation tests", "input": "7E-11"},
        {
          "description": "[basx361] Engineering notation tests",
          "input": "7E12",
          "expected": "7E+12"
        },
        {"description": "[basx409] Engineering notation tests", "input": "7E-12"},
        {"description": "[basx411] Engineering notation tests", "input": "7E-13"},
        {"description": "[basx383] Engineering notation tests", "input": "7E1", "expected": "7E+1"},
        {"description": "[basx387] Engineering notation tests", "input": "7E-1", "expected": "0.7"},
        {"description": "[basx381] Engineering notation tests", "input": "7E2", "expected": "7E+2"},
        {
          "description": "[basx389] Engineering notation tests",
          "input": "7E-2",
          "expected": "0.07"
        },
        {"description": "[basx379] Engineering notation tests", "input": "7E3", "expected": "7E+3"},
        {
          "description": "[basx391] Engineering notation tests",
          "input": "7E-3",
          "expected": "0.007"
        },
        {"description": "[basx377] Engineering notation tests", "input": "7E4", "expected": "7E+4"},
        {
          "description": "[basx393] Engineering notation tests",
          "input": "7E-4",
          "expected": "0.0007"
        },
        {"description": "[basx375] Engineering notation tests", "input": "7E5", "expected": "7E+5"},
        {
          "description": "[basx395] Engineering notation tests",
          "input": "7E-5",
          "expected": "0.00007"
        },
        {"description": "[basx373] Engineering notation tests", "input": "7E6", "expected": "7E+6"},
        {
          "description": "[basx397] Engineering notation tests",
          "input": "7E-6",
          "expected": "0.000007"
        },
        {"description": "[basx371] Engineering notation tests", "input": "7E7", "expected": "7E+7"},
        {"description": "[basx399] Engineering notation tests", "input": "7E-7"},
        {"description": "[basx369] Engineering notation tests", "input": "7E8", "expected": "7E+8"},
        {"description": "[basx401] Engineering notation tests", "input": "7E-8"},
        {"description": "[basx367] Engineering notation tests", "input": "7E9", "expected": "7E+9"},
        {"description": "[basx403] Engineering notation tests", "input": "7E-9"},
        {
          "description": "[basx007] conform to rules and exponent will be in permitted range).",
          "input": "10.0"
        },
        {
          "description": "[basx005] conform to rules and exponent will be in permitted range).",
          "input": "10"
        },
        {"description": "[basx165] Numbers with E", "input": "10E+009", "expected": "1.0E+10"},
        {"description": "[basx163] Numbers with E", "input": "10E+09", "expected": "1.0E+10"},
        {"description": "[basx325] Engineering notation tests", "input": "10e0", "expected": "10"},
        {
          "description": "[basx305] Engineering notation tests",
          "input": "10e10",
          "expected": "1.0E+11"
        },
        {
          "description": "[basx345] Engineering notation tests",
          "input": "10e-10",
          "expected": "1.0E-9"
        },
        {
          "description": "[basx303] Engineering notation tests",
          "input": "10e11",
          "expected": "1.0E+12"
        },
        {
          "description": "[basx347] Engineering notation tests",
          "input": "10e-11",
          "expected": "1.0E-10"
        },
        {
          "description": "[basx301] Engineering notation tests",
          "input": "10e12",
          "expected": "1.0E+13"
        },
        {
          "description": "[basx349] Engineering notation tests",
          "input": "10e-12",
          "expected": "1.0E-11"
        },
        {
          "description": "[basx351] Engineering notation tests",
          "input": "10e-13",
          "expected": "1.0E-12"
        },
        {
          "description": "[basx323] Engineering notation tests",
          "input": "10e1",
          "expected": "1.0E+2"
        },
        {
          "description": "[basx327] Engineering notation tests",
          "input": "10e-1",
          "expected": "1.0"
        },
        {
          "description": "[basx321] Engineering notation tests",
          "input": "10e2",
          "expected": "1.0E+3"
        },
        {
          "description": "[basx329] Engineering notation tests",
          "input": "10e-2",
          "expected": "0.10"
        },
        {
          "description": "[basx319] Engineering notation tests",
          "input": "10e3",
          "expected": "1.0E+4"
        },
        {
          "description": "[basx331] Engineering notation tests",
          "input": "10e-3",
          "expected": "0.010"
        },
        {
          "description": "[basx317] Engineering notation tests",
          "input": "10e4",
          "expected": "1.0E+5"
        },
        {
          "description": "[basx333] Engineering notation tests",
          "input": "10e-4",
          "expected": "0.0010"
        },
        {
          "description": "[basx315] Engineering notation tests",
          "input": "10e5",
          "expected": "1.0E+6"
        },
        {
          "description": "[basx335] Engineering notation tests",
          "input": "10e-5",
          "expected": "0.00010"
        },
        {
          "description": "[basx313] Engineering notation tests",
          "input": "10e6",
          "expected": "1.0E+7"
        },
        {
          "description": "[basx337] Engineering notation tests",
          "input": "10e-6",
          "expected": "0.000010"
        },
        {
          "description": "[basx311] Engineering notation tests",
          "input": "10e7",
          "expected": "1.0E+8"
        },
        {
          "description": "[basx339] Engineering notation tests",
          "input": "10e-7",
          "expected": "0.0000010"
        },
        {
          "description": "[basx309] Engineering notation tests",
          "input": "10e8",
          "expected": "1.0E+9"
        },
        {
          "description": "[basx341] Engineering notation tests",
          "input": "10e-8",
          "expected": "1.0E-7"
        },
        {"description": "[basx164] Numbers with E", "input": "10e+90", "expected": "1.0E+91"},
        {"description": "[basx162] Numbers with E", "input": "10E+9", "expected": "1.0E+10"},
        {
          "description": "[basx307] Engineering notation tests",
          "input": "10e9",
          "expected": "1.0E+10"
        },
        {
          "description": "[basx343] Engineering notation tests",
          "input": "10e-9",
          "expected": "1.0E-8"
        },
        {
          "description": "[basx008] conform to rules and exponent will be in permitted range).",
          "input": "10.1"
        },
        {
          "description": "[basx009] conform to rules and exponent will be in permitted range).",
          "input": "10.4"
        },
        {
          "description": "[basx010] conform to rules and exponent will be in permitted range).",
          "input": "10.5"
        },
        {
          "description": "[basx011] conform to rules and exponent will be in permitted range).",
          "input": "10.6"
        },
        {
          "description": "[basx012] conform to rules and exponent will be in permitted range).",
          "input": "10.9"
        },
        {
          "description": "[basx013] conform to rules and exponent will be in permitted range).",
          "input": "11.0"
        },
        {"description": "[basx040] strings without E cannot generate E in result", "input": "12"},
        {"description": "[basx190] Numbers with E", "input": "12.65"},
        {"description": "[basx197] Numbers with E", "input": "12.65E-0", "expected": "12.65"},
        {"description": "[basx196] Numbers with E", "input": "12.65E-1", "expected": "1.265"},
        {"description": "[basx198] Numbers with E", "input": "12.65E+1", "expected": "126.5"},
        {"description": "[basx191] Numbers with E", "input": "12.65E-20", "expected": "1.265E-19"},
        {"description": "[basx203] Numbers with E", "input": "12.65E+20", "expected": "1.265E+21"},
        {"description": "[basx195] Numbers with E", "input": "12.65E-2", "expected": "0.1265"},
        {"description": "[basx199] Numbers with E", "input": "12.65E+2", "expected": "1265"},
        {"description": "[basx194] Numbers with E", "input": "12.65E-3", "expected": "0.01265"},
        {"description": "[basx200] Numbers with E", "input": "12.65E+3", "expected": "1.265E+4"},
        {"description": "[basx193] Numbers with E", "input": "12.65E-4", "expected": "0.001265"},
        {"description": "[basx201] Numbers with E", "input": "12.65E+4", "expected": "1.265E+5"},
        {"description": "[basx192] Numbers with E", "input": "12.65E-8", "expected": "1.265E-7"},
        {"description": "[basx202] Numbers with E", "input": "12.65E+8", "expected": "1.265E+9"},
        {
          "description": "[basx044] strings without E cannot generate E in result",
          "input": "012.76",
          "expected": "12.76"
        },
        {
          "description": "[basx042] strings without E cannot generate E in result",
          "input": "12.76"
        },
        {
          "description": "[basx046] strings without E cannot generate E in result",
          "input": "17.",
          "expected": "17"
        },
        {
          "description": "[basx049] strings without E cannot generate E in result",
          "input": "0044",
          "expected": "44"
        },
        {
          "description": "[basx048] strings without E cannot generate E in result",
          "input": "044",
          "expected": "44"
        },
        {"description": "[basx158] Numbers with E", "input": "44E+9", "expected": "4.4E+10"},
        {"description": "[basx068] examples", "input": "50E-7", "expected": "0.0000050"},
        {"description": "[basx169] Numbers with E", "input": "100e+009", "expected": "1.00E+11"},
        {"description": "[basx167] Numbers with E", "input": "100e+09", "expected": "1.00E+11"},
        {"description": "[basx168] Numbers with E", "input": "100E+90", "expected": "1.00E+92"},
        {"description": "[basx166] Numbers with E", "input": "100e+9", "expected": "1.00E+11"},
        {"description": "[basx210] Numbers with E", "input": "126.5"},
        {"description": "[basx217] Numbers with E", "input": "126.5E-0", "expected": "126.5"},
        {"description": "[basx216] Numbers with E", "input": "126.5E-1", "expected": "12.65"},
        {"description": "[basx218] Numbers with E", "input": "126.5E+1", "expected": "1265"},
        {"description": "[basx211] Numbers with E", "input": "126.5E-20", "expected": "1.265E-18"},
        {"description": "[basx223] Numbers with E", "input": "126.5E+20", "expected": "1.265E+22"},
        {"description": "[basx215] Numbers with E", "input": "126.5E-2", "expected": "1.265"},
        {"description": "[basx219] Numbers with E", "input": "126.5E+2", "expected": "1.265E+4"},
        {"description": "[basx214] Numbers with E", "input": "126.5E-3", "expected": "0.1265"},
        {"description": "[basx220] Numbers with E", "input": "126.5E+3", "expected": "1.265E+5"},
        {"description": "[basx213] Numbers with E", "input": "126.5E-4", "expected": "0.01265"},
        {"description": "[basx221] Numbers with E", "input": "126.5E+4", "expected": "1.265E+6"},
        {"description": "[basx212] Numbers with E", "input": "126.5E-8", "expected": "0.000001265"},
        {"description": "[basx222] Numbers with E", "input": "126.5E+8", "expected": "1.265E+10"},
        {
          "description": "[basx006] conform to rules and exponent will be in permitted range).",
          "input": "1000"
        },
        {"description": "[basx230] Numbers with E", "input": "1265"},
        {"description": "[basx237] Numbers with E", "input": "1265E-0", "expected": "1265"},
        {"description": "[basx236] Numbers with E", "input": "1265E-1", "expected": "126.5"},
        {"description": "[basx238] Numbers with E", "input": "1265E+1", "expected": "1.265E+4"},
        {"description": "[basx231] Numbers with E", "input": "1265E-20", "expected": "1.265E-17"},
        {"description": "[basx243] Numbers with E", "input": "1265E+20", "expected": "1.265E+23"},
        {"description": "[basx235] Numbers with E", "input": "1265E-2", "expected": "12.65"},
        {"description": "[basx239] Numbers with E", "input": "1265E+2", "expected": "1.265E+5"},
        {"description": "[basx234] Numbers with E", "input": "1265E-3", "expected": "1.265"},
        {"description": "[basx240] Numbers with E", "input": "1265E+3", "expected": "1.265E+6"},
        {"description": "[basx233] Numbers with E", "input": "1265E-4", "expected": "0.1265"},
        {"description": "[basx241] Numbers with E", "input": "1265E+4", "expected": "1.265E+7"},
        {"description": "[basx232] Numbers with E", "input": "1265E-8", "expected": "0.00001265"},
        {"description": "[basx242] Numbers with E", "input": "1265E+8", "expected": "1.265E+11"},
        {
          "description": "[basx060] strings without E cannot generate E in result",
          "input": "345678.5432"
        },
        {
          "description": "[basx059] strings without E cannot generate E in result",
          "input": "0345678.54321",
          "expected": "345678.54321"
        },
        {
          "description": "[basx058] strings without E cannot generate E in result",
          "input": "345678.543210"
        },
        {
          "description": "[basx057] strings without E cannot generate E in result",
          "input": "2345678.543210"
        },
        {
          "description": "[basx056] strings without E cannot generate E in result",
          "input": "12345678.543210"
        },
        {
          "description": "[basx031] conform to rules and exponent will be in permitted range).",
          "input": "123456789.000000"
        },
        {
          "description": "[basx030] conform to rules and exponent will be in permitted range).",
          "input": "123456789.123456"
        },
        {
          "description": "[basx032] conform to rules and exponent will be in permitted range).",
          "input": "123456789123456"
        }
    ];

    data.forEach(function(testCase) {
        print(`Test - ${testCase.description}`);
        var output = NumberDecimal(testCase.input).toString();
        if (testCase.expected) {
            assert.eq(output, `NumberDecimal("${testCase.expected}")`);
        } else {
            assert.eq(output, `NumberDecimal("${testCase.input}")`);
        }
    });
}());