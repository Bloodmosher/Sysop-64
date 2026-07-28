/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

/*
 * Sysop-64 SID filter table uploader.
 *
 * The FPGA custom table is initialized to the Galway curve.  This tool can
 * upload either built-in preset, upload an arbitrary 1024-entry hex/text file,
 * switch the SID filter between the built-in curve and the custom table,
 * and adjust the runtime cutoff scale for the 6581/8580 models.
 */

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "sysop_library.h"

static const int16_t galway_6581_table[1024] = {
       261,    262,    262,    263,    263,    264,    264,    265,
       265,    266,    266,    267,    267,    267,    268,    269,
       268,    269,    269,    270,    271,    271,    272,    272,
       273,    273,    274,    275,    275,    276,    276,    277,
       276,    277,    278,    279,    279,    280,    281,    281,
       282,    283,    283,    284,    285,    286,    286,    287,
       287,    288,    289,    290,    290,    291,    292,    293,
       293,    294,    295,    296,    297,    298,    299,    300,
       297,    299,    299,    301,    301,    303,    303,    305,
       305,    306,    307,    309,    309,    310,    312,    313,
       313,    314,    315,    316,    317,    319,    320,    322,
       322,    324,    325,    326,    327,    329,    330,    332,
       330,    332,    333,    335,    336,    338,    339,    341,
       342,    343,    345,    347,    348,    350,    351,    353,
       353,    355,    357,    358,    360,    362,    364,    366,
       367,    369,    371,    373,    375,    377,    379,    381,
       369,    371,    373,    375,    376,    379,    381,    384,
       384,    387,    389,    391,    393,    396,    398,    400,
       400,    403,    405,    408,    410,    413,    415,    419,
       420,    423,    425,    428,    430,    433,    435,    439,
       436,    440,    442,    446,    448,    452,    455,    458,
       460,    464,    466,    470,    472,    476,    479,    483,
       483,    487,    490,    494,    497,    502,    505,    510,
       511,    516,    519,    524,    528,    531,    536,    541,
       529,    534,    538,    543,    545,    550,    556,    561,
       561,    567,    572,    576,    580,    586,    590,    596,
       596,    602,    607,    613,    617,    624,    628,    635,
       637,    644,    649,    656,    658,    666,    670,    678,
       673,    680,    685,    693,    698,    706,    711,    720,
       722,    731,    736,    745,    748,    756,    762,    771,
       771,    780,    786,    796,    802,    812,    818,    828,
       831,    841,    848,    858,    865,    872,    883,    894,
       786,    796,    802,    812,    818,    828,    835,    845,
       848,    858,    865,    876,    883,    894,    901,    913,
       909,    920,    932,    943,    947,    959,    967,    980,
       984,    996,   1005,   1018,   1026,   1039,   1048,   1062,
      1048,   1062,   1076,   1090,   1094,   1109,   1118,   1133,
      1138,   1153,   1163,   1178,   1189,   1204,   1215,   1231,
      1231,   1248,   1259,   1275,   1281,   1298,   1316,   1328,
      1333,   1351,   1364,   1382,   1395,   1413,   1426,   1446,
      1401,   1420,   1433,   1452,   1465,   1485,   1499,   1519,
      1526,   1547,   1561,   1583,   1597,   1619,   1634,   1656,
      1649,   1672,   1695,   1718,   1726,   1750,   1766,   1790,
      1799,   1824,   1840,   1866,   1883,   1909,   1927,   1954,
      1927,   1954,   1981,   2008,   2018,   2046,   2065,   2093,
      2103,   2132,   2152,   2182,   2202,   2233,   2253,   2285,
      2285,   2317,   2338,   2371,   2381,   2415,   2448,   2471,
      2482,   2516,   2540,   2575,   2599,   2635,   2659,   2695,
      2516,   2551,   2575,   2610,   2635,   2671,   2695,   2733,
      2745,   2783,   2809,   2847,   2860,   2900,   2939,   2966,
      2966,   3007,   3034,   3076,   3104,   3146,   3175,   3218,
      3232,   3276,   3306,   3351,   3381,   3426,   3457,   3504,
      3457,   3504,   3535,   3582,   3614,   3663,   3695,   3745,
      3761,   3811,   3845,   3896,   3930,   3982,   4017,   4069,
      4052,   4105,   4140,   4195,   4231,   4286,   4323,   4379,
      4397,   4454,   4492,   4550,   4588,   4647,   4686,   4746,
      4608,   4667,   4706,   4766,   4806,   4866,   4907,   4968,
      4989,   5051,   5093,   5156,   5177,   5241,   5306,   5349,
      5349,   5414,   5458,   5524,   5569,   5636,   5681,   5748,
      5771,   5840,   5886,   5955,   6002,   6072,   6119,   6190,
      6119,   6190,   6238,   6310,   6358,   6431,   6480,   6553,
      6578,   6652,   6702,   6776,   6827,   6902,   6953,   7029,
      7003,   7080,   7131,   7208,   7260,   7338,   7390,   7469,
      7495,   7574,   7627,   7706,   7759,   7839,   7893,   7973,
      6358,   6431,   6480,   6553,   6602,   6677,   6726,   6801,
      6827,   6902,   6953,   7029,   7080,   7157,   7208,   7286,
      7260,   7338,   7416,   7469,   7521,   7600,   7653,   7733,
      7759,   7839,   7893,   7973,   8027,   8108,   8163,   8244,
      8163,   8244,   8326,   8381,   8435,   8518,   8573,   8656,
      8683,   8766,   8822,   8905,   8961,   9044,   9100,   9184,
      9184,   9268,   9324,   9408,   9437,   9521,   9577,   9662,
      9690,   9774,   9831,   9916,   9972,  10057,  10113,  10198,
     10000,  10085,  10141,  10226,  10283,  10367,  10424,  10508,
     10536,  10621,  10677,  10762,  10818,  10902,  10958,  11042,
     11014,  11098,  11182,  11238,  11293,  11377,  11432,  11515,
     11543,  11626,  11681,  11764,  11818,  11900,  11955,  12037,
     11955,  12037,  12118,  12172,  12226,  12307,  12360,  12440,
     12467,  12547,  12600,  12679,  12732,  12810,  12862,  12940,
     12940,  13018,  13069,  13146,  13172,  13248,  13299,  13375,
     13400,  13475,  13525,  13599,  13649,  13722,  13771,  13844,
     13475,  13550,  13599,  13673,  13722,  13795,  13844,  13916,
     13940,  14012,  14060,  14131,  14154,  14224,  14271,  14340,
     14340,  14409,  14455,  14523,  14568,  14636,  14680,  14746,
     14768,  14834,  14878,  14942,  14985,  15028,  15091,  15154,
     15091,  15154,  15196,  15258,  15299,  15360,  15401,  15461,
     15481,  15540,  15579,  15638,  15677,  15715,  15772,  15829,
     15810,  15866,  15904,  15959,  15996,  16050,  16086,  16139,
     16157,  16210,  16245,  16297,  16331,  16382,  16415,  16465,
     16348,  16399,  16432,  16482,  16515,  16564,  16596,  16644,
     16660,  16707,  16738,  16785,  16800,  16846,  16876,  16921,
     16921,  16965,  16994,  17037,  17066,  17109,  17137,  17178,
     17192,  17233,  17260,  17300,  17327,  17353,  17392,  17431,
     17392,  17431,  17456,  17494,  17519,  17555,  17580,  17616,
     17628,  17663,  17687,  17721,  17744,  17767,  17801,  17834,
     17823,  17856,  17878,  17910,  17931,  17963,  17983,  18014,
     18024,  18054,  18074,  18104,  18123,  18152,  18171,  18199,
     17878,  17910,  17942,  17973,  17983,  18014,  18034,  18064,
     18074,  18104,  18123,  18152,  18171,  18199,  18218,  18246,
     18246,  18273,  18291,  18317,  18326,  18352,  18378,  18394,
     18403,  18428,  18444,  18468,  18485,  18508,  18524,  18547,
     18532,  18555,  18570,  18592,  18600,  18622,  18644,  18658,
     18665,  18686,  18700,  18721,  18734,  18754,  18768,  18787,
     18787,  18807,  18819,  18838,  18850,  18869,  18881,  18899,
     18905,  18916,  18934,  18951,  18957,  18973,  18984,  19001,
     18962,  18979,  18995,  19011,  19017,  19033,  19043,  19058,
     19063,  19079,  19089,  19103,  19113,  19127,  19137,  19151,
     19151,  19164,  19174,  19187,  19191,  19204,  19217,  19226,
     19230,  19243,  19251,  19263,  19271,  19283,  19291,  19302,
     19295,  19306,  19314,  19325,  19329,  19340,  19350,  19357,
     19361,  19371,  19378,  19388,  19395,  19405,  19412,  19421,
     19421,  19431,  19437,  19446,  19452,  19461,  19467,  19476,
     19479,  19485,  19493,  19501,  19504,  19512,  19518,  19526,
     19485,  19493,  19499,  19507,  19512,  19520,  19526,  19533,
     19536,  19544,  19549,  19556,  19561,  19568,  19573,  19580,
     19578,  19585,  19589,  19596,  19600,  19607,  19611,  19618,
     19620,  19626,  19630,  19636,  19640,  19646,  19650,  19656,
     19650,  19656,  19660,  19665,  19669,  19674,  19678,  19683,
     19685,  19690,  19694,  19699,  19702,  19707,  19710,  19715,
     19715,  19718,  19723,  19727,  19729,  19733,  19736,  19741,
     19742,  19746,  19749,  19753,  19756,  19760,  19763,  19767,
     19757,  19761,  19764,  19768,  19771,  19774,  19777,  19780,
     19782,  19785,  19788,  19791,  19793,  19797,  19799,  19802,
     19801,  19805,  19807,  19810,  19812,  19815,  19817,  19820,
     19821,  19824,  19826,  19829,  19831,  19834,  19835,  19838,
     19835,  19838,  19840,  19842,  19844,  19847,  19848,  19851,
     19852,  19854,  19856,  19858,  19860,  19862,  19864,  19866,
     19866,  19867,  19869,  19872,  19872,  19874,  19876,  19878,
     19878,  19880,  19882,  19884,  19885,  19887,  19888,  19890
};

