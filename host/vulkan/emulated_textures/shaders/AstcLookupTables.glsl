// Lookup tables for the ASTC decoder

// Number of trits, quints and bits that are used to encode the weights.
// Refer to "Table C.2.7 - Weight Range Encodings"
const uvec3 kWeightEncodings[] = {
    {0, 0, 0}, {0, 0, 0}, {0, 0, 1}, {1, 0, 0}, {0, 0, 2}, {0, 1, 0}, {1, 0, 1}, {0, 0, 3},
    {0, 0, 0}, {0, 0, 0}, {0, 1, 1}, {1, 0, 2}, {0, 0, 4}, {0, 1, 2}, {1, 0, 3}, {0, 0, 5},
};

// Number of trits, quints, and bits that we are used to encode the color endpoints, sorted from
// largest to smallest. This is the data from "Table C.2.16", but with the addition of encodings
// that use only bits.
const uvec3 kColorEncodings[] = {
    {0, 0, 8},  // 255
    {1, 0, 6},  // 191
    {0, 1, 5},  // 159
    {0, 0, 7},  // 127
    {1, 0, 5},  // 95
    {0, 1, 4},  // 79
    {0, 0, 6},  // 63
    {1, 0, 4},  // 47
    {0, 1, 3},  // 39
    {0, 0, 5},  // 31
    {1, 0, 3},  // 23
    {0, 1, 2},  // 19
    {0, 0, 4},  // 15
    {1, 0, 2},  // 11
    {0, 1, 1},  // 9
    {0, 0, 3},  // 7
    {1, 0, 1},  // 5
    {0, 0, 2},  // 3
    {0, 0, 1},  // 1
};

// Lookup table to decode a pack of 5 trits (encoded with 8 bits)
// index: the 8 bits that make the pack of 5 trits
// output: the values for the 5 trits, packed together using 2 bits each
// Refer to "C.2.12  Integer Sequence Encoding"
const uint kTritEncodings[256] = {
    0,   1,   2,   32,  4,   5,   6,   33,  8,   9,   10,  34,  40,  41,  42,  34,  16,  17,  18,
    36,  20,  21,  22,  37,  24,  25,  26,  38,  640, 641, 642, 672, 64,  65,  66,  96,  68,  69,
    70,  97,  72,  73,  74,  98,  104, 105, 106, 98,  80,  81,  82,  100, 84,  85,  86,  101, 88,
    89,  90,  102, 644, 645, 646, 673, 128, 129, 130, 160, 132, 133, 134, 161, 136, 137, 138, 162,
    168, 169, 170, 162, 144, 145, 146, 164, 148, 149, 150, 165, 152, 153, 154, 166, 648, 649, 650,
    674, 512, 513, 514, 544, 516, 517, 518, 545, 520, 521, 522, 546, 552, 553, 554, 546, 528, 529,
    530, 548, 532, 533, 534, 549, 536, 537, 538, 550, 680, 681, 682, 674, 256, 257, 258, 288, 260,
    261, 262, 289, 264, 265, 266, 290, 296, 297, 298, 290, 272, 273, 274, 292, 276, 277, 278, 293,
    280, 281, 282, 294, 656, 657, 658, 676, 320, 321, 322, 352, 324, 325, 326, 353, 328, 329, 330,
    354, 360, 361, 362, 354, 336, 337, 338, 356, 340, 341, 342, 357, 344, 345, 346, 358, 660, 661,
    662, 677, 384, 385, 386, 416, 388, 389, 390, 417, 392, 393, 394, 418, 424, 425, 426, 418, 400,
    401, 402, 420, 404, 405, 406, 421, 408, 409, 410, 422, 664, 665, 666, 678, 576, 577, 578, 608,
    580, 581, 582, 609, 584, 585, 586, 610, 616, 617, 618, 610, 592, 593, 594, 612, 596, 597, 598,
    613, 600, 601, 602, 614, 680, 681, 682, 678};

