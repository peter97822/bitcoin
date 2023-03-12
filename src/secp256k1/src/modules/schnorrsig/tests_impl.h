/***********************************************************************
 * Copyright (c) 2018-2020 Andrew Poelstra, Jonas Nick                 *
 * Distributed under the MIT software license, see the accompanying    *
 * file COPYING or https://www.opensource.org/licenses/mit-license.php.*
 ***********************************************************************/

#ifndef SECP256K1_MODULE_SCHNORRSIG_TESTS_H
#define SECP256K1_MODULE_SCHNORRSIG_TESTS_H

#include "../../../include/secp256k1_schnorrsig.h"

/* Checks that a bit flip in the n_flip-th argument (that has n_bytes many
 * bytes) changes the hash function
 */
static void nonce_function_bip340_bitflip(unsigned char **args, size_t n_flip, size_t n_bytes, size_t msglen, size_t algolen) {
    unsigned char nonces[2][32];
    CHECK(nonce_function_bip340(nonces[0], args[0], msglen, args[1], args[2], args[3], algolen, args[4]) == 1);
    secp256k1_testrand_flip(args[n_flip], n_bytes);
    CHECK(nonce_function_bip340(nonces[1], args[0], msglen, args[1], args[2], args[3], algolen, args[4]) == 1);
    CHECK(secp256k1_memcmp_var(nonces[0], nonces[1], 32) != 0);
}

/* Tests for the equality of two sha256 structs. This function only produces a
 * correct result if an integer multiple of 64 many bytes have been written
 * into the hash functions. */
static void test_sha256_eq(const secp256k1_sha256 *sha1, const secp256k1_sha256 *sha2) {
    /* Is buffer fully consumed? */
    CHECK((sha1->bytes & 0x3F) == 0);

    CHECK(sha1->bytes == sha2->bytes);
    CHECK(secp256k1_memcmp_var(sha1->s, sha2->s, sizeof(sha1->s)) == 0);
}

static void run_nonce_function_bip340_tests(void) {
    unsigned char tag[13] = "BIP0340/nonce";
    unsigned char aux_tag[11] = "BIP0340/aux";
    unsigned char algo[13] = "BIP0340/nonce";
    size_t algolen = sizeof(algo);
    secp256k1_sha256 sha;
    secp256k1_sha256 sha_optimized;
    unsigned char nonce[32], nonce_z[32];
    unsigned char msg[32];
    size_t msglen = sizeof(msg);
    unsigned char key[32];
    unsigned char pk[32];
    unsigned char aux_rand[32];
    unsigned char *args[5];
    int i;

    /* Check that hash initialized by
     * secp256k1_nonce_function_bip340_sha256_tagged has the expected
     * state. */
    secp256k1_sha256_initialize_tagged(&sha, tag, sizeof(tag));
    secp256k1_nonce_function_bip340_sha256_tagged(&sha_optimized);
    test_sha256_eq(&sha, &sha_optimized);

   /* Check that hash initialized by
    * secp256k1_nonce_function_bip340_sha256_tagged_aux has the expected
    * state. */
    secp256k1_sha256_initialize_tagged(&sha, aux_tag, sizeof(aux_tag));
    secp256k1_nonce_function_bip340_sha256_tagged_aux(&sha_optimized);
    test_sha256_eq(&sha, &sha_optimized);

    secp256k1_testrand256(msg);
    secp256k1_testrand256(key);
    secp256k1_testrand256(pk);
    secp256k1_testrand256(aux_rand);

    /* Check that a bitflip in an argument results in different nonces. */
    args[0] = msg;
    args[1] = key;
    args[2] = pk;
    args[3] = algo;
    args[4] = aux_rand;
    for (i = 0; i < COUNT; i++) {
        nonce_function_bip340_bitflip(args, 0, 32, msglen, algolen);
        nonce_function_bip340_bitflip(args, 1, 32, msglen, algolen);
        nonce_function_bip340_bitflip(args, 2, 32, msglen, algolen);
        /* Flip algo special case "BIP0340/nonce" */
        nonce_function_bip340_bitflip(args, 3, algolen, msglen, algolen);
        /* Flip algo again */
        nonce_function_bip340_bitflip(args, 3, algolen, msglen, algolen);
        nonce_function_bip340_bitflip(args, 4, 32, msglen, algolen);
    }

    /* NULL algo is disallowed */
    CHECK(nonce_function_bip340(nonce, msg, msglen, key, pk, NULL, 0, NULL) == 0);
    CHECK(nonce_function_bip340(nonce, msg, msglen, key, pk, algo, algolen, NULL) == 1);
    /* Other algo is fine */
    secp256k1_testrand_bytes_test(algo, algolen);
    CHECK(nonce_function_bip340(nonce, msg, msglen, key, pk, algo, algolen, NULL) == 1);

    for (i = 0; i < COUNT; i++) {
        unsigned char nonce2[32];
        uint32_t offset = secp256k1_testrand_int(msglen - 1);
        size_t msglen_tmp = (msglen + offset) % msglen;
        size_t algolen_tmp;

        /* Different msglen gives different nonce */
        CHECK(nonce_function_bip340(nonce2, msg, msglen_tmp, key, pk, algo, algolen, NULL) == 1);
        CHECK(secp256k1_memcmp_var(nonce, nonce2, 32) != 0);

        /* Different algolen gives different nonce */
        offset = secp256k1_testrand_int(algolen - 1);
        algolen_tmp = (algolen + offset) % algolen;
        CHECK(nonce_function_bip340(nonce2, msg, msglen, key, pk, algo, algolen_tmp, NULL) == 1);
        CHECK(secp256k1_memcmp_var(nonce, nonce2, 32) != 0);
    }

    /* NULL aux_rand argument is allowed, and identical to passing all zero aux_rand. */
    memset(aux_rand, 0, 32);
    CHECK(nonce_function_bip340(nonce_z, msg, msglen, key, pk, algo, algolen, &aux_rand) == 1);
    CHECK(nonce_function_bip340(nonce, msg, msglen, key, pk, algo, algolen, NULL) == 1);
    CHECK(secp256k1_memcmp_var(nonce_z, nonce, 32) == 0);
}

