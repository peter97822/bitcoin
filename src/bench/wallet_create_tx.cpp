// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <chainparams.h>
#include <wallet/coincontrol.h>
#include <consensus/merkle.h>
#include <kernel/chain.h>
#include <node/context.h>
#include <test/util/setup_common.h>
#include <test/util/wallet.h>
#include <validation.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>

using wallet::CWallet;
using wallet::CreateMockWalletDatabase;
using wallet::DBErrors;
using wallet::WALLET_FLAG_DESCRIPTORS;

struct TipBlock
{
    uint256 prev_block_hash;
    int64_t prev_block_time;
    int tip_height;
};

TipBlock getTip(const CChainParams& params, const node::NodeContext& context)
{
    auto tip = WITH_LOCK(::cs_main, return context.chainman->ActiveTip());
    return (tip) ? TipBlock{tip->GetBlockHash(), tip->GetBlockTime(), tip->nHeight} :
           TipBlock{params.GenesisBlock().GetHash(), params.GenesisBlock().GetBlockTime(), 0};
}

void generateFakeBlock(const CChainParams& params,
                       const node::NodeContext& context,
                       CWallet& wallet,
                       const CScript& coinbase_out_script)
{
    TipBlock tip{getTip(params, context)};

    // Create block
    CBlock block;
    CMutableTransaction coinbase_tx;
    coinbase_tx.vin.resize(1);
    coinbase_tx.vin[0].prevout.SetNull();
    coinbase_tx.vout.resize(2);
    coinbase_tx.vout[0].scriptPubKey = coinbase_out_script;
    coinbase_tx.vout[0].nValue = 49 * COIN;
    coinbase_tx.vin[0].scriptSig = CScript() << ++tip.tip_height << OP_0;
    coinbase_tx.vout[1].scriptPubKey = coinbase_out_script; // extra output
    coinbase_tx.vout[1].nValue = 1 * COIN;
    block.vtx = {MakeTransactionRef(std::move(coinbase_tx))};

    block.nVersion = VERSIONBITS_LAST_OLD_BLOCK_VERSION;
    block.hashPrevBlock = tip.prev_block_hash;
    block.hashMerkleRoot = BlockMerkleRoot(block);
    block.nTime = ++tip.prev_block_time;
    block.nBits = params.GenesisBlock().nBits;
    block.nNonce = 0;

    {
        LOCK(::cs_main);
        // Add it to the index
        CBlockIndex* pindex{context.chainman->m_blockman.AddToBlockIndex(block, context.chainman->m_best_header)};
        // add it to the chain
        context.chainman->ActiveChain().SetTip(*pindex);
    }

    // notify wallet
    const auto& pindex = WITH_LOCK(::cs_main, return context.chainman->ActiveChain().Tip());
    wallet.blockConnected(kernel::MakeBlockInfo(pindex, &block));
}

struct PreSelectInputs {
    // How many coins from the wallet the process should select
    int num_of_internal_inputs;
    // future: this could have external inputs as well.
};

