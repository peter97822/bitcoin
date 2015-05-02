#!/bin/bash
set -e

CURDIR=$(cd $(dirname "$0"); pwd)
# Get BUILDDIR and REAL_BITCOIND
. "${CURDIR}/tests-config.sh"

export BITCOINCLI=${BUILDDIR}/qa/pull-tester/run-bitcoin-cli
export BITCOIND=${REAL_BITCOIND}

if [ "x${EXEEXT}" = "x.exe" ]; then
  echo "Win tests currently disabled"
  exit 0
fi

#Run the tests

testScripts=(
    'wallet.py'
    'listtransactions.py'
    'mempool_resurrect_test.py'
    'txn_doublespend.py'
    'txn_doublespend.py --mineblock'
    'getchaintips.py'
    'rawtransactions.py'
    'rest.py'
    'mempool_spendcoinbase.py'
    'mempool_coinbase_spends.py'
    'httpbasics.py'
    'zapwallettxes.py'
    'proxy_test.py'
    'merkle_blocks.py'
    'signrawtransactions.py'
);
testScriptsExt=(
    'bipdersig-p2p.py'
    'bipdersig.py'
    'getblocktemplate_longpoll.py'
    'getblocktemplate_proposals.py'
    'prune.py'
    'forknotify.py'
    'invalidateblock.py'
    'keypool.py'
    'receivedby.py'
    'reindex.py'
    'rpcbind_test.py'
    'script_test.py'
    'smartfees.py'
    'maxblocksinflight.py'
    'invalidblockrequest.py'
    'rawtransactions.py'
#    'forknotify.py'
);
if [ "x${ENABLE_BITCOIND}${ENABLE_UTILS}${ENABLE_WALLET}" = "x111" ]; then
    for (( i = 0; i < ${#testScripts[@]}; i++ ))
    do
        if [ -z "$1" ] || [ "$1" == "-extended" ] || [ "$1" == "${testScripts[$i]}" ] || [ "$1.py" == "${testScripts[$i]}" ]
        then
            echo -e "Running testscript \033[1m${testScripts[$i]}...\033[0m"
            ${BUILDDIR}/qa/rpc-tests/${testScripts[$i]} --srcdir "${BUILDDIR}/src"
        fi
    done
    for (( i = 0; i < ${#testScriptsExt[@]}; i++ ))
    do
        if [ "$1" == "-extended" ] || [ "$1" == "${testScriptsExt[$i]}" ] || [ "$1.py" == "${testScriptsExt[$i]}" ]
        then
            echo -e "Running testscript \033[1m${testScriptsExt[$i]}...\033[0m"
            ${BUILDDIR}/qa/rpc-tests/${testScriptsExt[$i]} --srcdir "${BUILDDIR}/src"
        fi
    done
else
  echo "No rpc tests to run. Wallet, utils, and bitcoind must all be enabled"
fi