static void test_schnorrsig_api(void) {
    unsigned char sk1[32];
    unsigned char sk2[32];
    unsigned char sk3[32];
    unsigned char msg[32];
    secp256k1_keypair keypairs[3];
    secp256k1_keypair invalid_keypair = {{ 0 }};
    secp256k1_xonly_pubkey pk[3];
    secp256k1_xonly_pubkey zero_pk;
    unsigned char sig[64];
    secp256k1_schnorrsig_extraparams extraparams = SECP256K1_SCHNORRSIG_EXTRAPARAMS_INIT;
    secp256k1_schnorrsig_extraparams invalid_extraparams = {{ 0 }, NULL, NULL};

    /** setup **/
    int ecount = 0;

    secp256k1_context_set_error_callback(CTX, counting_illegal_callback_fn, &ecount);
    secp256k1_context_set_illegal_callback(CTX, counting_illegal_callback_fn, &ecount);
    secp256k1_context_set_error_callback(STATIC_CTX, counting_illegal_callback_fn, &ecount);
    secp256k1_context_set_illegal_callback(STATIC_CTX, counting_illegal_callback_fn, &ecount);

    secp256k1_testrand256(sk1);
    secp256k1_testrand256(sk2);
    secp256k1_testrand256(sk3);
    secp256k1_testrand256(msg);
    CHECK(secp256k1_keypair_create(CTX, &keypairs[0], sk1) == 1);
    CHECK(secp256k1_keypair_create(CTX, &keypairs[1], sk2) == 1);
    CHECK(secp256k1_keypair_create(CTX, &keypairs[2], sk3) == 1);
    CHECK(secp256k1_keypair_xonly_pub(CTX, &pk[0], NULL, &keypairs[0]) == 1);
    CHECK(secp256k1_keypair_xonly_pub(CTX, &pk[1], NULL, &keypairs[1]) == 1);
    CHECK(secp256k1_keypair_xonly_pub(CTX, &pk[2], NULL, &keypairs[2]) == 1);
    memset(&zero_pk, 0, sizeof(zero_pk));

    /** main test body **/
    ecount = 0;
    CHECK(secp256k1_schnorrsig_sign32(CTX, sig, msg, &keypairs[0], NULL) == 1);
    CHECK(ecount == 0);
    CHECK(secp256k1_schnorrsig_sign32(CTX, NULL, msg, &keypairs[0], NULL) == 0);
    CHECK(ecount == 1);
    CHECK(secp256k1_schnorrsig_sign32(CTX, sig, NULL, &keypairs[0], NULL) == 0);
    CHECK(ecount == 2);
    CHECK(secp256k1_schnorrsig_sign32(CTX, sig, msg, NULL, NULL) == 0);
    CHECK(ecount == 3);
    CHECK(secp256k1_schnorrsig_sign32(CTX, sig, msg, &invalid_keypair, NULL) == 0);
    CHECK(ecount == 4);
    CHECK(secp256k1_schnorrsig_sign32(STATIC_CTX, sig, msg, &keypairs[0], NULL) == 0);
    CHECK(ecount == 5);

    ecount = 0;
    CHECK(secp256k1_schnorrsig_sign_custom(CTX, sig, msg, sizeof(msg), &keypairs[0], &extraparams) == 1);
    CHECK(ecount == 0);
    CHECK(secp256k1_schnorrsig_sign_custom(CTX, NULL, msg, sizeof(msg), &keypairs[0], &extraparams) == 0);
    CHECK(ecount == 1);
    CHECK(secp256k1_schnorrsig_sign_custom(CTX, sig, NULL, sizeof(msg), &keypairs[0], &extraparams) == 0);
    CHECK(ecount == 2);
    CHECK(secp256k1_schnorrsig_sign_custom(CTX, sig, NULL, 0, &keypairs[0], &extraparams) == 1);
    CHECK(ecount == 2);
    CHECK(secp256k1_schnorrsig_sign_custom(CTX, sig, msg, sizeof(msg), NULL, &extraparams) == 0);
    CHECK(ecount == 3);
    CHECK(secp256k1_schnorrsig_sign_custom(CTX, sig, msg, sizeof(msg), &invalid_keypair, &extraparams) == 0);
    CHECK(ecount == 4);
    CHECK(secp256k1_schnorrsig_sign_custom(CTX, sig, msg, sizeof(msg), &keypairs[0], NULL) == 1);
    CHECK(ecount == 4);
    CHECK(secp256k1_schnorrsig_sign_custom(CTX, sig, msg, sizeof(msg), &keypairs[0], &invalid_extraparams) == 0);
    CHECK(ecount == 5);
    CHECK(secp256k1_schnorrsig_sign_custom(STATIC_CTX, sig, msg, sizeof(msg), &keypairs[0], &extraparams) == 0);
    CHECK(ecount == 6);

    ecount = 0;
    CHECK(secp256k1_schnorrsig_sign32(CTX, sig, msg, &keypairs[0], NULL) == 1);
    CHECK(secp256k1_schnorrsig_verify(CTX, sig, msg, sizeof(msg), &pk[0]) == 1);
    CHECK(ecount == 0);
    CHECK(secp256k1_schnorrsig_verify(CTX, NULL, msg, sizeof(msg), &pk[0]) == 0);
    CHECK(ecount == 1);
    CHECK(secp256k1_schnorrsig_verify(CTX, sig, NULL, sizeof(msg), &pk[0]) == 0);
    CHECK(ecount == 2);
    CHECK(secp256k1_schnorrsig_verify(CTX, sig, NULL, 0, &pk[0]) == 0);
    CHECK(ecount == 2);
    CHECK(secp256k1_schnorrsig_verify(CTX, sig, msg, sizeof(msg), NULL) == 0);
    CHECK(ecount == 3);
    CHECK(secp256k1_schnorrsig_verify(CTX, sig, msg, sizeof(msg), &zero_pk) == 0);
    CHECK(ecount == 4);

    secp256k1_context_set_error_callback(STATIC_CTX, NULL, NULL);
    secp256k1_context_set_illegal_callback(STATIC_CTX, NULL, NULL);
}

/* Checks that hash initialized by secp256k1_schnorrsig_sha256_tagged has the
 * expected state. */
static void test_schnorrsig_sha256_tagged(void) {
    unsigned char tag[17] = "BIP0340/challenge";
    secp256k1_sha256 sha;
    secp256k1_sha256 sha_optimized;

    secp256k1_sha256_initialize_tagged(&sha, (unsigned char *) tag, sizeof(tag));
    secp256k1_schnorrsig_sha256_tagged(&sha_optimized);
    test_sha256_eq(&sha, &sha_optimized);
}

/* Helper function for schnorrsig_bip_vectors
 * Signs the message and checks that it's the same as expected_sig. */
static void test_schnorrsig_bip_vectors_check_signing(const unsigned char *sk, const unsigned char *pk_serialized, const unsigned char *aux_rand, const unsigned char *msg32, const unsigned char *expected_sig) {
    unsigned char sig[64];
    secp256k1_keypair keypair;
    secp256k1_xonly_pubkey pk, pk_expected;

    CHECK(secp256k1_keypair_create(CTX, &keypair, sk));
    CHECK(secp256k1_schnorrsig_sign32(CTX, sig, msg32, &keypair, aux_rand));
    CHECK(secp256k1_memcmp_var(sig, expected_sig, 64) == 0);

    CHECK(secp256k1_xonly_pubkey_parse(CTX, &pk_expected, pk_serialized));
    CHECK(secp256k1_keypair_xonly_pub(CTX, &pk, NULL, &keypair));
    CHECK(secp256k1_memcmp_var(&pk, &pk_expected, sizeof(pk)) == 0);
    CHECK(secp256k1_schnorrsig_verify(CTX, sig, msg32, 32, &pk));
}

