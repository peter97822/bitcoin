/*****************************************************************************************************
 * Copyright (c) 2013, 2014, 2017, 2021 Pieter Wuille, Andrew Poelstra, Jonas Nick, Russell O'Connor *
 * Distributed under the MIT software license, see the accompanying                                  *
 * file COPYING or https://www.opensource.org/licenses/mit-license.php.                              *
 *****************************************************************************************************/

#include <inttypes.h>
#include <stdio.h>

#include "../include/secp256k1.h"

#include "assumptions.h"
#include "util.h"

#include "field_impl.h"
#include "group_impl.h"
#include "int128_impl.h"
#include "ecmult.h"
#include "ecmult_compute_table_impl.h"

static void print_table(FILE *fp, const char *name, int window_g, const secp256k1_ge_storage* table) {
    int j;
    int i;

    fprintf(fp, "const secp256k1_ge_storage %s[ECMULT_TABLE_SIZE(WINDOW_G)] = {\n", name);
    fprintf(fp, " S(%"PRIx32",%"PRIx32",%"PRIx32",%"PRIx32",%"PRIx32",%"PRIx32",%"PRIx32",%"PRIx32
                  ",%"PRIx32",%"PRIx32",%"PRIx32",%"PRIx32",%"PRIx32",%"PRIx32",%"PRIx32",%"PRIx32")\n",
                SECP256K1_GE_STORAGE_CONST_GET(table[0]));

    j = 1;
    for(i = 3; i <= window_g; ++i) {
        fprintf(fp, "#if WINDOW_G > %d\n", i-1);
        for(;j < ECMULT_TABLE_SIZE(i); ++j) {
            fprintf(fp, ",S(%"PRIx32",%"PRIx32",%"PRIx32",%"PRIx32",%"PRIx32",%"PRIx32",%"PRIx32",%"PRIx32
                          ",%"PRIx32",%"PRIx32",%"PRIx32",%"PRIx32",%"PRIx32",%"PRIx32",%"PRIx32",%"PRIx32")\n",
                        SECP256K1_GE_STORAGE_CONST_GET(table[j]));
        }
        fprintf(fp, "#endif\n");
    }
    fprintf(fp, "};\n");
}

static void print_two_tables(FILE *fp, int window_g) {
    secp256k1_ge_storage* table = malloc(ECMULT_TABLE_SIZE(window_g) * sizeof(secp256k1_ge_storage));
    secp256k1_ge_storage* table_128 = malloc(ECMULT_TABLE_SIZE(window_g) * sizeof(secp256k1_ge_storage));

    secp256k1_ecmult_compute_two_tables(table, table_128, window_g, &secp256k1_ge_const_g);

    print_table(fp, "secp256k1_pre_g", window_g, table);
    print_table(fp, "secp256k1_pre_g_128", window_g, table_128);

    free(table);
    free(table_128);
}

int main(void) {
    /* Always compute all tables for window sizes up to 15. */
    int window_g = (ECMULT_WINDOW_SIZE < 15) ? 15 : ECMULT_WINDOW_SIZE;
    FILE* fp;

    fp = fopen("src/precomputed_ecmult.c","w");
    if (fp == NULL) {
        fprintf(stderr, "Could not open src/precomputed_ecmult.h for writing!\n");
        return -1;
    }

    fprintf(fp, "/* This file was automatically generated by precompute_ecmult. */\n");
    fprintf(fp, "/* This file contains an array secp256k1_pre_g with odd multiples of the base point G and\n");
    fprintf(fp, " * an array secp256k1_pre_g_128 with odd multiples of 2^128*G for accelerating the computation of a*P + b*G.\n");
    fprintf(fp, " */\n");
    fprintf(fp, "#include \"group.h\"\n");
    fprintf(fp, "#include \"ecmult.h\"\n");
    fprintf(fp, "#include \"precomputed_ecmult.h\"\n");
    fprintf(fp, "#define S(a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p) SECP256K1_GE_STORAGE_CONST(0x##a##u,0x##b##u,0x##c##u,0x##d##u,0x##e##u,0x##f##u,0x##g##u,0x##h##u,0x##i##u,0x##j##u,0x##k##u,0x##l##u,0x##m##u,0x##n##u,0x##o##u,0x##p##u)\n");
    fprintf(fp, "#if ECMULT_WINDOW_SIZE > %d\n", window_g);
    fprintf(fp, "   #error configuration mismatch, invalid ECMULT_WINDOW_SIZE. Try deleting precomputed_ecmult.c before the build.\n");
    fprintf(fp, "#endif\n");
    fprintf(fp, "#ifdef EXHAUSTIVE_TEST_ORDER\n");
    fprintf(fp, "#    error Cannot compile precomputed_ecmult.c in exhaustive test mode\n");
    fprintf(fp, "#endif /* EXHAUSTIVE_TEST_ORDER */\n");
    fprintf(fp, "#define WINDOW_G ECMULT_WINDOW_SIZE\n");

    print_two_tables(fp, window_g);

    fprintf(fp, "#undef S\n");
    fclose(fp);

    return 0;
}