// Lookup table to decode a pack of 3 quints (encoded with 7 bits)
// index: the 7 bits that make the pack of 3 quints
// output: the values for the 3 quints, packed together using 3 bits each
// Refer to "C.2.12  Integer Sequence Encoding"
const uint kQuintEncodings[128] = {
    0,   1,   2,   3,   4,   32,  36,  292, 8,   9,   10,  11,  12,  33,  100, 292, 16,  17,  18,
    19,  20,  34,  164, 292, 24,  25,  26,  27,  28,  35,  228, 292, 64,  65,  66,  67,  68,  96,
    260, 288, 72,  73,  74,  75,  76,  97,  268, 289, 80,  81,  82,  83,  84,  98,  276, 290, 88,
    89,  90,  91,  92,  99,  284, 291, 128, 129, 130, 131, 132, 160, 258, 259, 136, 137, 138, 139,
    140, 161, 266, 267, 144, 145, 146, 147, 148, 162, 274, 275, 152, 153, 154, 155, 156, 163, 282,
    283, 192, 193, 194, 195, 196, 224, 256, 257, 200, 201, 202, 203, 204, 225, 264, 265, 208, 209,
    210, 211, 212, 226, 272, 273, 216, 217, 218, 219, 220, 227, 280, 281};

// Array to unquantize weights encoded with the trit + bits encoding.
// Use `3 * (2^bits - 1) + trit` to find the index in this table.
const uint kUnquantTritWeightMap[45] = {
    0, 32, 64, 0,  64, 12, 52, 25, 39, 0,  64, 17, 47, 5,  59, 23, 41, 11, 53, 28, 36, 0,  64,
    8, 56, 16, 48, 24, 40, 2,  62, 11, 53, 19, 45, 27, 37, 5,  59, 13, 51, 22, 42, 30, 34,
};

// Array to unquantize weights encoded with the quint + bits encoding.
// Use `5 * (2^bits - 1) + quint` to find the index in this table.
const uint kUnquantQuintWeightMap[35] = {
    0,  16, 32, 48, 64, 0, 64, 7,  57, 14, 50, 21, 43, 28, 36, 0,  64, 16,
    48, 3,  61, 19, 45, 6, 58, 23, 41, 9,  55, 26, 38, 13, 51, 29, 35,
};

// Array to unquantize color endpoint data encoded with the trit + bits encoding.
// Use `3 * (2^bits - 1) + quint` to find the index in this table.
const uint kUnquantTritColorMap[381] = {
    0,   0,   0,   0,   255, 51,  204, 102, 153, 0,   255, 69,  186, 23,  232, 92,  163, 46,  209,
    116, 139, 0,   255, 33,  222, 66,  189, 99,  156, 11,  244, 44,  211, 77,  178, 110, 145, 22,
    233, 55,  200, 88,  167, 121, 134, 0,   255, 16,  239, 32,  223, 48,  207, 65,  190, 81,  174,
    97,  158, 113, 142, 5,   250, 21,  234, 38,  217, 54,  201, 70,  185, 86,  169, 103, 152, 119,
    136, 11,  244, 27,  228, 43,  212, 59,  196, 76,  179, 92,  163, 108, 147, 124, 131, 0,   255,
    8,   247, 16,  239, 24,  231, 32,  223, 40,  215, 48,  207, 56,  199, 64,  191, 72,  183, 80,
    175, 88,  167, 96,  159, 104, 151, 112, 143, 120, 135, 2,   253, 10,  245, 18,  237, 26,  229,
    35,  220, 43,  212, 51,  204, 59,  196, 67,  188, 75,  180, 83,  172, 91,  164, 99,  156, 107,
    148, 115, 140, 123, 132, 5,   250, 13,  242, 21,  234, 29,  226, 37,  218, 45,  210, 53,  202,
    61,  194, 70,  185, 78,  177, 86,  169, 94,  161, 102, 153, 110, 145, 118, 137, 126, 129, 0,
    255, 4,   251, 8,   247, 12,  243, 16,  239, 20,  235, 24,  231, 28,  227, 32,  223, 36,  219,
    40,  215, 44,  211, 48,  207, 52,  203, 56,  199, 60,  195, 64,  191, 68,  187, 72,  183, 76,
    179, 80,  175, 84,  171, 88,  167, 92,  163, 96,  159, 100, 155, 104, 151, 108, 147, 112, 143,
    116, 139, 120, 135, 124, 131, 1,   254, 5,   250, 9,   246, 13,  242, 17,  238, 21,  234, 25,
    230, 29,  226, 33,  222, 37,  218, 41,  214, 45,  210, 49,  206, 53,  202, 57,  198, 61,  194,
    65,  190, 69,  186, 73,  182, 77,  178, 81,  174, 85,  170, 89,  166, 93,  162, 97,  158, 101,
    154, 105, 150, 109, 146, 113, 142, 117, 138, 121, 134, 125, 130, 2,   253, 6,   249, 10,  245,
    14,  241, 18,  237, 22,  233, 26,  229, 30,  225, 34,  221, 38,  217, 42,  213, 46,  209, 50,
    205, 54,  201, 58,  197, 62,  193, 66,  189, 70,  185, 74,  181, 78,  177, 82,  173, 86,  169,
    90,  165, 94,  161, 98,  157, 102, 153, 106, 149, 110, 145, 114, 141, 118, 137, 122, 133, 126,
    129,
};