/* Helper function for schnorrsig_bip_vectors
 * Checks that both verify and verify_batch (TODO) return the same value as expected. */
static void test_schnorrsig_bip_vectors_check_verify(const unsigned char *pk_serialized, const unsigned char *msg32, const unsigned char *sig, int expected) {
    secp256k1_xonly_pubkey pk;

    CHECK(secp256k1_xonly_pubkey_parse(CTX, &pk, pk_serialized));
    CHECK(expected == secp256k1_schnorrsig_verify(CTX, sig, msg32, 32, &pk));
}

/* Test vectors according to BIP-340 ("Schnorr Signatures for secp256k1"). See
 * https://github.com/bitcoin/bips/blob/master/bip-0340/test-vectors.csv. */
static void test_schnorrsig_bip_vectors(void) {
    {
        /* Test vector 0 */
        const unsigned char sk[32] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03
        };
        const unsigned char pk[32] = {
            0xF9, 0x30, 0x8A, 0x01, 0x92, 0x58, 0xC3, 0x10,
            0x49, 0x34, 0x4F, 0x85, 0xF8, 0x9D, 0x52, 0x29,
            0xB5, 0x31, 0xC8, 0x45, 0x83, 0x6F, 0x99, 0xB0,
            0x86, 0x01, 0xF1, 0x13, 0xBC, 0xE0, 0x36, 0xF9
        };
        unsigned char aux_rand[32] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        const unsigned char msg[32] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        const unsigned char sig[64] = {
            0xE9, 0x07, 0x83, 0x1F, 0x80, 0x84, 0x8D, 0x10,
            0x69, 0xA5, 0x37, 0x1B, 0x40, 0x24, 0x10, 0x36,
            0x4B, 0xDF, 0x1C, 0x5F, 0x83, 0x07, 0xB0, 0x08,
            0x4C, 0x55, 0xF1, 0xCE, 0x2D, 0xCA, 0x82, 0x15,
            0x25, 0xF6, 0x6A, 0x4A, 0x85, 0xEA, 0x8B, 0x71,
            0xE4, 0x82, 0xA7, 0x4F, 0x38, 0x2D, 0x2C, 0xE5,
            0xEB, 0xEE, 0xE8, 0xFD, 0xB2, 0x17, 0x2F, 0x47,
            0x7D, 0xF4, 0x90, 0x0D, 0x31, 0x05, 0x36, 0xC0
        };
        test_schnorrsig_bip_vectors_check_signing(sk, pk, aux_rand, msg, sig);
        test_schnorrsig_bip_vectors_check_verify(pk, msg, sig, 1);
    }
    {
        /* Test vector 1 */
        const unsigned char sk[32] = {
            0xB7, 0xE1, 0x51, 0x62, 0x8A, 0xED, 0x2A, 0x6A,
            0xBF, 0x71, 0x58, 0x80, 0x9C, 0xF4, 0xF3, 0xC7,
            0x62, 0xE7, 0x16, 0x0F, 0x38, 0xB4, 0xDA, 0x56,
            0xA7, 0x84, 0xD9, 0x04, 0x51, 0x90, 0xCF, 0xEF
        };
        const unsigned char pk[32] = {
            0xDF, 0xF1, 0xD7, 0x7F, 0x2A, 0x67, 0x1C, 0x5F,
            0x36, 0x18, 0x37, 0x26, 0xDB, 0x23, 0x41, 0xBE,
            0x58, 0xFE, 0xAE, 0x1D, 0xA2, 0xDE, 0xCE, 0xD8,
            0x43, 0x24, 0x0F, 0x7B, 0x50, 0x2B, 0xA6, 0x59
        };
        unsigned char aux_rand[32] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01
        };
        const unsigned char msg[32] = {
            0x24, 0x3F, 0x6A, 0x88, 0x85, 0xA3, 0x08, 0xD3,
            0x13, 0x19, 0x8A, 0x2E, 0x03, 0x70, 0x73, 0x44,
            0xA4, 0x09, 0x38, 0x22, 0x29, 0x9F, 0x31, 0xD0,
            0x08, 0x2E, 0xFA, 0x98, 0xEC, 0x4E, 0x6C, 0x89
        };
        const unsigned char sig[64] = {
            0x68, 0x96, 0xBD, 0x60, 0xEE, 0xAE, 0x29, 0x6D,
            0xB4, 0x8A, 0x22, 0x9F, 0xF7, 0x1D, 0xFE, 0x07,
            0x1B, 0xDE, 0x41, 0x3E, 0x6D, 0x43, 0xF9, 0x17,
            0xDC, 0x8D, 0xCF, 0x8C, 0x78, 0xDE, 0x33, 0x41,
            0x89, 0x06, 0xD1, 0x1A, 0xC9, 0x76, 0xAB, 0xCC,
            0xB2, 0x0B, 0x09, 0x12, 0x92, 0xBF, 0xF4, 0xEA,
            0x89, 0x7E, 0xFC, 0xB6, 0x39, 0xEA, 0x87, 0x1C,
            0xFA, 0x95, 0xF6, 0xDE, 0x33, 0x9E, 0x4B, 0x0A
        };
        test_schnorrsig_bip_vectors_check_signing(sk, pk, aux_rand, msg, sig);
        test_schnorrsig_bip_vectors_check_verify(pk, msg, sig, 1);
    }
    {
        /* Test vector 2 */
        const unsigned char sk[32] = {
            0xC9, 0x0F, 0xDA, 0xA2, 0x21, 0x68, 0xC2, 0x34,
            0xC4, 0xC6, 0x62, 0x8B, 0x80, 0xDC, 0x1C, 0xD1,
            0x29, 0x02, 0x4E, 0x08, 0x8A, 0x67, 0xCC, 0x74,
            0x02, 0x0B, 0xBE, 0xA6, 0x3B, 0x14, 0xE5, 0xC9
        };
        const unsigned char pk[32] = {
            0xDD, 0x30, 0x8A, 0xFE, 0xC5, 0x77, 0x7E, 0x13,
            0x12, 0x1F, 0xA7, 0x2B, 0x9C, 0xC1, 0xB7, 0xCC,
            0x01, 0x39, 0x71, 0x53, 0x09, 0xB0, 0x86, 0xC9,
            0x60, 0xE1, 0x8F, 0xD9, 0x69, 0x77, 0x4E, 0xB8
        };
        unsigned char aux_rand[32] = {
            0xC8, 0x7A, 0xA5, 0x38, 0x24, 0xB4, 0xD7, 0xAE,
            0x2E, 0xB0, 0x35, 0xA2, 0xB5, 0xBB, 0xBC, 0xCC,
            0x08, 0x0E, 0x76, 0xCD, 0xC6, 0xD1, 0x69, 0x2C,
            0x4B, 0x0B, 0x62, 0xD7, 0x98, 0xE6, 0xD9, 0x06
        };
        const unsigned char msg[32] = {
            0x7E, 0x2D, 0x58, 0xD8, 0xB3, 0xBC, 0xDF, 0x1A,
            0xBA, 0xDE, 0xC7, 0x82, 0x90, 0x54, 0xF9, 0x0D,
            0xDA, 0x98, 0x05, 0xAA, 0xB5, 0x6C, 0x77, 0x33,
            0x30, 0x24, 0xB9, 0xD0, 0xA5, 0x08, 0xB7, 0x5C
        };
        const unsigned char sig[64] = {
            0x58, 0x31, 0xAA, 0xEE, 0xD7, 0xB4, 0x4B, 0xB7,
            0x4E, 0x5E, 0xAB, 0x94, 0xBA, 0x9D, 0x42, 0x94,
            0xC4, 0x9B, 0xCF, 0x2A, 0x60, 0x72, 0x8D, 0x8B,
            0x4C, 0x20, 0x0F, 0x50, 0xDD, 0x31, 0x3C, 0x1B,
            0xAB, 0x74, 0x58, 0x79, 0xA5, 0xAD, 0x95, 0x4A,
            0x72, 0xC4, 0x5A, 0x91, 0xC3, 0xA5, 0x1D, 0x3C,
            0x7A, 0xDE, 0xA9, 0x8D, 0x82, 0xF8, 0x48, 0x1E,
            0x0E, 0x1E, 0x03, 0x67, 0x4A, 0x6F, 0x3F, 0xB7
        };
        test_schnorrsig_bip_vectors_check_signing(sk, pk, aux_rand, msg, sig);
        test_schnorrsig_bip_vectors_check_verify(pk, msg, sig, 1);
    }
    {
        /* Test vector 3 */
        const unsigned char sk[32] = {
            0x0B, 0x43, 0x2B, 0x26, 0x77, 0x93, 0x73, 0x81,
            0xAE, 0xF0, 0x5B, 0xB0, 0x2A, 0x66, 0xEC, 0xD0,
            0x12, 0x77, 0x30, 0x62, 0xCF, 0x3F, 0xA2, 0x54,
            0x9E, 0x44, 0xF5, 0x8E, 0xD2, 0x40, 0x17, 0x10
        };
        const unsigned char pk[32] = {
            0x25, 0xD1, 0xDF, 0xF9, 0x51, 0x05, 0xF5, 0x25,
            0x3C, 0x40, 0x22, 0xF6, 0x28, 0xA9, 0x96, 0xAD,
            0x3A, 0x0D, 0x95, 0xFB, 0xF2, 0x1D, 0x46, 0x8A,
            0x1B, 0x33, 0xF8, 0xC1, 0x60, 0xD8, 0xF5, 0x17
        };
        unsigned char aux_rand[32] = {
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
        };
        const unsigned char msg[32] = {
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
        };
        const unsigned char sig[64] = {
            0x7E, 0xB0, 0x50, 0x97, 0x57, 0xE2, 0x46, 0xF1,
            0x94, 0x49, 0x88, 0x56, 0x51, 0x61, 0x1C, 0xB9,
            0x65, 0xEC, 0xC1, 0xA1, 0x87, 0xDD, 0x51, 0xB6,
            0x4F, 0xDA, 0x1E, 0xDC, 0x96, 0x37, 0xD5, 0xEC,
            0x97, 0x58, 0x2B, 0x9C, 0xB1, 0x3D, 0xB3, 0x93,
            0x37, 0x05, 0xB3, 0x2B, 0xA9, 0x82, 0xAF, 0x5A,
            0xF2, 0x5F, 0xD7, 0x88, 0x81, 0xEB, 0xB3, 0x27,
            0x71, 0xFC, 0x59, 0x22, 0xEF, 0xC6, 0x6E, 0xA3
        };
        test_schnorrsig_bip_vectors_check_signing(sk, pk, aux_rand, msg, sig);
        test_schnorrsig_bip_vectors_check_verify(pk, msg, sig, 1);
    }
    {
        /* Test vector 4 */
        const unsigned char pk[32] = {
            0xD6, 0x9C, 0x35, 0x09, 0xBB, 0x99, 0xE4, 0x12,
            0xE6, 0x8B, 0x0F, 0xE8, 0x54, 0x4E, 0x72, 0x83,
            0x7D, 0xFA, 0x30, 0x74, 0x6D, 0x8B, 0xE2, 0xAA,
            0x65, 0x97, 0x5F, 0x29, 0xD2, 0x2D, 0xC7, 0xB9
        };
        const unsigned char msg[32] = {
            0x4D, 0xF3, 0xC3, 0xF6, 0x8F, 0xCC, 0x83, 0xB2,
            0x7E, 0x9D, 0x42, 0xC9, 0x04, 0x31, 0xA7, 0x24,
            0x99, 0xF1, 0x78, 0x75, 0xC8, 0x1A, 0x59, 0x9B,
            0x56, 0x6C, 0x98, 0x89, 0xB9, 0x69, 0x67, 0x03
        };
        const unsigned char sig[64] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x3B, 0x78, 0xCE, 0x56, 0x3F,
            0x89, 0xA0, 0xED, 0x94, 0x14, 0xF5, 0xAA, 0x28,
            0xAD, 0x0D, 0x96, 0xD6, 0x79, 0x5F, 0x9C, 0x63,
            0x76, 0xAF, 0xB1, 0x54, 0x8A, 0xF6, 0x03, 0xB3,
            0xEB, 0x45, 0xC9, 0xF8, 0x20, 0x7D, 0xEE, 0x10,
            0x60, 0xCB, 0x71, 0xC0, 0x4E, 0x80, 0xF5, 0x93,
            0x06, 0x0B, 0x07, 0xD2, 0x83, 0x08, 0xD7, 0xF4
        };
        test_schnorrsig_bip_vectors_check_verify(pk, msg, sig, 1);
    }
    {
        /* Test vector 5 */
        const unsigned char pk[32] = {
            0xEE, 0xFD, 0xEA, 0x4C, 0xDB, 0x67, 0x77, 0x50,
            0xA4, 0x20, 0xFE, 0xE8, 0x07, 0xEA, 0xCF, 0x21,
            0xEB, 0x98, 0x98, 0xAE, 0x79, 0xB9, 0x76, 0x87,
            0x66, 0xE4, 0xFA, 0xA0, 0x4A, 0x2D, 0x4A, 0x34
        };
        secp256k1_xonly_pubkey pk_parsed;
        /* No need to check the signature of the test vector as parsing the pubkey already fails */
        CHECK(!secp256k1_xonly_pubkey_parse(CTX, &pk_parsed, pk));
    }
    {
        /* Test vector 6 */
        const unsigned char pk[32] = {
            0xDF, 0xF1, 0xD7, 0x7F, 0x2A, 0x67, 0x1C, 0x5F,
            0x36, 0x18, 0x37, 0x26, 0xDB, 0x23, 0x41, 0xBE,
            0x58, 0xFE, 0xAE, 0x1D, 0xA2, 0xDE, 0xCE, 0xD8,
            0x43, 0x24, 0x0F, 0x7B, 0x50, 0x2B, 0xA6, 0x59
        };
        const unsigned char msg[32] = {
            0x24, 0x3F, 0x6A, 0x88, 0x85, 0xA3, 0x08, 0xD3,
            0x13, 0x19, 0x8A, 0x2E, 0x03, 0x70, 0x73, 0x44,
            0xA4, 0x09, 0x38, 0x22, 0x29, 0x9F, 0x31, 0xD0,
            0x08, 0x2E, 0xFA, 0x98, 0xEC, 0x4E, 0x6C, 0x89
        };
        const unsigned char sig[64] = {
            0xFF, 0xF9, 0x7B, 0xD5, 0x75, 0x5E, 0xEE, 0xA4,
            0x20, 0x45, 0x3A, 0x14, 0x35, 0x52, 0x35, 0xD3,
            0x82, 0xF6, 0x47, 0x2F, 0x85, 0x68, 0xA1, 0x8B,
            0x2F, 0x05, 0x7A, 0x14, 0x60, 0x29, 0x75, 0x56,
            0x3C, 0xC2, 0x79, 0x44, 0x64, 0x0A, 0xC6, 0x07,
            0xCD, 0x10, 0x7A, 0xE1, 0x09, 0x23, 0xD9, 0xEF,
            0x7A, 0x73, 0xC6, 0x43, 0xE1, 0x66, 0xBE, 0x5E,
            0xBE, 0xAF, 0xA3, 0x4B, 0x1A, 0xC5, 0x53, 0xE2
        };
        test_schnorrsig_bip_vectors_check_verify(pk, msg, sig, 0);
    }
    {
        /* Test vector 7 */
        const unsigned char pk[32] = {
            0xDF, 0xF1, 0xD7, 0x7F, 0x2A, 0x67, 0x1C, 0x5F,
            0x36, 0x18, 0x37, 0x26, 0xDB, 0x23, 0x41, 0xBE,
            0x58, 0xFE, 0xAE, 0x1D, 0xA2, 0xDE, 0xCE, 0xD8,
            0x43, 0x24, 0x0F, 0x7B, 0x50, 0x2B, 0xA6, 0x59
        };
        const unsigned char msg[32] = {
            0x24, 0x3F, 0x6A, 0x88, 0x85, 0xA3, 0x08, 0xD3,
            0x13, 0x19, 0x8A, 0x2E, 0x03, 0x70, 0x73, 0x44,
            0xA4, 0x09, 0x38, 0x22, 0x29, 0x9F, 0x31, 0xD0,
            0x08, 0x2E, 0xFA, 0x98, 0xEC, 0x4E, 0x6C, 0x89
        };
        const unsigned char sig[64] = {
            0x1F, 0xA6, 0x2E, 0x33, 0x1E, 0xDB, 0xC2, 0x1C,
            0x39, 0x47, 0x92, 0xD2, 0xAB, 0x11, 0x00, 0xA7,
            0xB4, 0x32, 0xB0, 0x13, 0xDF, 0x3F, 0x6F, 0xF4,
            0xF9, 0x9F, 0xCB, 0x33, 0xE0, 0xE1, 0x51, 0x5F,
            0x28, 0x89, 0x0B, 0x3E, 0xDB, 0x6E, 0x71, 0x89,
            0xB6, 0x30, 0x44, 0x8B, 0x51, 0x5C, 0xE4, 0xF8,
            0x62, 0x2A, 0x95, 0x4C, 0xFE, 0x54, 0x57, 0x35,
            0xAA, 0xEA, 0x51, 0x34, 0xFC, 0xCD, 0xB2, 0xBD
        };
        test_schnorrsig_bip_vectors_check_verify(pk, msg, sig, 0);
    }
    {
        /* Test vector 8 */
        const unsigned char pk[32] = {
            0xDF, 0xF1, 0xD7, 0x7F, 0x2A, 0x67, 0x1C, 0x5F,
            0x36, 0x18, 0x37, 0x26, 0xDB, 0x23, 0x41, 0xBE,
            0x58, 0xFE, 0xAE, 0x1D, 0xA2, 0xDE, 0xCE, 0xD8,
            0x43, 0x24, 0x0F, 0x7B, 0x50, 0x2B, 0xA6, 0x59
        };
        const unsigned char msg[32] = {
            0x24, 0x3F, 0x6A, 0x88, 0x85, 0xA3, 0x08, 0xD3,
            0x13, 0x19, 0x8A, 0x2E, 0x03, 0x70, 0x73, 0x44,
            0xA4, 0x09, 0x38, 0x22, 0x29, 0x9F, 0x31, 0xD0,
            0x08, 0x2E, 0xFA, 0x98, 0xEC, 0x4E, 0x6C, 0x89
        };
        const unsigned char sig[64] = {
            0x6C, 0xFF, 0x5C, 0x3B, 0xA8, 0x6C, 0x69, 0xEA,
            0x4B, 0x73, 0x76, 0xF3, 0x1A, 0x9B, 0xCB, 0x4F,
            0x74, 0xC1, 0x97, 0x60, 0x89, 0xB2, 0xD9, 0x96,
            0x3D, 0xA2, 0xE5, 0x54, 0x3E, 0x17, 0x77, 0x69,
            0x96, 0x17, 0x64, 0xB3, 0xAA, 0x9B, 0x2F, 0xFC,
            0xB6, 0xEF, 0x94, 0x7B, 0x68, 0x87, 0xA2, 0x26,
            0xE8, 0xD7, 0xC9, 0x3E, 0x00, 0xC5, 0xED, 0x0C,
            0x18, 0x34, 0xFF, 0x0D, 0x0C, 0x2E, 0x6D, 0xA6
        };
        test_schnorrsig_bip_vectors_check_verify(pk, msg, sig, 0);
    }
    {
        /* Test vector 9 */
        const unsigned char pk[32] = {
            0xDF, 0xF1, 0xD7, 0x7F, 0x2A, 0x67, 0x1C, 0x5F,
            0x36, 0x18, 0x37, 0x26, 0xDB, 0x23, 0x41, 0xBE,
            0x58, 0xFE, 0xAE, 0x1D, 0xA2, 0xDE, 0xCE, 0xD8,
            0x43, 0x24, 0x0F, 0x7B, 0x50, 0x2B, 0xA6, 0x59
        };
        const unsigned char msg[32] = {
            0x24, 0x3F, 0x6A, 0x88, 0x85, 0xA3, 0x08, 0xD3,
            0x13, 0x19, 0x8A, 0x2E, 0x03, 0x70, 0x73, 0x44,
            0xA4, 0x09, 0x38, 0x22, 0x29, 0x9F, 0x31, 0xD0,
            0x08, 0x2E, 0xFA, 0x98, 0xEC, 0x4E, 0x6C, 0x89
        };
        const unsigned char sig[64] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x12, 0x3D, 0xDA, 0x83, 0x28, 0xAF, 0x9C, 0x23,
            0xA9, 0x4C, 0x1F, 0xEE, 0xCF, 0xD1, 0x23, 0xBA,
            0x4F, 0xB7, 0x34, 0x76, 0xF0, 0xD5, 0x94, 0xDC,
            0xB6, 0x5C, 0x64, 0x25, 0xBD, 0x18, 0x60, 0x51
        };
        test_schnorrsig_bip_vectors_check_verify(pk, msg, sig, 0);
    }
    {
        /* Test vector 10 */
        const unsigned char pk[32] = {
            0xDF, 0xF1, 0xD7, 0x7F, 0x2A, 0x67, 0x1C, 0x5F,
            0x36, 0x18, 0x37, 0x26, 0xDB, 0x23, 0x41, 0xBE,
            0x58, 0xFE, 0xAE, 0x1D, 0xA2, 0xDE, 0xCE, 0xD8,
            0x43, 0x24, 0x0F, 0x7B, 0x50, 0x2B, 0xA6, 0x59
        };
        const unsigned char msg[32] = {
            0x24, 0x3F, 0x6A, 0x88, 0x85, 0xA3, 0x08, 0xD3,
            0x13, 0x19, 0x8A, 0x2E, 0x03, 0x70, 0x73, 0x44,
            0xA4, 0x09, 0x38, 0x22, 0x29, 0x9F, 0x31, 0xD0,
            0x08, 0x2E, 0xFA, 0x98, 0xEC, 0x4E, 0x6C, 0x89
        };
        const unsigned char sig[64] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
            0x76, 0x15, 0xFB, 0xAF, 0x5A, 0xE2, 0x88, 0x64,
            0x01, 0x3C, 0x09, 0x97, 0x42, 0xDE, 0xAD, 0xB4,
            0xDB, 0xA8, 0x7F, 0x11, 0xAC, 0x67, 0x54, 0xF9,
            0x37, 0x80, 0xD5, 0xA1, 0x83, 0x7C, 0xF1, 0x97
        };
        test_schnorrsig_bip_vectors_check_verify(pk, msg, sig, 0);
    }
    {
        /* Test vector 11 */
        const unsigned char pk[32] = {
            0xDF, 0xF1, 0xD7, 0x7F, 0x2A, 0x67, 0x1C, 0x5F,
            0x36, 0x18, 0x37, 0x26, 0xDB, 0x23, 0x41, 0xBE,
            0x58, 0xFE, 0xAE, 0x1D, 0xA2, 0xDE, 0xCE, 0xD8,
            0x43, 0x24, 0x0F, 0x7B, 0x50, 0x2B, 0xA6, 0x59
        };
        const unsigned char msg[32] = {
            0x24, 0x3F, 0x6A, 0x88, 0x85, 0xA3, 0x08, 0xD3,
            0x13, 0x19, 0x8A, 0x2E, 0x03, 0x70, 0x73, 0x44,
            0xA4, 0x09, 0x38, 0x22, 0x29, 0x9F, 0x31, 0xD0,
            0x08, 0x2E, 0xFA, 0x98, 0xEC, 0x4E, 0x6C, 0x89
        };
        const unsigned char sig[64] = {
            0x4A, 0x29, 0x8D, 0xAC, 0xAE, 0x57, 0x39, 0x5A,
            0x15, 0xD0, 0x79, 0x5D, 0xDB, 0xFD, 0x1D, 0xCB,
            0x56, 0x4D, 0xA8, 0x2B, 0x0F, 0x26, 0x9B, 0xC7,
            0x0A, 0x74, 0xF8, 0x22, 0x04, 0x29, 0xBA, 0x1D,
            0x69, 0xE8, 0x9B, 0x4C, 0x55, 0x64, 0xD0, 0x03,
            0x49, 0x10, 0x6B, 0x84, 0x97, 0x78, 0x5D, 0xD7,
            0xD1, 0xD7, 0x13, 0xA8, 0xAE, 0x82, 0xB3, 0x2F,
            0xA7, 0x9D, 0x5F, 0x7F, 0xC4, 0x07, 0xD3, 0x9B
        };
        test_schnorrsig_bip_vectors_check_verify(pk, msg, sig, 0);
    }
    {
        /* Test vector 12 */
        const unsigned char pk[32] = {
            0xDF, 0xF1, 0xD7, 0x7F, 0x2A, 0x67, 0x1C, 0x5F,
            0x36, 0x18, 0x37, 0x26, 0xDB, 0x23, 0x41, 0xBE,
            0x58, 0xFE, 0xAE, 0x1D, 0xA2, 0xDE, 0xCE, 0xD8,
            0x43, 0x24, 0x0F, 0x7B, 0x50, 0x2B, 0xA6, 0x59
        };
        const unsigned char msg[32] = {
            0x24, 0x3F, 0x6A, 0x88, 0x85, 0xA3, 0x08, 0xD3,
            0x13, 0x19, 0x8A, 0x2E, 0x03, 0x70, 0x73, 0x44,
            0xA4, 0x09, 0x38, 0x22, 0x29, 0x9F, 0x31, 0xD0,
            0x08, 0x2E, 0xFA, 0x98, 0xEC, 0x4E, 0x6C, 0x89
        };
        const unsigned char sig[64] = {
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFE, 0xFF, 0xFF, 0xFC, 0x2F,
            0x69, 0xE8, 0x9B, 0x4C, 0x55, 0x64, 0xD0, 0x03,
            0x49, 0x10, 0x6B, 0x84, 0x97, 0x78, 0x5D, 0xD7,
            0xD1, 0xD7, 0x13, 0xA8, 0xAE, 0x82, 0xB3, 0x2F,
            0xA7, 0x9D, 0x5F, 0x7F, 0xC4, 0x07, 0xD3, 0x9B
        };
        test_schnorrsig_bip_vectors_check_verify(pk, msg, sig, 0);
    }
    {
        /* Test vector 13 */
        const unsigned char pk[32] = {
            0xDF, 0xF1, 0xD7, 0x7F, 0x2A, 0x67, 0x1C, 0x5F,
            0x36, 0x18, 0x37, 0x26, 0xDB, 0x23, 0x41, 0xBE,
            0x58, 0xFE, 0xAE, 0x1D, 0xA2, 0xDE, 0xCE, 0xD8,
            0x43, 0x24, 0x0F, 0x7B, 0x50, 0x2B, 0xA6, 0x59
        };
        const unsigned char msg[32] = {
            0x24, 0x3F, 0x6A, 0x88, 0x85, 0xA3, 0x08, 0xD3,
            0x13, 0x19, 0x8A, 0x2E, 0x03, 0x70, 0x73, 0x44,
            0xA4, 0x09, 0x38, 0x22, 0x29, 0x9F, 0x31, 0xD0,
            0x08, 0x2E, 0xFA, 0x98, 0xEC, 0x4E, 0x6C, 0x89
        };
        const unsigned char sig[64] = {
            0x6C, 0xFF, 0x5C, 0x3B, 0xA8, 0x6C, 0x69, 0xEA,
            0x4B, 0x73, 0x76, 0xF3, 0x1A, 0x9B, 0xCB, 0x4F,
            0x74, 0xC1, 0x97, 0x60, 0x89, 0xB2, 0xD9, 0x96,
            0x3D, 0xA2, 0xE5, 0x54, 0x3E, 0x17, 0x77, 0x69,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE,
            0xBA, 0xAE, 0xDC, 0xE6, 0xAF, 0x48, 0xA0, 0x3B,
            0xBF, 0xD2, 0x5E, 0x8C, 0xD0, 0x36, 0x41, 0x41
        };
        test_schnorrsig_bip_vectors_check_verify(pk, msg, sig, 0);
    }
    {
        /* Test vector 14 */
        const unsigned char pk[32] = {
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFE, 0xFF, 0xFF, 0xFC, 0x30
        };
        secp256k1_xonly_pubkey pk_parsed;
        /* No need to check the signature of the test vector as parsing the pubkey already fails */
        CHECK(!secp256k1_xonly_pubkey_parse(CTX, &pk_parsed, pk));
    }
}