static void WalletCreateTx(benchmark::Bench& bench, const OutputType output_type, bool allow_other_inputs, std::optional<PreSelectInputs> preset_inputs)
{
    const auto test_setup = MakeNoLogFileContext<const TestingSetup>();

    CWallet wallet{test_setup->m_node.chain.get(), "", gArgs, CreateMockWalletDatabase()};
    {
        LOCK(wallet.cs_wallet);
        wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        wallet.SetupDescriptorScriptPubKeyMans();
        if (wallet.LoadWallet() != DBErrors::LOAD_OK) assert(false);
    }

    // Generate destinations
    CScript dest = GetScriptForDestination(getNewDestination(wallet, output_type));

    // Generate chain; each coinbase will have two outputs to fill-up the wallet
    const auto& params = Params();
    unsigned int chain_size = 5000; // 5k blocks means 10k UTXO for the wallet (minus 200 due COINBASE_MATURITY)
    for (unsigned int i = 0; i < chain_size; ++i) {
        generateFakeBlock(params, test_setup->m_node, wallet, dest);
    }

    // Check available balance
    auto bal = wallet::GetAvailableBalance(wallet); // Cache
    assert(bal == 50 * COIN * (chain_size - COINBASE_MATURITY));

    wallet::CCoinControl coin_control;
    coin_control.m_allow_other_inputs = allow_other_inputs;

    CAmount target = 0;
    if (preset_inputs) {
        // Select inputs, each has 49 BTC
        const auto& res = WITH_LOCK(wallet.cs_wallet,
                                    return wallet::AvailableCoins(wallet, nullptr, std::nullopt, 1, MAX_MONEY,
                                                                  MAX_MONEY, preset_inputs->num_of_internal_inputs));
        for (int i=0; i < preset_inputs->num_of_internal_inputs; i++) {
            const auto& coin{res.coins.at(output_type)[i]};
            target += coin.txout.nValue;
            coin_control.Select(coin.outpoint);
        }
    }

    // If automatic coin selection is enabled, add the value of another UTXO to the target
    if (coin_control.m_allow_other_inputs) target += 50 * COIN;
    std::vector<wallet::CRecipient> recipients = {{dest, target, true}};

    bench.epochIterations(5).run([&] {
        LOCK(wallet.cs_wallet);
        const auto& tx_res = CreateTransaction(wallet, recipients, -1, coin_control);
        assert(tx_res);
    });
}

static void AvailableCoins(benchmark::Bench& bench, const std::vector<OutputType>& output_type)
{
    const auto test_setup = MakeNoLogFileContext<const TestingSetup>();
    CWallet wallet{test_setup->m_node.chain.get(), "", gArgs, CreateMockWalletDatabase()};
    {
        LOCK(wallet.cs_wallet);
        wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        wallet.SetupDescriptorScriptPubKeyMans();
        if (wallet.LoadWallet() != DBErrors::LOAD_OK) assert(false);
    }

    // Generate destinations
    std::vector<CScript> dest_wallet;
    for (auto type : output_type) {
        dest_wallet.emplace_back(GetScriptForDestination(getNewDestination(wallet, type)));
    }

    // Generate chain; each coinbase will have two outputs to fill-up the wallet
    const auto& params = Params();
    unsigned int chain_size = 1000;
    for (unsigned int i = 0; i < chain_size / dest_wallet.size(); ++i) {
        for (const auto& dest : dest_wallet) {
            generateFakeBlock(params, test_setup->m_node, wallet, dest);
        }
    }

    // Check available balance
    auto bal = wallet::GetAvailableBalance(wallet); // Cache
    assert(bal == 50 * COIN * (chain_size - COINBASE_MATURITY));

    bench.epochIterations(2).run([&] {
        LOCK(wallet.cs_wallet);
        const auto& res = wallet::AvailableCoins(wallet);
        assert(res.All().size() == (chain_size - COINBASE_MATURITY) * 2);
    });
}

static void WalletCreateTxUseOnlyPresetInputs(benchmark::Bench& bench) { WalletCreateTx(bench, OutputType::BECH32, /*allow_other_inputs=*/false,
                                                                                        {{/*num_of_internal_inputs=*/4}}); }

static void WalletCreateTxUsePresetInputsAndCoinSelection(benchmark::Bench& bench) { WalletCreateTx(bench, OutputType::BECH32, /*allow_other_inputs=*/true,
                                                                                                    {{/*num_of_internal_inputs=*/4}}); }

static void WalletAvailableCoins(benchmark::Bench& bench) { AvailableCoins(bench, {OutputType::BECH32M}); }

BENCHMARK(WalletCreateTxUseOnlyPresetInputs, benchmark::PriorityLevel::LOW)
BENCHMARK(WalletCreateTxUsePresetInputsAndCoinSelection, benchmark::PriorityLevel::LOW)
BENCHMARK(WalletAvailableCoins, benchmark::PriorityLevel::LOW);