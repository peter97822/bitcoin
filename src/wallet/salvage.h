// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_SALVAGE_H
#define BITCOIN_WALLET_SALVAGE_H

#include <fs.h>
#include <streams.h>

bool RecoverDatabaseFile(const fs::path& file_path, void *callbackDataIn, bool (*recoverKVcallback)(void* callbackData, CDataStream ssKey, CDataStream ssValue), std::string& out_backup_filename);

/* Recover filter (used as callback), will only let keys (cryptographical keys) as KV/key-type pass through */
bool RecoverKeysOnlyFilter(void *callbackData, CDataStream ssKey, CDataStream ssValue);

#endif // BITCOIN_WALLET_SALVAGE_H