/* Nonce function that returns constant 0 */
static int nonce_function_failing(unsigned char *nonce32, const unsigned char *msg, size_t msglen, const unsigned char *key32, const unsigned char *xonly_pk32, const unsigned char *algo, size_t algolen, void *data) {
    (void) msg;
    (void) msglen;
    (void) key32;
    (void) xonly_pk32;
    (void) algo;
    (void) algolen;
    (void) data;
    (void) nonce32;
    return 0;
}

/* Nonce function that sets nonce to 0 */
static int nonce_function_0(unsigned char *nonce32, const unsigned char *msg, size_t msglen, const unsigned char *key32, const unsigned char *xonly_pk32, const unsigned char *algo, size_t algolen, void *data) {
    (void) msg;
    (void) msglen;
    (void) key32;
    (void) xonly_pk32;
    (void) algo;
    (void) algolen;
    (void) data;

    memset(nonce32, 0, 32);
    return 1;
}

/* Nonce function that sets nonce to 0xFF...0xFF */
static int nonce_function_overflowing(unsigned char *nonce32, const unsigned char *msg, size_t msglen, const unsigned char *key32, const unsigned char *xonly_pk32, const unsigned char *algo, size_t algolen, void *data) {
    (void) msg;
    (void) msglen;
    (void) key32;
    (void) xonly_pk32;
    (void) algo;
    (void) algolen;
    (void) data;

    memset(nonce32, 0xFF, 32);
    return 1;
}