static const int16_t sysop64_6581_table[1024] = {
       466,    466,    466,    466,    468,    468,    471,    474,
       474,    472,    468,    463,    460,    459,    460,    462,
       465,    469,    472,    477,    480,    480,    477,    475,
       475,    474,    474,    474,    474,    472,    472,    472,
       474,    471,    471,    468,    468,    468,    469,    472,
       475,    475,    478,    480,    482,    485,    488,    490,
       491,    489,    488,    485,    482,    478,    474,    472,
       471,    471,    472,    475,    477,    477,    478,    480,
       480,    478,    477,    474,    474,    474,    477,    479,
       482,    485,    487,    489,    490,    489,    489,    487,
       486,    485,    485,    484,    484,    484,    484,    486,
       486,    485,    483,    482,    482,    481,    481,    483,
       485,    488,    493,    497,    500,    498,    496,    493,
       491,    491,    492,    494,    496,    496,    497,    499,
       499,    497,    494,    490,    491,    494,    501,    507,
       510,    509,    504,    500,    497,    498,    498,    502,
       504,    504,    503,    501,    502,    503,    506,    509,
       512,    512,    512,    512,    513,    513,    514,    516,
       517,    516,    514,    513,    513,    514,    517,    520,
       523,    524,    523,    524,    526,    529,    536,    541,
       543,    539,    531,    523,    520,    522,    528,    534,
       539,    541,    543,    543,    545,    545,    544,    543,
       545,    545,    549,    552,    552,    549,    546,    543,
       543,    548,    557,    566,    573,    575,    573,    570,
       570,    570,    572,    574,    577,    578,    581,    583,
       587,    590,    593,    597,    598,    596,    594,    591,
       591,    594,    598,    604,    608,    608,    606,    605,
       605,    605,    606,    608,    609,    610,    611,    612,
       615,    619,    623,    629,    635,    637,    640,    643,
       646,    648,    651,    653,    655,    654,    653,    653,
       655,    659,    665,    671,    677,    680,    682,    683,
       689,    696,    707,    717,    724,    724,    718,    713,
       713,    717,    725,    735,    742,    743,    743,    743,
       745,    749,    755,    760,    767,    772,    777,    782,
       788,    792,    799,    806,    811,    812,    815,    816,
       822,    827,    838,    848,    856,    861,    866,    869,
       875,    880,    888,    897,    903,    909,    911,    914,
       920,    928,    940,    952,    960,    965,    968,    972,
       978,    985,    996,   1006,   1017,   1026,   1034,   1043,
      1053,   1062,   1072,   1082,   1092,   1102,   1110,   1119,
      1131,   1141,   1154,   1166,   1178,   1187,   1196,   1204,
      1214,   1224,   1234,   1244,   1255,   1268,   1283,   1300,
      1314,   1325,   1336,   1346,   1360,   1378,   1399,   1421,
      1440,   1452,   1463,   1471,   1484,   1496,   1511,   1527,
      1543,   1557,   1572,   1587,   1603,   1621,   1640,   1659,
      1676,   1692,   1707,   1722,   1736,   1746,   1755,   1765,
      1781,   1803,   1830,   1857,   1880,   1893,   1902,   1909,
      1922,   1944,   1970,   1995,   2016,   2025,   2031,   2035,
      2046,   2063,   2084,   2109,   2132,   2151,   2172,   2191,
      2212,   2230,   2246,   2265,   2287,   2314,   2346,   2377,
      2407,   2431,   2451,   2472,   2495,   2522,   2551,   2580,
      2608,   2634,   2658,   2683,   2710,   2740,   2771,   2802,
      2832,   2858,   2881,   2906,   2933,   2964,   2998,   3032,
      3064,   3092,   3120,   3147,   3177,   3207,   3241,   3274,
      3307,   3335,   3361,   3389,   3420,   3455,   3494,   3533,
      3572,   3608,   3643,   3677,   3710,   3740,   3768,   3796,
      3827,   3857,   3888,   3922,   3958,   3998,   4041,   4085,
      4127,   4166,   4204,   4241,   4279,   4314,   4348,   4384,
      4425,   4473,   4528,   4582,   4630,   4668,   4700,   4733,
      4770,   4814,   4863,   4912,   4961,   5005,   5048,   5090,
      5137,   5188,   5242,   5297,   5350,   5394,   5434,   5476,
      5528,   5593,   5668,   5742,   5808,   5859,   5901,   5943,
      5991,   6049,   6114,   6180,   6243,   6300,   6356,   6414,
      6480,   6575,   6694,   6798,   6853,   6824,   6739,   6654,
      5433,   5441,   5492,   5562,   5625,   5681,   5741,   5803,
      5860,   5906,   5949,   5992,   6038,   6090,   6147,   6204,
      6258,   6303,   6344,   6386,   6434,   6488,   6545,   6606,
      6666,   6727,   6789,   6849,   6906,   6955,   7001,   7044,
      7090,   7135,   7181,   7228,   7279,   7333,   7393,   7452,
      7505,   7548,   7586,   7625,   7672,   7729,   7795,   7861,
      7925,   7980,   8031,   8084,   8140,   8204,   8272,   8340,
      8400,   8452,   8497,   8540,   8578,   8608,   8632,   8658,
      8698,   8754,   8823,   8894,   8959,   9013,   9062,   9109,
      9157,   9205,   9252,   9300,   9352,   9410,   9472,   9535,
      9597,   9656,   9715,   9773,   9829,   9881,   9929,   9979,
     10033,  10093,  10159,  10223,  10284,  10334,  10382,  10428,
     10476,  10524,  10573,  10624,  10678,  10735,  10794,  10853,
     10914,  10975,  11038,  11098,  11158,  11212,  11264,  11315,
     11371,  11432,  11496,  11560,  11623,  11683,  11742,  11801,
     11862,  11934,  12015,  12085,  12125,  12115,  12071,  12026,
     12018,  12062,  12137,  12221,  12297,  12357,  12413,  12468,
     12524,  12584,  12644,  12703,  12758,  12804,  12846,  12888,
     12939,  13002,  13073,  13145,  13208,  13257,  13299,  13341,
     13390,  13454,  13525,  13595,  13654,  13696,  13727,  13758,
     13799,  13856,  13922,  13989,  14047,  14090,  14124,  14159,
     14206,  14270,  14345,  14420,  14484,  14529,  14566,  14600,
     14640,  14687,  14738,  14791,  14844,  14898,  14955,  15011,
     15065,  15114,  15163,  15209,  15252,  15289,  15321,  15353,
     15390,  15433,  15480,  15528,  15573,  15614,  15654,  15693,
     15733,  15775,  15817,  15861,  15906,  15952,  15999,  16046,
     16096,  16146,  16197,  16248,  16295,  16337,  16375,  16412,
     16455,  16504,  16558,  16611,  16656,  16691,  16720,  16748,
     16783,  16827,  16876,  16925,  16970,  17007,  17039,  17069,
     17102,  17136,  17172,  17207,  17243,  17279,  17316,  17352,
     17386,  17416,  17444,  17471,  17502,  17536,  17573,  17610,
     17646,  17685,  17729,  17763,  17776,  17753,  17704,  17655,
     17632,  17649,  17687,  17734,  17772,  17797,  17818,  17838,
     17863,  17894,  17927,  17961,  17991,  18016,  18039,  18060,
     18079,  18093,  18103,  18115,  18134,  18163,  18199,  18236,
     18270,  18300,  18329,  18355,  18375,  18385,  18389,  18392,
     18403,  18423,  18449,  18477,  18502,  18523,  18544,  18562,
     18578,  18591,  18600,  18609,  18621,  18638,  18656,  18676,
     18695,  18713,  18733,  18752,  18770,  18787,  18803,  18820,
     18836,  18854,  18872,  18890,  18904,  18914,  18921,  18928,
     18937,  18946,  18957,  18968,  18981,  18996,  19012,  19028,
     19045,  19060,  19075,  19090,  19105,  19119,  19133,  19147,
     19159,  19170,  19179,  19189,  19199,  19209,  19219,  19229,
     19240,  19251,  19262,  19274,  19284,  19295,  19304,  19313,
     19323,  19332,  19342,  19352,  19361,  19371,  19380,  19390,
     19399,  19408,  19418,  19427,  19435,  19440,  19443,  19447,
     19452,  19460,  19468,  19477,  19486,  19496,  19506,  19516,
     19523,  19529,  19532,  19535,  19536,  19535,  19533,  19530,
     19530,  19532,  19535,  19539,  19544,  19549,  19556,  19562,
     19568,  19571,  19571,  19572,  19573,  19574,  19574,  19575,
     19576,  19577,  19577,  19578,  19579,  19579,  19580,  19580,
     19581,  19582,  19582,  19583,  19584,  19584,  19585,  19585,
     19586,  19586,  19587,  19587,  19588,  19588,  19589,  19589,
     19590,  19590,  19591,  19591,  19592,  19592,  19593,  19593,
     19594,  19594,  19594,  19595,  19595,  19596,  19596,  19597,
     19597,  19597,  19598,  19598,  19599,  19599,  19599,  19600,
     19600,  19601,  19601,  19601,  19602,  19602,  19602,  19603,
     19603,  19603,  19604,  19604,  19604,  19605,  19605,  19605,
     19606,  19606,  19606,  19606,  19607,  19607,  19607,  19607,
     19608,  19608,  19608,  19608,  19609,  19609,  19609,  19609,
     19610,  19610,  19610,  19610,  19610,  19611,  19611,  19611,
     19611,  19611,  19612,  19612,  19612,  19612,  19612,  19613,
     19613,  19613,  19613,  19613,  19613,  19614,  19614,  19614,
     19614,  19614,  19614,  19615,  19615,  19615,  19615,  19615
};

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s builtin\n"
        "  %s custom\n"
        "  %s sid-6581 | 6581\n"
        "  %s sid-8580 | 8580\n"
        "  %s sid-volume <0-255>\n"
        "  %s upload-galway [--activate]\n"
        "  %s upload-follin [--activate]\n"
        "  %s upload-average [--activate]\n"
        "  %s upload-strong [--activate]\n"
        "  %s upload-extreme [--activate]\n"
        "  %s upload-sysop64 [--activate]\n"
        "  %s upload <table.hex> [--activate]\n"
        "  %s scale-6581 <scale>\n"
        "  %s scale-8580 <scale>\n"
        "  %s sid2-enable\n"
        "  %s sid2-disable\n"
        "  %s sid2-base DE00|DE20|...|DFE0\n"
        "  %s server [port] [html-path]\n"
        "\n"
        "Tables are 1024 signed 16-bit entries. File input accepts hex values,\n"
        "one or more per line; // comments are ignored.\n",
        argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0,
        argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0);
}


