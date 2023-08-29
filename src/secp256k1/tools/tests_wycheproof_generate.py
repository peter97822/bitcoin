#!/usr/bin/env python3
# Copyright (c) 2023 Random "Randy" Lattice and Sean Andersen
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php.
'''
Generate a C file with ECDSA testvectors from the Wycheproof project.
'''

import json
import sys

filename_input = sys.argv[1]

with open(filename_input) as f:
    doc = json.load(f)

num_groups = len(doc['testGroups'])

def to_c_array(x):
    if x == "":
        return ""
    s = ',0x'.join(a+b for a,b in zip(x[::2], x[1::2]))
    return "0x" + s


num_vectors = 0
offset_msg_running, offset_pk_running, offset_sig = 0, 0, 0
out = ""
messages = ""
signatures = ""
public_keys = ""
cache_msgs = {}
cache_public_keys = {}

for i in range(num_groups):
    group = doc['testGroups'][i]
    num_tests = len(group['tests'])
    public_key = group['publicKey']
    for j in range(num_tests):
        test_vector = group['tests'][j]
        # // 2 to convert hex to byte length
        sig_size = len(test_vector['sig']) // 2
        msg_size = len(test_vector['msg']) // 2

        if test_vector['result'] == "invalid":
            expected_verify = 0
        elif test_vector['result'] == "valid":
            expected_verify = 1
        else:
            raise ValueError("invalid result field")

        if num_vectors != 0 and sig_size != 0:
            signatures += ",\n  "

        new_msg = False
        msg = to_c_array(test_vector['msg'])
        msg_offset = offset_msg_running
        # check for repeated msg
        if msg not in cache_msgs:
            if num_vectors != 0 and msg_size != 0:
                messages += ",\n  "
            cache_msgs[msg] = offset_msg_running
            messages += msg
            new_msg = True
        else:
            msg_offset = cache_msgs[msg]

        new_pk = False
        pk = to_c_array(public_key['uncompressed'])
        pk_offset = offset_pk_running
        # check for repeated pk
        if pk not in cache_public_keys:
            if num_vectors != 0:
                public_keys += ",\n  "
            cache_public_keys[pk] = offset_pk_running
            public_keys += pk
            new_pk = True
        else:
            pk_offset = cache_public_keys[pk]

        signatures += to_c_array(test_vector['sig'])

        out += "  /" + "* tcId: " + str(test_vector['tcId']) + ". " + test_vector['comment'] + " *" + "/\n"
        out += f"  {{{pk_offset}, {msg_offset}, {msg_size}, {offset_sig}, {sig_size}, {expected_verify} }},\n"
        if new_msg:
            offset_msg_running += msg_size
        if new_pk:
            offset_pk_running += 65
        offset_sig += sig_size
        num_vectors += 1

struct_definition = """
typedef struct {
    size_t pk_offset;
    size_t msg_offset;
    size_t msg_len;
    size_t sig_offset;
    size_t sig_len;
    int expected_verify;
} wycheproof_ecdsa_testvector;
"""


print("/* Note: this file was autogenerated using tests_wycheproof_generate.py. Do not edit. */")
print(f"#define SECP256K1_ECDSA_WYCHEPROOF_NUMBER_TESTVECTORS ({num_vectors})")

print(struct_definition)

print("static const unsigned char wycheproof_ecdsa_messages[]    = { " + messages + "};\n")
print("static const unsigned char wycheproof_ecdsa_public_keys[] = { " + public_keys + "};\n")
print("static const unsigned char wycheproof_ecdsa_signatures[]  = { " + signatures + "};\n")

print("static const wycheproof_ecdsa_testvector testvectors[SECP256K1_ECDSA_WYCHEPROOF_NUMBER_TESTVECTORS] = {")
print(out)
print("};")