static void test_schnorrsig_sign(void) {
    unsigned char sk[32];
    secp256k1_xonly_pubkey pk;
    secp256k1_keypair keypair;
    const unsigned char msg[32] = "this is a msg for a schnorrsig..";
    unsigned char sig[64];
    unsigned char sig2[64];
    unsigned char zeros64[64] = { 0 };
    secp256k1_schnorrsig_extraparams extraparams = SECP256K1_SCHNORRSIG_EXTRAPARAMS_INIT;
    unsigned char aux_rand[32];

    secp256k1_testrand256(sk);
    secp256k1_testrand256(aux_rand);
    CHECK(secp256k1_keypair_create(CTX, &keypair, sk));
    CHECK(secp256k1_keypair_xonly_pub(CTX, &pk, NULL, &keypair));
    CHECK(secp256k1_schnorrsig_sign32(CTX, sig, msg, &keypair, NULL) == 1);
    CHECK(secp256k1_schnorrsig_verify(CTX, sig, msg, sizeof(msg), &pk));
    /* Check that deprecated alias gives the same result */
    CHECK(secp256k1_schnorrsig_sign(CTX, sig2, msg, &keypair, NULL) == 1);
    CHECK(secp256k1_memcmp_var(sig, sig2, sizeof(sig)) == 0);

    /* Test different nonce functions */
    CHECK(secp256k1_schnorrsig_sign_custom(CTX, sig, msg, sizeof(msg), &keypair, &extraparams) == 1);
    CHECK(secp256k1_schnorrsig_verify(CTX, sig, msg, sizeof(msg), &pk));
    memset(sig, 1, sizeof(sig));
    extraparams.noncefp = nonce_function_failing;
    CHECK(secp256k1_schnorrsig_sign_custom(CTX, sig, msg, sizeof(msg), &keypair, &extraparams) == 0);
    CHECK(secp256k1_memcmp_var(sig, zeros64, sizeof(sig)) == 0);
    memset(&sig, 1, sizeof(sig));
    extraparams.noncefp = nonce_function_0;
    CHECK(secp256k1_schnorrsig_sign_custom(CTX, sig, msg, sizeof(msg), &keypair, &extraparams) == 0);
    CHECK(secp256k1_memcmp_var(sig, zeros64, sizeof(sig)) == 0);
    memset(&sig, 1, sizeof(sig));
    extraparams.noncefp = nonce_function_overflowing;
    CHECK(secp256k1_schnorrsig_sign_custom(CTX, sig, msg, sizeof(msg), &keypair, &extraparams) == 1);
    CHECK(secp256k1_schnorrsig_verify(CTX, sig, msg, sizeof(msg), &pk));

    /* When using the default nonce function, schnorrsig_sign_custom produces
     * the same result as schnorrsig_sign with aux_rand = extraparams.ndata */
    extraparams.noncefp = NULL;
    extraparams.ndata = aux_rand;
    CHECK(secp256k1_schnorrsig_sign_custom(CTX, sig, msg, sizeof(msg), &keypair, &extraparams) == 1);
    CHECK(secp256k1_schnorrsig_sign32(CTX, sig2, msg, &keypair, extraparams.ndata) == 1);
    CHECK(secp256k1_memcmp_var(sig, sig2, sizeof(sig)) == 0);
}