static int parse_scale_q8_8(const char *text, uint16_t *out)
{
    char *end = NULL;
    errno = 0;
    double value = strtod(text, &end);
    if (end == text || *end != '\0' || errno != 0 || value < 0.0) {
        return -1;
    }

    double scaled = value * 256.0;
    if (scaled > 65535.0) {
        return -1;
    }

    *out = (uint16_t)(scaled + 0.5);
    return 0;
}

static int parse_u8(const char *text, uint8_t *out)
{
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 0);
    if (end == text || *end != '\0' || errno != 0 || value > 255) {
        return -1;
    }
    *out = (uint8_t)value;
    return 0;
}

static uint8_t selected_sid = 1;

static int parse_sid_number(const char *text, uint8_t *sid)
{
    if (strcmp(text, "1") == 0 || strcmp(text, "sid1") == 0 || strcmp(text, "SID1") == 0) {
        *sid = 1;
        return 0;
    }
    if (strcmp(text, "2") == 0 || strcmp(text, "sid2") == 0 || strcmp(text, "SID2") == 0) {
        *sid = 2;
        return 0;
    }
    return -1;
}

static int set_sid_model_for(uint8_t sid, const char *model)
{
    if (strcmp(model, "6581") == 0 || strcmp(model, "sid-6581") == 0 || strcmp(model, "--sid-6581") == 0) {
        sysop_sid_set_model(sid, false);
        return 0;
    }
    if (strcmp(model, "8580") == 0 || strcmp(model, "sid-8580") == 0 || strcmp(model, "--sid-8580") == 0) {
        sysop_sid_set_model(sid, true);
        return 0;
    }
    return -1;
}