// Array to unquantize color endpoint data encoded with the quint + bits encoding.
// Use `5 * (2^bits - 1) + quint` to find the index in this table.
const uint kUnquantQuintColorMap[315] = {
    0,   0,   0,   0,   0,   0,   255, 28,  227, 56,  199, 84,  171, 113, 142, 0,   255, 67,  188,
    13,  242, 80,  175, 27,  228, 94,  161, 40,  215, 107, 148, 54,  201, 121, 134, 0,   255, 32,
    223, 65,  190, 97,  158, 6,   249, 39,  216, 71,  184, 104, 151, 13,  242, 45,  210, 78,  177,
    110, 145, 19,  236, 52,  203, 84,  171, 117, 138, 26,  229, 58,  197, 91,  164, 123, 132, 0,
    255, 16,  239, 32,  223, 48,  207, 64,  191, 80,  175, 96,  159, 112, 143, 3,   252, 19,  236,
    35,  220, 51,  204, 67,  188, 83,  172, 100, 155, 116, 139, 6,   249, 22,  233, 38,  217, 54,
    201, 71,  184, 87,  168, 103, 152, 119, 136, 9,   246, 25,  230, 42,  213, 58,  197, 74,  181,
    90,  165, 106, 149, 122, 133, 13,  242, 29,  226, 45,  210, 61,  194, 77,  178, 93,  162, 109,
    146, 125, 130, 0,   255, 8,   247, 16,  239, 24,  231, 32,  223, 40,  215, 48,  207, 56,  199,
    64,  191, 72,  183, 80,  175, 88,  167, 96,  159, 104, 151, 112, 143, 120, 135, 1,   254, 9,
    246, 17,  238, 25,  230, 33,  222, 41,  214, 49,  206, 57,  198, 65,  190, 73,  182, 81,  174,
    89,  166, 97,  158, 105, 150, 113, 142, 121, 134, 3,   252, 11,  244, 19,  236, 27,  228, 35,
    220, 43,  212, 51,  204, 59,  196, 67,  188, 75,  180, 83,  172, 91,  164, 99,  156, 107, 148,
    115, 140, 123, 132, 4,   251, 12,  243, 20,  235, 28,  227, 36,  219, 44,  211, 52,  203, 60,
    195, 68,  187, 76,  179, 84,  171, 92,  163, 100, 155, 108, 147, 116, 139, 124, 131, 6,   249,
    14,  241, 22,  233, 30,  225, 38,  217, 46,  209, 54,  201, 62,  193, 70,  185, 78,  177, 86,
    169, 94,  161, 102, 153, 110, 145, 118, 137, 126, 129,
};