#define N_SIGS 3
/* Creates N_SIGS valid signatures and verifies them with verify and
 * verify_batch (TODO). Then flips some bits and checks that verification now
 * fails. */
static void test_schnorrsig_sign_verify(void) {
    unsigned char sk[32];
    unsigned char msg[N_SIGS][32];
    unsigned char sig[N_SIGS][64];
    size_t i;
    secp256k1_keypair keypair;
    secp256k1_xonly_pubkey pk;
    secp256k1_scalar s;

    secp256k1_testrand256(sk);
    CHECK(secp256k1_keypair_create(CTX, &keypair, sk));
    CHECK(secp256k1_keypair_xonly_pub(CTX, &pk, NULL, &keypair));

    for (i = 0; i < N_SIGS; i++) {
        secp256k1_testrand256(msg[i]);
        CHECK(secp256k1_schnorrsig_sign32(CTX, sig[i], msg[i], &keypair, NULL));
        CHECK(secp256k1_schnorrsig_verify(CTX, sig[i], msg[i], sizeof(msg[i]), &pk));
    }

    {
        /* Flip a few bits in the signature and in the message and check that
         * verify and verify_batch (TODO) fail */
        size_t sig_idx = secp256k1_testrand_int(N_SIGS);
        size_t byte_idx = secp256k1_testrand_bits(5);
        unsigned char xorbyte = secp256k1_testrand_int(254)+1;
        sig[sig_idx][byte_idx] ^= xorbyte;
        CHECK(!secp256k1_schnorrsig_verify(CTX, sig[sig_idx], msg[sig_idx], sizeof(msg[sig_idx]), &pk));
        sig[sig_idx][byte_idx] ^= xorbyte;

        byte_idx = secp256k1_testrand_bits(5);
        sig[sig_idx][32+byte_idx] ^= xorbyte;
        CHECK(!secp256k1_schnorrsig_verify(CTX, sig[sig_idx], msg[sig_idx], sizeof(msg[sig_idx]), &pk));
        sig[sig_idx][32+byte_idx] ^= xorbyte;

        byte_idx = secp256k1_testrand_bits(5);
        msg[sig_idx][byte_idx] ^= xorbyte;
        CHECK(!secp256k1_schnorrsig_verify(CTX, sig[sig_idx], msg[sig_idx], sizeof(msg[sig_idx]), &pk));
        msg[sig_idx][byte_idx] ^= xorbyte;

        /* Check that above bitflips have been reversed correctly */
        CHECK(secp256k1_schnorrsig_verify(CTX, sig[sig_idx], msg[sig_idx], sizeof(msg[sig_idx]), &pk));
    }

    /* Test overflowing s */
    CHECK(secp256k1_schnorrsig_sign32(CTX, sig[0], msg[0], &keypair, NULL));
    CHECK(secp256k1_schnorrsig_verify(CTX, sig[0], msg[0], sizeof(msg[0]), &pk));
    memset(&sig[0][32], 0xFF, 32);
    CHECK(!secp256k1_schnorrsig_verify(CTX, sig[0], msg[0], sizeof(msg[0]), &pk));

    /* Test negative s */
    CHECK(secp256k1_schnorrsig_sign32(CTX, sig[0], msg[0], &keypair, NULL));
    CHECK(secp256k1_schnorrsig_verify(CTX, sig[0], msg[0], sizeof(msg[0]), &pk));
    secp256k1_scalar_set_b32(&s, &sig[0][32], NULL);
    secp256k1_scalar_negate(&s, &s);
    secp256k1_scalar_get_b32(&sig[0][32], &s);
    CHECK(!secp256k1_schnorrsig_verify(CTX, sig[0], msg[0], sizeof(msg[0]), &pk));

    /* The empty message can be signed & verified */
    CHECK(secp256k1_schnorrsig_sign_custom(CTX, sig[0], NULL, 0, &keypair, NULL) == 1);
    CHECK(secp256k1_schnorrsig_verify(CTX, sig[0], NULL, 0, &pk) == 1);

    {
        /* Test varying message lengths */
        unsigned char msg_large[32 * 8];
        uint32_t msglen  = secp256k1_testrand_int(sizeof(msg_large));
        for (i = 0; i < sizeof(msg_large); i += 32) {
            secp256k1_testrand256(&msg_large[i]);
        }
        CHECK(secp256k1_schnorrsig_sign_custom(CTX, sig[0], msg_large, msglen, &keypair, NULL) == 1);
        CHECK(secp256k1_schnorrsig_verify(CTX, sig[0], msg_large, msglen, &pk) == 1);
        /* Verification for a random wrong message length fails */
        msglen = (msglen + (sizeof(msg_large) - 1)) % sizeof(msg_large);
        CHECK(secp256k1_schnorrsig_verify(CTX, sig[0], msg_large, msglen, &pk) == 0);
    }
}
#undef N_SIGS