static int set_sid_model(const char *model)
{
    return set_sid_model_for(selected_sid, model);
}

static int parse_sid_base(const char *text, uint16_t *base)
{
    char *end = NULL;
    int radix = 16;
    errno = 0;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        radix = 0;
    }
    unsigned long value = strtoul(text, &end, radix);
    if (end == text || *end != '\0' || errno != 0 || value < 0xde00 || value > 0xdfff || (value & 0x1f) != 0) {
        return -1;
    }
    *base = (uint16_t)value;
    return 0;
}
static int parse_value(const char *token, int16_t *out)
{
    char *end = NULL;
    int base = 16;

    if (token[0] == '-' || token[0] == '+') {
        base = 0;
    } else if (token[0] == '0' && (token[1] == 'x' || token[1] == 'X')) {
        base = 0;
    }

    errno = 0;
    long value = strtol(token, &end, base);
    if (end == token || *end != '\0' || errno != 0) {
        return -1;
    }

    if (value < -32768 || value > 0xffff) {
        return -1;
    }

    *out = (int16_t)(uint16_t)value;
    return 0;
}

static int load_table_file(const char *path, int16_t *table, size_t count)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        perror(path);
        return -1;
    }

    char line[512];
    size_t index = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *comment = strstr(line, "//");
        if (comment) {
            *comment = '\0';
        }

        char *token = strtok(line, " \t\r\n,");
        while (token) {
            if (index >= count) {
                fprintf(stderr, "%s: too many entries\n", path);
                fclose(fp);
                return -1;
            }
            if (parse_value(token, &table[index]) != 0) {
                fprintf(stderr, "%s: invalid value '%s'\n", path, token);
                fclose(fp);
                return -1;
            }
            index++;
            token = strtok(NULL, " \t\r\n,");
        }
    }

    fclose(fp);

    if (index != count) {
        fprintf(stderr, "%s: expected %zu entries, got %zu\n", path, count, index);
        return -1;
    }

    return 0;
}


