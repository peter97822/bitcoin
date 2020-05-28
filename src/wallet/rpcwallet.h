// Copyright (c) 2016-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_RPCWALLET_H
#define BITCOIN_WALLET_RPCWALLET_H

#include <span.h>

#include <memory>
#include <string>
#include <vector>

class CRPCCommand;
class CWallet;
class JSONRPCRequest;
class LegacyScriptPubKeyMan;
class UniValue;
struct PartiallySignedTransaction;
class CTransaction;

namespace interfaces {
class Chain;
}

//! Pointer to chain interface that needs to be declared as a global to be
//! accessible loadwallet and createwallet methods. Due to limitations of the
//! RPC framework, there's currently no direct way to pass in state to RPC
//! methods without globals.
extern interfaces::Chain* g_rpc_chain;
Span<const CRPCCommand> GetWalletRPCCommands();

/**
 * Figures out what wallet, if any, to use for a JSONRPCRequest.
 *
 * @param[in] request JSONRPCRequest that wishes to access a wallet
 * @return nullptr if no wallet should be used, or a pointer to the CWallet
 */
std::shared_ptr<CWallet> GetWalletForJSONRPCRequest(const JSONRPCRequest& request);

void EnsureWalletIsUnlocked(const CWallet*);
bool EnsureWalletIsAvailable(const CWallet*, bool avoidException);
LegacyScriptPubKeyMan& EnsureLegacyScriptPubKeyMan(CWallet& wallet, bool also_create = false);

UniValue getaddressinfo(const JSONRPCRequest& request);
UniValue signrawtransactionwithwallet(const JSONRPCRequest& request);
#endif //BITCOIN_WALLET_RPCWALLET_H