static void test_schnorrsig_taproot(void) {
    unsigned char sk[32];
    secp256k1_keypair keypair;
    secp256k1_xonly_pubkey internal_pk;
    unsigned char internal_pk_bytes[32];
    secp256k1_xonly_pubkey output_pk;
    unsigned char output_pk_bytes[32];
    unsigned char tweak[32];
    int pk_parity;
    unsigned char msg[32];
    unsigned char sig[64];

    /* Create output key */
    secp256k1_testrand256(sk);
    CHECK(secp256k1_keypair_create(CTX, &keypair, sk) == 1);
    CHECK(secp256k1_keypair_xonly_pub(CTX, &internal_pk, NULL, &keypair) == 1);
    /* In actual taproot the tweak would be hash of internal_pk */
    CHECK(secp256k1_xonly_pubkey_serialize(CTX, tweak, &internal_pk) == 1);
    CHECK(secp256k1_keypair_xonly_tweak_add(CTX, &keypair, tweak) == 1);
    CHECK(secp256k1_keypair_xonly_pub(CTX, &output_pk, &pk_parity, &keypair) == 1);
    CHECK(secp256k1_xonly_pubkey_serialize(CTX, output_pk_bytes, &output_pk) == 1);

    /* Key spend */
    secp256k1_testrand256(msg);
    CHECK(secp256k1_schnorrsig_sign32(CTX, sig, msg, &keypair, NULL) == 1);
    /* Verify key spend */
    CHECK(secp256k1_xonly_pubkey_parse(CTX, &output_pk, output_pk_bytes) == 1);
    CHECK(secp256k1_schnorrsig_verify(CTX, sig, msg, sizeof(msg), &output_pk) == 1);

    /* Script spend */
    CHECK(secp256k1_xonly_pubkey_serialize(CTX, internal_pk_bytes, &internal_pk) == 1);
    /* Verify script spend */
    CHECK(secp256k1_xonly_pubkey_parse(CTX, &internal_pk, internal_pk_bytes) == 1);
    CHECK(secp256k1_xonly_pubkey_tweak_add_check(CTX, output_pk_bytes, pk_parity, &internal_pk, tweak) == 1);
}

static void run_schnorrsig_tests(void) {
    int i;
    run_nonce_function_bip340_tests();

    test_schnorrsig_api();
    test_schnorrsig_sha256_tagged();
    test_schnorrsig_bip_vectors();
    for (i = 0; i < COUNT; i++) {
        test_schnorrsig_sign();
        test_schnorrsig_sign_verify();
    }
    test_schnorrsig_taproot();
}

#endif