static uint16_t sid_default_fc_dac(uint16_t fc)
{
    static const uint16_t dac_6581_cutoff_bits[11] = {
        0x0020, 0x002f, 0x0052, 0x009c, 0x012b, 0x0243,
        0x0463, 0x0880, 0x107b, 0x1ff4, 0x3df3
    };
    uint32_t sum = 1u << 3;

    for (unsigned bit = 0; bit < 11; bit++) {
        if (fc & (1u << bit)) {
            sum += dac_6581_cutoff_bits[bit];
        }
    }

    return (uint16_t)(sum >> 4);
}

static int16_t clamp_s16_long(long value)
{
    if (value < -32768) {
        return -32768;
    }
    if (value > 32767) {
        return 32767;
    }
    return (int16_t)value;
}

static void fill_6581_curve_table(int16_t *table, double fc_base, double curve_shift)
{
    const double fc_offset = 1024.0 + 512.0 + curve_shift;

    for (uint16_t i = 0; i < 1024; i++) {
        uint16_t fc = (uint16_t)(i << 1);
        double hz = fc_base + 12000.0 * (1.0 + tanh(((double)sid_default_fc_dac(fc) - fc_offset) / 350.0));
        long value = lround(hz * 0.8235496645826426);
        table[i] = clamp_s16_long(value);
    }
}

static void fill_sid_default_table(int16_t *table)
{
    const double y0 = 9883.0;
    const double fc_offset = 0x480;
    const double fc_base = 250.0;

    for (uint16_t i = 0; i < 1024; i++) {
        uint16_t fc = (uint16_t)(i << 1);
        double x = ((double)sid_default_fc_dac(fc) - fc_offset) / 350.0;
        long value = lround(fc_base + y0 + (y0 * tanh(x)));
        table[i] = clamp_s16_long(value);
    }
}

static int fill_named_preset_table(const char *name, int16_t *table)
{
    if (strcmp(name, "follin") == 0 || strcmp(name, "follin-style") == 0) {
        fill_6581_curve_table(table, 240.0, -785.0);
        return 0;
    }
    if (strcmp(name, "galway") == 0 || strcmp(name, "galway-style") == 0) {
        memcpy(table, galway_6581_table, sizeof(galway_6581_table));
        return 0;
    }
    if (strcmp(name, "average") == 0) {
        fill_6581_curve_table(table, 250.0, 0.0);
        return 0;
    }
    if (strcmp(name, "strong") == 0 || strcmp(name, "strong-filter") == 0) {
        fill_6581_curve_table(table, 260.0, 400.0);
        return 0;
    }
    if (strcmp(name, "extreme") == 0 || strcmp(name, "extreme-filter") == 0) {
        fill_6581_curve_table(table, 200.0, 760.0);
        return 0;
    }
    if (strcmp(name, "default") == 0) {
        fill_sid_default_table(table);
        return 0;
    }
    if (strcmp(name, "sysop64") == 0) {
        memcpy(table, sysop64_6581_table, sizeof(sysop64_6581_table));
        return 0;
    }
    return -1;
}
static void upload_table_to_sid(uint8_t sid, const int16_t *table)
{
    for (uint16_t i = 0; i < 1024; i++) {
        sysop_sid_filter_table_write_sid(sid, i, table[i]);
        if ((i & 0x7f) == 0x7f) {
            printf("uploaded %u/1024\n", (unsigned)i + 1);
        }
    }
}

static void upload_table(const int16_t *table)
{
    upload_table_to_sid(selected_sid, table);
}



static char *sidcontrol_html = NULL;
static char sidcontrol_html_path[512];

static char *read_text_file(const char *path, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    char *data = NULL;
    long len;

    if (!fp) {
        return NULL;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }

    len = ftell(fp);
    if (len < 0) {
        fclose(fp);
        return NULL;
    }

    rewind(fp);
    data = malloc((size_t)len + 1);
    if (!data) {
        fclose(fp);
        return NULL;
    }

    if (fread(data, 1, (size_t)len, fp) != (size_t)len) {
        free(data);
        fclose(fp);
        return NULL;
    }

    fclose(fp);
    data[len] = '\0';
    if (out_len) {
        *out_len = (size_t)len;
    }
    return data;
}

static int try_load_html(const char *path)
{
    size_t len = 0;
    char *loaded = read_text_file(path, &len);
    if (!loaded) {
        return -1;
    }

    free(sidcontrol_html);
    sidcontrol_html = loaded;
    snprintf(sidcontrol_html_path, sizeof(sidcontrol_html_path), "%s", path);
    printf("sidcontrol: loaded web app %s (%zu bytes)\n", sidcontrol_html_path, len);
    return 0;
}

static int load_sidcontrol_html(const char *argv0, const char *requested_path)
{
    const char *fallbacks[] = {
        "sidcontrol.html",
        "../tools/sidcontrol.html",
        "../../tools/sidcontrol.html",
        "/usr/local/bin/sidcontrol.html",
        "/usr/local/share/sysop64/sidcontrol.html",
        "sidfilter.html",
        "../tools/sidfilter.html",
        "../../tools/sidfilter.html"
    };
    char beside_exe[512];
    const char *slash;

    if (requested_path) {
        if (try_load_html(requested_path) == 0) {
            return 0;
        }
        fprintf(stderr, "sidcontrol: could not load %s\n", requested_path);
        return -1;
    }

    slash = strrchr(argv0, '/');
    if (slash) {
        size_t dir_len = (size_t)(slash - argv0);
        if (dir_len + sizeof("/sidcontrol.html") < sizeof(beside_exe)) {
            memcpy(beside_exe, argv0, dir_len);
            strcpy(beside_exe + dir_len, "/sidcontrol.html");
            if (try_load_html(beside_exe) == 0) {
                return 0;
            }
        }
    }

    for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); i++) {
        if (try_load_html(fallbacks[i]) == 0) {
            return 0;
        }
    }

    fprintf(stderr, "sidcontrol: could not find sidcontrol.html; pass an explicit path\n");
    return -1;
}

static int send_all(int fd, const char *data, size_t len)
{
    while (len > 0) {
        ssize_t sent = send(fd, data, len, 0);
        if (sent <= 0) {
            return -1;
        }
        data += sent;
        len -= (size_t)sent;
    }
    return 0;
}

static void http_send(int fd, const char *status, const char *type, const char *body)
{
    char header[256];
    size_t body_len = strlen(body);
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n",
        status, type, body_len);
    if (header_len > 0) {
        send_all(fd, header, (size_t)header_len);
        send_all(fd, body, body_len);
    }
}

static void http_send_table(int fd, const int16_t *table)
{
    char header[256];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n");
    send_all(fd, header, (size_t)header_len);

    char line[32];
    for (int i = 0; i < 1024; i++) {
        int line_len = snprintf(line, sizeof(line), "%d\n", table[i]);
        send_all(fd, line, (size_t)line_len);
    }
}

static const char *query_value(const char *path, const char *key, char *out, size_t out_len)
{
    const char *q = strchr(path, '?');
    size_t key_len = strlen(key);
    if (!q) {
        return NULL;
    }
    q++;
    while (*q) {
        const char *next = strchr(q, '&');
        size_t part_len = next ? (size_t)(next - q) : strlen(q);
        if (part_len > key_len + 1 && strncmp(q, key, key_len) == 0 && q[key_len] == '=') {
            size_t value_len = part_len - key_len - 1;
            if (value_len >= out_len) {
                value_len = out_len - 1;
            }
            memcpy(out, q + key_len + 1, value_len);
            out[value_len] = '\0';
            return out;
        }
        if (!next) {
            break;
        }
        q = next + 1;
    }
    return NULL;
}


static uint8_t get_query_sid(const char *path)
{
    char sid_text[16];
    if (query_value(path, "sid", sid_text, sizeof(sid_text)) && strcmp(sid_text, "2") == 0) {
        return 2;
    }
    return 1;
}

static size_t http_content_length(const char *req, const char *headers_end)
{
    const char *line = req;
    while (line && line < headers_end) {
        const char *line_end = strstr(line, "\r\n");
        if (!line_end || line_end > headers_end) {
            line_end = headers_end;
        }
        if ((size_t)(line_end - line) >= 15 && strncasecmp(line, "Content-Length:", 15) == 0) {
            return (size_t)strtoul(line + 15, NULL, 10);
        }
        if (line_end == headers_end) {
            break;
        }
        line = line_end + 2;
    }
    return 0;
}

static int parse_http_value(const char *text, int16_t *out)
{
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 0);
    if (end == text || *end != '\0' || errno != 0 || value < -32768 || value > 32767) {
        return -1;
    }
    *out = (int16_t)value;
    return 0;
}

static int parse_body_table(char *body, int16_t *table)
{
    size_t index = 0;
    char *token = strtok(body, " \t\r\n,");
    while (token) {
        if (index >= 1024 || parse_http_value(token, &table[index]) != 0) {
            return -1;
        }
        index++;
        token = strtok(NULL, " \t\r\n,");
    }
    return index == 1024 ? 0 : -1;
}

static int apply_entry_pairs(uint8_t sid, char *body)
{
    char *token = strtok(body, " \t\r\n,");
    while (token) {
        char *index_text = token;
        char *value_text = strtok(NULL, " \t\r\n,");
        int16_t value;
        unsigned long index;
        if (!value_text) {
            return -1;
        }
        index = strtoul(index_text, NULL, 0);
        if (index >= 1024 || parse_http_value(value_text, &value) != 0) {
            return -1;
        }
        sysop_sid_filter_table_write_sid(sid, (uint16_t)index, value);
        token = strtok(NULL, " \t\r\n,");
    }
    return 0;
}

static void handle_http_client(int fd)
{
    enum { REQ_MAX = 131072 };
    char req[REQ_MAX + 1];
    size_t used = 0;
    ssize_t got;
    char method[8] = {0};
    char path[512] = {0};

    while (used < REQ_MAX) {
        got = recv(fd, req + used, REQ_MAX - used, 0);
        if (got <= 0) {
            return;
        }
        used += (size_t)got;
        req[used] = '\0';
        char *headers_end = strstr(req, "\r\n\r\n");
        if (headers_end) {
            size_t header_bytes = (size_t)(headers_end + 4 - req);
            size_t content_length = http_content_length(req, headers_end);
            if (used >= header_bytes + content_length) {
                break;
            }
        }
    }

    if (sscanf(req, "%7s %511s", method, path) != 2) {
        http_send(fd, "400 Bad Request", "text/plain", "bad request\n");
        return;
    }

    char *headers_end = strstr(req, "\r\n\r\n");
    char *body = headers_end ? headers_end + 4 : req + used;

    if (strcmp(method, "GET") == 0 && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
        http_send(fd, "200 OK", "text/html", sidcontrol_html);    } else if (strcmp(method, "GET") == 0 && strncmp(path, "/preset?", 8) == 0) {
        char name[32];
        int16_t preset_table[1024];
        if (!query_value(path, "name", name, sizeof(name))) {
            strcpy(name, "galway");
        }
        if (fill_named_preset_table(name, preset_table) != 0) {
            http_send(fd, "404 Not Found", "text/plain", "unknown preset\n");
        } else {
            http_send_table(fd, preset_table);
        }
    } else if (strcmp(method, "POST") == 0 && strncmp(path, "/entries", 8) == 0) {
        if (apply_entry_pairs(get_query_sid(path), body) != 0) {
            http_send(fd, "400 Bad Request", "text/plain", "bad entries\n");
            return;
        }
        http_send(fd, "200 OK", "text/plain", "ok\n");
    } else if (strcmp(method, "POST") == 0 && strncmp(path, "/entry?", 7) == 0) {
        char index_text[32], value_text[32];
        int16_t value;
        unsigned long index;
        if (!query_value(path, "index", index_text, sizeof(index_text)) ||
            !query_value(path, "value", value_text, sizeof(value_text)) ||
            parse_http_value(value_text, &value) != 0) {
            http_send(fd, "400 Bad Request", "text/plain", "bad entry\n");
            return;
        }
        index = strtoul(index_text, NULL, 0);
        if (index >= 1024) {
            http_send(fd, "400 Bad Request", "text/plain", "bad index\n");
            return;
        }
        sysop_sid_filter_table_write_sid(get_query_sid(path), (uint16_t)index, value);
        http_send(fd, "200 OK", "text/plain", "ok\n");
    } else if (strcmp(method, "POST") == 0 && strncmp(path, "/apply", 6) == 0) {
        int16_t table[1024];
        if (parse_body_table(body, table) != 0) {
            http_send(fd, "400 Bad Request", "text/plain", "expected 1024 values\n");
            return;
        }
        for (uint16_t i = 0; i < 1024; i++) {
            sysop_sid_filter_table_write_sid(get_query_sid(path), i, table[i]);
        }
        http_send(fd, "200 OK", "text/plain", "ok\n");
    } else if (strcmp(method, "POST") == 0 && strncmp(path, "/activate?", 10) == 0) {
        char enable_text[16];
        int enable = query_value(path, "enable", enable_text, sizeof(enable_text)) ? atoi(enable_text) : 1;
        sysop_sid_filter_use_custom_sid(get_query_sid(path), enable != 0);
        http_send(fd, "200 OK", "text/plain", "ok\n");
    } else if (strcmp(method, "POST") == 0 && strncmp(path, "/sid-model?", 11) == 0) {
        char model[16];
        if (!query_value(path, "model", model, sizeof(model)) ||
            set_sid_model_for(get_query_sid(path), model) != 0) {
            http_send(fd, "400 Bad Request", "text/plain", "bad SID model\n");
            return;
        }
        http_send(fd, "200 OK", "text/plain", "ok\n");
    } else if (strcmp(method, "POST") == 0 && strncmp(path, "/sid-volume?", 12) == 0) {
        char value_text[32];
        uint8_t volume;
        if (!query_value(path, "value", value_text, sizeof(value_text)) ||
            parse_u8(value_text, &volume) != 0) {
            http_send(fd, "400 Bad Request", "text/plain", "bad SID volume\n");
            return;
        }
        sysop_audio_set_sid_volume(volume, volume);
        http_send(fd, "200 OK", "text/plain", "ok\n");
    } else if (strcmp(method, "POST") == 0 && strncmp(path, "/sid2-enable?", 13) == 0) {
        char enable_text[16];
        int enable;
        if (!query_value(path, "enable", enable_text, sizeof(enable_text))) {
            http_send(fd, "400 Bad Request", "text/plain", "missing enable\n");
            return;
        }
        enable = atoi(enable_text);
        sysop_sid2_enable(enable != 0);
        http_send(fd, "200 OK", "text/plain", enable ? "sid2 enabled\n" : "sid2 disabled\n");
    } else if (strcmp(method, "POST") == 0 && strncmp(path, "/sid2-base?", 11) == 0) {
        char base_text[32];
        uint16_t base;
        if (!query_value(path, "base", base_text, sizeof(base_text)) || parse_sid_base(base_text, &base) != 0) {
            http_send(fd, "400 Bad Request", "text/plain", "bad base\n");
            return;
        }
        sysop_sid2_set_base(base);
        http_send(fd, "200 OK", "text/plain", "sid2 base updated\n");
    } else if (strcmp(method, "POST") == 0 && strncmp(path, "/scale?", 7) == 0) {
        char model[16], value_text[32];
        uint16_t scale_q8_8;
        if (!query_value(path, "model", model, sizeof(model)) ||
            !query_value(path, "value", value_text, sizeof(value_text)) ||
            parse_scale_q8_8(value_text, &scale_q8_8) != 0) {
            http_send(fd, "400 Bad Request", "text/plain", "bad scale\n");
            return;
        }
        if (strcmp(model, "8580") == 0) {
            sysop_sid_filter_set_scale_8580_sid(get_query_sid(path), scale_q8_8);
        } else {
            sysop_sid_filter_set_scale_6581_sid(get_query_sid(path), scale_q8_8);
        }
        http_send(fd, "200 OK", "text/plain", "ok\n");
    } else {
        http_send(fd, "404 Not Found", "text/plain", "not found\n");
    }
}

static int run_http_server(uint16_t port)
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return -1;
    }

    int one = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, 8) < 0) {
        perror("listen");
        close(server_fd);
        return -1;
    }

    printf("sidcontrol: listening on http://0.0.0.0:%u/\n", (unsigned)port);
    fflush(stdout);

    for (;;) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            close(server_fd);
            return -1;
        }
        handle_http_client(client_fd);
        close(client_fd);
    }
}

static int has_activate(int argc, char **argv, int start)
{
    for (int i = start; i < argc; i++) {
        if (strcmp(argv[i], "--activate") == 0) {
            return 1;
        }
        fprintf(stderr, "unknown option: %s\n", argv[i]);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int rc = 1;
    const int16_t *table = NULL;
    int16_t file_table[1024];
    int activate_arg = 2;
    int activate = 0;
    uint16_t scale_q8_8 = 0;

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (argc >= 3 && strcmp(argv[1], "--sid") == 0) {
        if (parse_sid_number(argv[2], &selected_sid) != 0) {
            fprintf(stderr, "invalid SID number: %s\n", argv[2]);
            return 1;
        }
        argv += 2;
        argc -= 2;
    }

    if (sysop_init() < 0) {
        return 1;
    }

    if (strcmp(argv[1], "server") == 0) {
        uint16_t port = 8080;
        const char *html_path = NULL;
        if (argc > 4) {
            usage(argv[0]);
            goto out;
        }
        if (argc >= 3) {
            char *end = NULL;
            unsigned long parsed_port = strtoul(argv[2], &end, 0);
            if (end != argv[2] && *end == '\0') {
                if (parsed_port == 0 || parsed_port > 65535) {
                    fprintf(stderr, "invalid port '%s'\n", argv[2]);
                    goto out;
                }
                port = (uint16_t)parsed_port;
                if (argc == 4) {
                    html_path = argv[3];
                }
            } else {
                if (argc == 4) {
                    usage(argv[0]);
                    goto out;
                }
                html_path = argv[2];
            }
        }
        if (load_sidcontrol_html(argv[0], html_path) != 0) {
            goto out;
        }
        rc = run_http_server(port) == 0 ? 0 : 1;
        goto out;
    }

    if (strcmp(argv[1], "sid2-enable") == 0) {
        sysop_sid2_enable(true);
        printf("sid2 enabled\n");
        rc = 0;
        goto out;
    }

    if (strcmp(argv[1], "sid2-disable") == 0) {
        sysop_sid2_enable(false);
        printf("sid2 disabled\n");
        rc = 0;
        goto out;
    }

    if (strcmp(argv[1], "sid2-base") == 0) {
        uint16_t base;
        if (argc != 3 || parse_sid_base(argv[2], &base) != 0) {
            fprintf(stderr, "usage: %s sid2-base DE00|DE20|...|DFE0\n", argv[0]);
            goto out;
        }
        sysop_sid2_set_base(base);
        printf("sid2 base set to $%04X\n", base);
        rc = 0;
        goto out;
    }

    if (strcmp(argv[1], "builtin") == 0) {
        sysop_sid_filter_use_custom(false);
        printf("SID filter: built-in table selected\n");
        rc = 0;
        goto out;
    }

    if (strcmp(argv[1], "custom") == 0) {
        sysop_sid_filter_use_custom(true);
        printf("SID filter: custom table selected\n");
        rc = 0;
        goto out;
    }

    if (strcmp(argv[1], "sid-6581") == 0 || strcmp(argv[1], "--sid-6581") == 0 || strcmp(argv[1], "6581") == 0 ||
        strcmp(argv[1], "sid-8580") == 0 || strcmp(argv[1], "--sid-8580") == 0 || strcmp(argv[1], "8580") == 0) {
        if (argc != 2) {
            usage(argv[0]);
            goto out;
        }
        if (set_sid_model(argv[1]) != 0) {
            usage(argv[0]);
            goto out;
        }
        printf("SID model: %s\n", strstr(argv[1], "8580") ? "8580" : "6581");
        rc = 0;
        goto out;
    }

    if (strcmp(argv[1], "sid-volume") == 0 || strcmp(argv[1], "volume") == 0 || strcmp(argv[1], "--set-sid-volume") == 0) {
        uint8_t volume;
        if (argc != 3 || parse_u8(argv[2], &volume) != 0) {
            usage(argv[0]);
            goto out;
        }
        sysop_audio_set_sid_volume(volume, volume);
        printf("SID volume: %u\n", (unsigned)volume);
        rc = 0;
        goto out;
    }

    if (strcmp(argv[1], "scale-6581") == 0 || strcmp(argv[1], "scale-8580") == 0) {
        if (argc != 3) {
            usage(argv[0]);
            goto out;
        }
        if (parse_scale_q8_8(argv[2], &scale_q8_8) != 0) {
            fprintf(stderr, "invalid scale '%s' (use a non-negative value up to 255.996)\n", argv[2]);
            goto out;
        }

        if (strcmp(argv[1], "scale-6581") == 0) {
            sysop_sid_filter_set_scale_6581_sid(selected_sid, scale_q8_8);
            printf("SID 6581 cutoff scale: %s (q8.8=%u)\n", argv[2], scale_q8_8);
        } else {
            sysop_sid_filter_set_scale_8580_sid(selected_sid, scale_q8_8);
            printf("SID 8580 cutoff scale: %s (q8.8=%u)\n", argv[2], scale_q8_8);
        }
        rc = 0;
        goto out;
    }    if (strcmp(argv[1], "upload-galway") == 0 ||
        strcmp(argv[1], "upload-follin") == 0 ||
        strcmp(argv[1], "upload-average") == 0 ||
        strcmp(argv[1], "upload-strong") == 0 ||
        strcmp(argv[1], "upload-extreme") == 0 ||
        strcmp(argv[1], "upload-sysop64") == 0) {
        const char *name = argv[1] + strlen("upload-");
        if (fill_named_preset_table(name, file_table) != 0) {
            fprintf(stderr, "unknown preset '%s'\n", name);
            goto out;
        }
        table = file_table;
    } else if (strcmp(argv[1], "upload") == 0) {
        if (argc < 3) {
            usage(argv[0]);
            goto out;
        }
        if (load_table_file(argv[2], file_table, 1024) != 0) {
            goto out;
        }
        table = file_table;
        activate_arg = 3;
    } else {
        usage(argv[0]);
        goto out;
    }

    activate = has_activate(argc, argv, activate_arg);
    if (activate < 0) {
        goto out;
    }

    upload_table(table);
    if (activate) {
        sysop_sid_filter_use_custom(true);
        printf("SID filter: custom table selected\n");
    }

    rc = 0;

out:
    sysop_uninit();
    return rc;
}
