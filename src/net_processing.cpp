// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <net_processing.h>

#include <addrman.h>
#include <banman.h>
#include <blockencodings.h>
#include <blockfilter.h>
#include <chainparams.h>
#include <consensus/amount.h>
#include <consensus/validation.h>
#include <deploymentstatus.h>
#include <hash.h>
#include <headerssync.h>
#include <index/blockfilterindex.h>
#include <merkleblock.h>
#include <netbase.h>
#include <netmessagemaker.h>
#include <node/blockstorage.h>
#include <policy/fees.h>
#include <policy/policy.h>
#include <policy/settings.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <random.h>
#include <reverse_iterator.h>
#include <scheduler.h>
#include <streams.h>
#include <sync.h>
#include <timedata.h>
#include <tinyformat.h>
#include <txmempool.h>
#include <txorphanage.h>
#include <txrequest.h>
#include <util/check.h> // For NDEBUG compile time check
#include <util/strencodings.h>
#include <util/system.h>
#include <util/trace.h>
#include <validation.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <typeinfo>

using node::ReadBlockFromDisk;
using node::ReadRawBlockFromDisk;
using node::fImporting;
using node::fPruneMode;
using node::fReindex;

/** How long to cache transactions in mapRelay for normal relay */
static constexpr auto RELAY_TX_CACHE_TIME = 15min;
/** How long a transaction has to be in the mempool before it can unconditionally be relayed (even when not in mapRelay). */
static constexpr auto UNCONDITIONAL_RELAY_DELAY = 2min;
/** Headers download timeout.
 *  Timeout = base + per_header * (expected number of headers) */
static constexpr auto HEADERS_DOWNLOAD_TIMEOUT_BASE = 15min;
static constexpr auto HEADERS_DOWNLOAD_TIMEOUT_PER_HEADER = 1ms;
/** How long to wait for a peer to respond to a getheaders request */
static constexpr auto HEADERS_RESPONSE_TIME{2min};
/** Protect at least this many outbound peers from disconnection due to slow/
 * behind headers chain.
 */
static constexpr int32_t MAX_OUTBOUND_PEERS_TO_PROTECT_FROM_DISCONNECT = 4;
/** Timeout for (unprotected) outbound peers to sync to our chainwork */
static constexpr auto CHAIN_SYNC_TIMEOUT{20min};
/** How frequently to check for stale tips */
static constexpr auto STALE_CHECK_INTERVAL{10min};
/** How frequently to check for extra outbound peers and disconnect */
static constexpr auto EXTRA_PEER_CHECK_INTERVAL{45s};
/** Minimum time an outbound-peer-eviction candidate must be connected for, in order to evict */
static constexpr auto MINIMUM_CONNECT_TIME{30s};
/** SHA256("main address relay")[0:8] */
static constexpr uint64_t RANDOMIZER_ID_ADDRESS_RELAY = 0x3cac0035b5866b90ULL;
/// Age after which a stale block will no longer be served if requested as
/// protection against fingerprinting. Set to one month, denominated in seconds.
static constexpr int STALE_RELAY_AGE_LIMIT = 30 * 24 * 60 * 60;
/// Age after which a block is considered historical for purposes of rate
/// limiting block relay. Set to one week, denominated in seconds.
static constexpr int HISTORICAL_BLOCK_AGE = 7 * 24 * 60 * 60;
/** Time between pings automatically sent out for latency probing and keepalive */
static constexpr auto PING_INTERVAL{2min};
/** The maximum number of entries in a locator */
static const unsigned int MAX_LOCATOR_SZ = 101;
/** The maximum number of entries in an 'inv' protocol message */
static const unsigned int MAX_INV_SZ = 50000;
/** Maximum number of in-flight transaction requests from a peer. It is not a hard limit, but the threshold at which
 *  point the OVERLOADED_PEER_TX_DELAY kicks in. */
static constexpr int32_t MAX_PEER_TX_REQUEST_IN_FLIGHT = 100;
/** Maximum number of transactions to consider for requesting, per peer. It provides a reasonable DoS limit to
 *  per-peer memory usage spent on announcements, while covering peers continuously sending INVs at the maximum
 *  rate (by our own policy, see INVENTORY_BROADCAST_PER_SECOND) for several minutes, while not receiving
 *  the actual transaction (from any peer) in response to requests for them. */
static constexpr int32_t MAX_PEER_TX_ANNOUNCEMENTS = 5000;
/** How long to delay requesting transactions via txids, if we have wtxid-relaying peers */
static constexpr auto TXID_RELAY_DELAY{2s};
/** How long to delay requesting transactions from non-preferred peers */
static constexpr auto NONPREF_PEER_TX_DELAY{2s};
/** How long to delay requesting transactions from overloaded peers (see MAX_PEER_TX_REQUEST_IN_FLIGHT). */
static constexpr auto OVERLOADED_PEER_TX_DELAY{2s};
/** How long to wait before downloading a transaction from an additional peer */
static constexpr auto GETDATA_TX_INTERVAL{60s};
/** Limit to avoid sending big packets. Not used in processing incoming GETDATA for compatibility */
static const unsigned int MAX_GETDATA_SZ = 1000;
/** Number of blocks that can be requested at any given time from a single peer. */
static const int MAX_BLOCKS_IN_TRANSIT_PER_PEER = 16;
/** Time during which a peer must stall block download progress before being disconnected. */
static constexpr auto BLOCK_STALLING_TIMEOUT{2s};
/** Number of headers sent in one getheaders result. We rely on the assumption that if a peer sends
 *  less than this number, we reached its tip. Changing this value is a protocol upgrade. */
static const unsigned int MAX_HEADERS_RESULTS = 2000;
/** Maximum depth of blocks we're willing to serve as compact blocks to peers
 *  when requested. For older blocks, a regular BLOCK response will be sent. */
static const int MAX_CMPCTBLOCK_DEPTH = 5;
/** Maximum depth of blocks we're willing to respond to GETBLOCKTXN requests for. */
static const int MAX_BLOCKTXN_DEPTH = 10;
/** Size of the "block download window": how far ahead of our current height do we fetch?
 *  Larger windows tolerate larger download speed differences between peer, but increase the potential
 *  degree of disordering of blocks on disk (which make reindexing and pruning harder). We'll probably
 *  want to make this a per-peer adaptive value at some point. */
static const unsigned int BLOCK_DOWNLOAD_WINDOW = 1024;
/** Block download timeout base, expressed in multiples of the block interval (i.e. 10 min) */
static constexpr double BLOCK_DOWNLOAD_TIMEOUT_BASE = 1;
/** Additional block download timeout per parallel downloading peer (i.e. 5 min) */
static constexpr double BLOCK_DOWNLOAD_TIMEOUT_PER_PEER = 0.5;
/** Maximum number of headers to announce when relaying blocks with headers message.*/
static const unsigned int MAX_BLOCKS_TO_ANNOUNCE = 8;
/** Maximum number of unconnecting headers announcements before DoS score */
static const int MAX_UNCONNECTING_HEADERS = 10;
/** Minimum blocks required to signal NODE_NETWORK_LIMITED */
static const unsigned int NODE_NETWORK_LIMITED_MIN_BLOCKS = 288;
/** Average delay between local address broadcasts */
static constexpr auto AVG_LOCAL_ADDRESS_BROADCAST_INTERVAL{24h};
/** Average delay between peer address broadcasts */
static constexpr auto AVG_ADDRESS_BROADCAST_INTERVAL{30s};
/** Delay between rotating the peers we relay a particular address to */
static constexpr auto ROTATE_ADDR_RELAY_DEST_INTERVAL{24h};
/** Average delay between trickled inventory transmissions for inbound peers.
 *  Blocks and peers with NetPermissionFlags::NoBan permission bypass this. */
static constexpr auto INBOUND_INVENTORY_BROADCAST_INTERVAL{5s};
/** Average delay between trickled inventory transmissions for outbound peers.
 *  Use a smaller delay as there is less privacy concern for them.
 *  Blocks and peers with NetPermissionFlags::NoBan permission bypass this. */
static constexpr auto OUTBOUND_INVENTORY_BROADCAST_INTERVAL{2s};
/** Maximum rate of inventory items to send per second.
 *  Limits the impact of low-fee transaction floods. */
static constexpr unsigned int INVENTORY_BROADCAST_PER_SECOND = 7;
/** Maximum number of inventory items to send per transmission. */
static constexpr unsigned int INVENTORY_BROADCAST_MAX = INVENTORY_BROADCAST_PER_SECOND * count_seconds(INBOUND_INVENTORY_BROADCAST_INTERVAL);
/** The number of most recently announced transactions a peer can request. */
static constexpr unsigned int INVENTORY_MAX_RECENT_RELAY = 3500;
/** Verify that INVENTORY_MAX_RECENT_RELAY is enough to cache everything typically
 *  relayed before unconditional relay from the mempool kicks in. This is only a
 *  lower bound, and it should be larger to account for higher inv rate to outbound
 *  peers, and random variations in the broadcast mechanism. */
static_assert(INVENTORY_MAX_RECENT_RELAY >= INVENTORY_BROADCAST_PER_SECOND * UNCONDITIONAL_RELAY_DELAY / std::chrono::seconds{1}, "INVENTORY_RELAY_MAX too low");
/** Average delay between feefilter broadcasts in seconds. */
static constexpr auto AVG_FEEFILTER_BROADCAST_INTERVAL{10min};
/** Maximum feefilter broadcast delay after significant change. */
static constexpr auto MAX_FEEFILTER_CHANGE_DELAY{5min};
/** Maximum number of compact filters that may be requested with one getcfilters. See BIP 157. */
static constexpr uint32_t MAX_GETCFILTERS_SIZE = 1000;
/** Maximum number of cf hashes that may be requested with one getcfheaders. See BIP 157. */
static constexpr uint32_t MAX_GETCFHEADERS_SIZE = 2000;
/** the maximum percentage of addresses from our addrman to return in response to a getaddr message. */
static constexpr size_t MAX_PCT_ADDR_TO_SEND = 23;
/** The maximum number of address records permitted in an ADDR message. */
static constexpr size_t MAX_ADDR_TO_SEND{1000};
/** The maximum rate of address records we're willing to process on average. Can be bypassed using
 *  the NetPermissionFlags::Addr permission. */
static constexpr double MAX_ADDR_RATE_PER_SECOND{0.1};
/** The soft limit of the address processing token bucket (the regular MAX_ADDR_RATE_PER_SECOND
 *  based increments won't go above this, but the MAX_ADDR_TO_SEND increment following GETADDR
 *  is exempt from this limit). */
static constexpr size_t MAX_ADDR_PROCESSING_TOKEN_BUCKET{MAX_ADDR_TO_SEND};
/** The compactblocks version we support. See BIP 152. */
static constexpr uint64_t CMPCTBLOCKS_VERSION{2};

// Internal stuff
namespace {
/** Blocks that are in flight, and that are in the queue to be downloaded. */
struct QueuedBlock {
    /** BlockIndex. We must have this since we only request blocks when we've already validated the header. */
    const CBlockIndex* pindex;
    /** Optional, used for CMPCTBLOCK downloads */
    std::unique_ptr<PartiallyDownloadedBlock> partialBlock;
};

/**
 * Data structure for an individual peer. This struct is not protected by
 * cs_main since it does not contain validation-critical data.
 *
 * Memory is owned by shared pointers and this object is destructed when
 * the refcount drops to zero.
 *
 * Mutexes inside this struct must not be held when locking m_peer_mutex.
 *
 * TODO: move most members from CNodeState to this structure.
 * TODO: move remaining application-layer data members from CNode to this structure.
 */
struct Peer {
    /** Same id as the CNode object for this peer */
    const NodeId m_id{0};

    /** Services we offered to this peer.
     *
     *  This is supplied by CConnman during peer initialization. It's const
     *  because there is no protocol defined for renegotiating services
     *  initially offered to a peer. The set of local services we offer should
     *  not change after initialization.
     *
     *  An interesting example of this is NODE_NETWORK and initial block
     *  download: a node which starts up from scratch doesn't have any blocks
     *  to serve, but still advertises NODE_NETWORK because it will eventually
     *  fulfill this role after IBD completes. P2P code is written in such a
     *  way that it can gracefully handle peers who don't make good on their
     *  service advertisements. */
    const ServiceFlags m_our_services;
    /** Services this peer offered to us. */
    std::atomic<ServiceFlags> m_their_services{NODE_NONE};

    /** Protects misbehavior data members */
    Mutex m_misbehavior_mutex;
    /** Accumulated misbehavior score for this peer */
    int m_misbehavior_score GUARDED_BY(m_misbehavior_mutex){0};
    /** Whether this peer should be disconnected and marked as discouraged (unless it has NetPermissionFlags::NoBan permission). */
    bool m_should_discourage GUARDED_BY(m_misbehavior_mutex){false};

    /** Protects block inventory data members */
    Mutex m_block_inv_mutex;
    /** List of blocks that we'll announce via an `inv` message.
     * There is no final sorting before sending, as they are always sent
     * immediately and in the order requested. */
    std::vector<uint256> m_blocks_for_inv_relay GUARDED_BY(m_block_inv_mutex);
    /** Unfiltered list of blocks that we'd like to announce via a `headers`
     * message. If we can't announce via a `headers` message, we'll fall back to
     * announcing via `inv`. */
    std::vector<uint256> m_blocks_for_headers_relay GUARDED_BY(m_block_inv_mutex);
    /** The final block hash that we sent in an `inv` message to this peer.
     * When the peer requests this block, we send an `inv` message to trigger
     * the peer to request the next sequence of block hashes.
     * Most peers use headers-first syncing, which doesn't use this mechanism */
    uint256 m_continuation_block GUARDED_BY(m_block_inv_mutex) {};

    /** This peer's reported block height when we connected */
    std::atomic<int> m_starting_height{-1};

    /** The pong reply we're expecting, or 0 if no pong expected. */
    std::atomic<uint64_t> m_ping_nonce_sent{0};
    /** When the last ping was sent, or 0 if no ping was ever sent */
    std::atomic<std::chrono::microseconds> m_ping_start{0us};
    /** Whether a ping has been requested by the user */
    std::atomic<bool> m_ping_queued{false};

    /** Whether this peer relays txs via wtxid */
    std::atomic<bool> m_wtxid_relay{false};
    /** The feerate in the most recent BIP133 `feefilter` message sent to the peer.
     *  It is *not* a p2p protocol violation for the peer to send us
     *  transactions with a lower fee rate than this. See BIP133. */
    CAmount m_fee_filter_sent GUARDED_BY(NetEventsInterface::g_msgproc_mutex){0};
    /** Timestamp after which we will send the next BIP133 `feefilter` message
      * to the peer. */
    std::chrono::microseconds m_next_send_feefilter GUARDED_BY(NetEventsInterface::g_msgproc_mutex){0};

    struct TxRelay {
        mutable RecursiveMutex m_bloom_filter_mutex;
        /** Whether the peer wishes to receive transaction announcements.
         *
         * This is initially set based on the fRelay flag in the received
         * `version` message. If initially set to false, it can only be flipped
         * to true if we have offered the peer NODE_BLOOM services and it sends
         * us a `filterload` or `filterclear` message. See BIP37. */
        bool m_relay_txs GUARDED_BY(m_bloom_filter_mutex){false};
        /** A bloom filter for which transactions to announce to the peer. See BIP37. */
        std::unique_ptr<CBloomFilter> m_bloom_filter PT_GUARDED_BY(m_bloom_filter_mutex) GUARDED_BY(m_bloom_filter_mutex){nullptr};

        mutable RecursiveMutex m_tx_inventory_mutex;
        /** A filter of all the txids and wtxids that the peer has announced to
         *  us or we have announced to the peer. We use this to avoid announcing
         *  the same txid/wtxid to a peer that already has the transaction. */
        CRollingBloomFilter m_tx_inventory_known_filter GUARDED_BY(m_tx_inventory_mutex){50000, 0.000001};
        /** Set of transaction ids we still have to announce (txid for
         *  non-wtxid-relay peers, wtxid for wtxid-relay peers). We use the
         *  mempool to sort transactions in dependency order before relay, so
         *  this does not have to be sorted. */
        std::set<uint256> m_tx_inventory_to_send GUARDED_BY(m_tx_inventory_mutex);
        /** Whether the peer has requested us to send our complete mempool. Only
         *  permitted if the peer has NetPermissionFlags::Mempool. See BIP35. */
        bool m_send_mempool GUARDED_BY(m_tx_inventory_mutex){false};
        /** The last time a BIP35 `mempool` request was serviced. */
        std::atomic<std::chrono::seconds> m_last_mempool_req{0s};
        /** The next time after which we will send an `inv` message containing
         *  transaction announcements to this peer. */
        std::chrono::microseconds m_next_inv_send_time GUARDED_BY(NetEventsInterface::g_msgproc_mutex){0};

        /** Minimum fee rate with which to filter transaction announcements to this node. See BIP133. */
        std::atomic<CAmount> m_fee_filter_received{0};
    };

    /* Initializes a TxRelay struct for this peer. Can be called at most once for a peer. */
    TxRelay* SetTxRelay() EXCLUSIVE_LOCKS_REQUIRED(!m_tx_relay_mutex)
    {
        LOCK(m_tx_relay_mutex);
        Assume(!m_tx_relay);
        m_tx_relay = std::make_unique<Peer::TxRelay>();
        return m_tx_relay.get();
    };

    TxRelay* GetTxRelay() EXCLUSIVE_LOCKS_REQUIRED(!m_tx_relay_mutex)
    {
        return WITH_LOCK(m_tx_relay_mutex, return m_tx_relay.get());
    };

    /** A vector of addresses to send to the peer, limited to MAX_ADDR_TO_SEND. */
    std::vector<CAddress> m_addrs_to_send GUARDED_BY(NetEventsInterface::g_msgproc_mutex);
    /** Probabilistic filter to track recent addr messages relayed with this
     *  peer. Used to avoid relaying redundant addresses to this peer.
     *
     *  We initialize this filter for outbound peers (other than
     *  block-relay-only connections) or when an inbound peer sends us an
     *  address related message (ADDR, ADDRV2, GETADDR).
     *
     *  Presence of this filter must correlate with m_addr_relay_enabled.
     **/
    std::unique_ptr<CRollingBloomFilter> m_addr_known GUARDED_BY(NetEventsInterface::g_msgproc_mutex);
    /** Whether we are participating in address relay with this connection.
     *
     *  We set this bool to true for outbound peers (other than
     *  block-relay-only connections), or when an inbound peer sends us an
     *  address related message (ADDR, ADDRV2, GETADDR).
     *
     *  We use this bool to decide whether a peer is eligible for gossiping
     *  addr messages. This avoids relaying to peers that are unlikely to
     *  forward them, effectively blackholing self announcements. Reasons
     *  peers might support addr relay on the link include that they connected
     *  to us as a block-relay-only peer or they are a light client.
     *
     *  This field must correlate with whether m_addr_known has been
     *  initialized.*/
    std::atomic_bool m_addr_relay_enabled{false};
    /** Whether a getaddr request to this peer is outstanding. */
    bool m_getaddr_sent GUARDED_BY(NetEventsInterface::g_msgproc_mutex){false};
    /** Guards address sending timers. */
    mutable Mutex m_addr_send_times_mutex;
    /** Time point to send the next ADDR message to this peer. */
    std::chrono::microseconds m_next_addr_send GUARDED_BY(m_addr_send_times_mutex){0};
    /** Time point to possibly re-announce our local address to this peer. */
    std::chrono::microseconds m_next_local_addr_send GUARDED_BY(m_addr_send_times_mutex){0};
    /** Whether the peer has signaled support for receiving ADDRv2 (BIP155)
     *  messages, indicating a preference to receive ADDRv2 instead of ADDR ones. */
    std::atomic_bool m_wants_addrv2{false};
    /** Whether this peer has already sent us a getaddr message. */
    bool m_getaddr_recvd GUARDED_BY(NetEventsInterface::g_msgproc_mutex){false};
    /** Number of addresses that can be processed from this peer. Start at 1 to
     *  permit self-announcement. */
    double m_addr_token_bucket GUARDED_BY(NetEventsInterface::g_msgproc_mutex){1.0};
    /** When m_addr_token_bucket was last updated */
    std::chrono::microseconds m_addr_token_timestamp GUARDED_BY(NetEventsInterface::g_msgproc_mutex){GetTime<std::chrono::microseconds>()};
    /** Total number of addresses that were dropped due to rate limiting. */
    std::atomic<uint64_t> m_addr_rate_limited{0};
    /** Total number of addresses that were processed (excludes rate-limited ones). */
    std::atomic<uint64_t> m_addr_processed{0};

    /** Set of txids to reconsider once their parent transactions have been accepted **/
    std::set<uint256> m_orphan_work_set GUARDED_BY(g_cs_orphans);

    /** Whether we've sent this peer a getheaders in response to an inv prior to initial-headers-sync completing */
    bool m_inv_triggered_getheaders_before_sync GUARDED_BY(NetEventsInterface::g_msgproc_mutex){false};

    /** Protects m_getdata_requests **/
    Mutex m_getdata_requests_mutex;
    /** Work queue of items requested by this peer **/
    std::deque<CInv> m_getdata_requests GUARDED_BY(m_getdata_requests_mutex);

    /** Time of the last getheaders message to this peer */
    NodeClock::time_point m_last_getheaders_timestamp GUARDED_BY(NetEventsInterface::g_msgproc_mutex){};

    /** Protects m_headers_sync **/
    Mutex m_headers_sync_mutex;
    /** Headers-sync state for this peer (eg for initial sync, or syncing large
     * reorgs) **/
    std::unique_ptr<HeadersSyncState> m_headers_sync PT_GUARDED_BY(m_headers_sync_mutex) GUARDED_BY(m_headers_sync_mutex) {};

    /** Whether we've sent our peer a sendheaders message. **/
    std::atomic<bool> m_sent_sendheaders{false};

    explicit Peer(NodeId id, ServiceFlags our_services)
        : m_id{id}
        , m_our_services{our_services}
    {}

private:
    Mutex m_tx_relay_mutex;

    /** Transaction relay data. Will be a nullptr if we're not relaying
     *  transactions with this peer (e.g. if it's a block-relay-only peer or
     *  the peer has sent us fRelay=false with bloom filters disabled). */
    std::unique_ptr<TxRelay> m_tx_relay GUARDED_BY(m_tx_relay_mutex);
};

using PeerRef = std::shared_ptr<Peer>;

/**
 * Maintain validation-specific state about nodes, protected by cs_main, instead
 * by CNode's own locks. This simplifies asynchronous operation, where
 * processing of incoming data is done after the ProcessMessage call returns,
 * and we're no longer holding the node's locks.
 */
struct CNodeState {
    //! The best known block we know this peer has announced.
    const CBlockIndex* pindexBestKnownBlock{nullptr};
    //! The hash of the last unknown block this peer has announced.
    uint256 hashLastUnknownBlock{};
    //! The last full block we both have.
    const CBlockIndex* pindexLastCommonBlock{nullptr};
    //! The best header we have sent our peer.
    const CBlockIndex* pindexBestHeaderSent{nullptr};
    //! Length of current-streak of unconnecting headers announcements
    int nUnconnectingHeaders{0};
    //! Whether we've started headers synchronization with this peer.
    bool fSyncStarted{false};
    //! When to potentially disconnect peer for stalling headers download
    std::chrono::microseconds m_headers_sync_timeout{0us};
    //! Since when we're stalling block download progress (in microseconds), or 0.
    std::chrono::microseconds m_stalling_since{0us};
    std::list<QueuedBlock> vBlocksInFlight;
    //! When the first entry in vBlocksInFlight started downloading. Don't care when vBlocksInFlight is empty.
    std::chrono::microseconds m_downloading_since{0us};
    int nBlocksInFlight{0};
    //! Whether we consider this a preferred download peer.
    bool fPreferredDownload{false};
    //! Whether this peer wants invs or headers (when possible) for block announcements.
    bool fPreferHeaders{false};
    /** Whether this peer wants invs or cmpctblocks (when possible) for block announcements. */
    bool m_requested_hb_cmpctblocks{false};
    /** Whether this peer will send us cmpctblocks if we request them. */
    bool m_provides_cmpctblocks{false};

    /** State used to enforce CHAIN_SYNC_TIMEOUT and EXTRA_PEER_CHECK_INTERVAL logic.
      *
      * Both are only in effect for outbound, non-manual, non-protected connections.
      * Any peer protected (m_protect = true) is not chosen for eviction. A peer is
      * marked as protected if all of these are true:
      *   - its connection type is IsBlockOnlyConn() == false
      *   - it gave us a valid connecting header
      *   - we haven't reached MAX_OUTBOUND_PEERS_TO_PROTECT_FROM_DISCONNECT yet
      *   - its chain tip has at least as much work as ours
      *
      * CHAIN_SYNC_TIMEOUT: if a peer's best known block has less work than our tip,
      * set a timeout CHAIN_SYNC_TIMEOUT in the future:
      *   - If at timeout their best known block now has more work than our tip
      *     when the timeout was set, then either reset the timeout or clear it
      *     (after comparing against our current tip's work)
      *   - If at timeout their best known block still has less work than our
      *     tip did when the timeout was set, then send a getheaders message,
      *     and set a shorter timeout, HEADERS_RESPONSE_TIME seconds in future.
      *     If their best known block is still behind when that new timeout is
      *     reached, disconnect.
      *
      * EXTRA_PEER_CHECK_INTERVAL: after each interval, if we have too many outbound peers,
      * drop the outbound one that least recently announced us a new block.
      */
    struct ChainSyncTimeoutState {
        //! A timeout used for checking whether our peer has sufficiently synced
        std::chrono::seconds m_timeout{0s};
        //! A header with the work we require on our peer's chain
        const CBlockIndex* m_work_header{nullptr};
        //! After timeout is reached, set to true after sending getheaders
        bool m_sent_getheaders{false};
        //! Whether this peer is protected from disconnection due to a bad/slow chain
        bool m_protect{false};
    };

    ChainSyncTimeoutState m_chain_sync;

    //! Time of last new block announcement
    int64_t m_last_block_announcement{0};

    //! Whether this peer is an inbound connection
    const bool m_is_inbound;

    //! A rolling bloom filter of all announced tx CInvs to this peer.
    CRollingBloomFilter m_recently_announced_invs = CRollingBloomFilter{INVENTORY_MAX_RECENT_RELAY, 0.000001};

    CNodeState(bool is_inbound) : m_is_inbound(is_inbound) {}
};

class PeerManagerImpl final : public PeerManager
{
public:
    PeerManagerImpl(CConnman& connman, AddrMan& addrman,
                    BanMan* banman, ChainstateManager& chainman,
                    CTxMemPool& pool, bool ignore_incoming_txs);

    /** Overridden from CValidationInterface. */
    void BlockConnected(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex* pindexConnected) override
        EXCLUSIVE_LOCKS_REQUIRED(!m_recent_confirmed_transactions_mutex);
    void BlockDisconnected(const std::shared_ptr<const CBlock> &block, const CBlockIndex* pindex) override
        EXCLUSIVE_LOCKS_REQUIRED(!m_recent_confirmed_transactions_mutex);
    void UpdatedBlockTip(const CBlockIndex *pindexNew, const CBlockIndex *pindexFork, bool fInitialDownload) override
        EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex);
    void BlockChecked(const CBlock& block, const BlockValidationState& state) override
        EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex);
    void NewPoWValidBlock(const CBlockIndex *pindex, const std::shared_ptr<const CBlock>& pblock) override
        EXCLUSIVE_LOCKS_REQUIRED(!m_most_recent_block_mutex);

    /** Implement NetEventsInterface */
    void InitializeNode(CNode& node, ServiceFlags our_services) override EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex);
    void FinalizeNode(const CNode& node) override EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex, !m_headers_presync_mutex);
    bool ProcessMessages(CNode* pfrom, std::atomic<bool>& interrupt) override
        EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex, !m_recent_confirmed_transactions_mutex, !m_most_recent_block_mutex, !m_headers_presync_mutex, g_msgproc_mutex);
    bool SendMessages(CNode* pto) override
        EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex, !m_recent_confirmed_transactions_mutex, !m_most_recent_block_mutex, g_msgproc_mutex);

    /** Implement PeerManager */
    void StartScheduledTasks(CScheduler& scheduler) override;
    void CheckForStaleTipAndEvictPeers() override;
    std::optional<std::string> FetchBlock(NodeId peer_id, const CBlockIndex& block_index) override
        EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex);
    bool GetNodeStateStats(NodeId nodeid, CNodeStateStats& stats) const override EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex);
    bool IgnoresIncomingTxs() override { return m_ignore_incoming_txs; }
    void SendPings() override EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex);
    void RelayTransaction(const uint256& txid, const uint256& wtxid) override EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex);
    void SetBestHeight(int height) override { m_best_height = height; };
    void UnitTestMisbehaving(NodeId peer_id, int howmuch) override EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex) { Misbehaving(*Assert(GetPeerRef(peer_id)), howmuch, ""); };
    void ProcessMessage(CNode& pfrom, const std::string& msg_type, CDataStream& vRecv,
                        const std::chrono::microseconds time_received, const std::atomic<bool>& interruptMsgProc) override
        EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex, !m_recent_confirmed_transactions_mutex, !m_most_recent_block_mutex, !m_headers_presync_mutex, g_msgproc_mutex);
    void UpdateLastBlockAnnounceTime(NodeId node, int64_t time_in_seconds) override;

private:
    /** Consider evicting an outbound peer based on the amount of time they've been behind our tip */
    void ConsiderEviction(CNode& pto, Peer& peer, std::chrono::seconds time_in_seconds) EXCLUSIVE_LOCKS_REQUIRED(cs_main, g_msgproc_mutex);

    /** If we have extra outbound peers, try to disconnect the one with the oldest block announcement */
    void EvictExtraOutboundPeers(std::chrono::seconds now) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /** Retrieve unbroadcast transactions from the mempool and reattempt sending to peers */
    void ReattemptInitialBroadcast(CScheduler& scheduler) EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex);

    /** Get a shared pointer to the Peer object.
     *  May return an empty shared_ptr if the Peer object can't be found. */
    PeerRef GetPeerRef(NodeId id) const EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex);

    /** Get a shared pointer to the Peer object and remove it from m_peer_map.
     *  May return an empty shared_ptr if the Peer object can't be found. */
    PeerRef RemovePeer(NodeId id) EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex);

    /**
     * Increment peer's misbehavior score. If the new value >= DISCOURAGEMENT_THRESHOLD, mark the node
     * to be discouraged, meaning the peer might be disconnected and added to the discouragement filter.
     */
    void Misbehaving(Peer& peer, int howmuch, const std::string& message);

    /**
     * Potentially mark a node discouraged based on the contents of a BlockValidationState object
     *
     * @param[in] via_compact_block this bool is passed in because net_processing should
     * punish peers differently depending on whether the data was provided in a compact
     * block message or not. If the compact block had a valid header, but contained invalid
     * txs, the peer should not be punished. See BIP 152.
     *
     * @return Returns true if the peer was punished (probably disconnected)
     */
    bool MaybePunishNodeForBlock(NodeId nodeid, const BlockValidationState& state,
                                 bool via_compact_block, const std::string& message = "")
        EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex);

    /**
     * Potentially disconnect and discourage a node based on the contents of a TxValidationState object
     *
     * @return Returns true if the peer was punished (probably disconnected)
     */
    bool MaybePunishNodeForTx(NodeId nodeid, const TxValidationState& state, const std::string& message = "")
        EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex);

    /** Maybe disconnect a peer and discourage future connections from its address.
     *
     * @param[in]   pnode     The node to check.
     * @param[in]   peer      The peer object to check.
     * @return                True if the peer was marked for disconnection in this function
     */
    bool MaybeDiscourageAndDisconnect(CNode& pnode, Peer& peer);

    void ProcessOrphanTx(std::set<uint256>& orphan_work_set) EXCLUSIVE_LOCKS_REQUIRED(cs_main, g_cs_orphans)
        EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex);
    /** Process a single headers message from a peer.
     *
     * @param[in]   pfrom     CNode of the peer
     * @param[in]   peer      The peer sending us the headers
     * @param[in]   headers   The headers received. Note that this may be modified within ProcessHeadersMessage.
     * @param[in]   via_compact_block   Whether this header came in via compact block handling.
    */
    void ProcessHeadersMessage(CNode& pfrom, Peer& peer,
                               std::vector<CBlockHeader>&& headers,
                               bool via_compact_block)
        EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex, !m_headers_presync_mutex, g_msgproc_mutex);
    /** Various helpers for headers processing, invoked by ProcessHeadersMessage() */
    /** Return true if headers are continuous and have valid proof-of-work (DoS points assigned on failure) */
    bool CheckHeadersPoW(const std::vector<CBlockHeader>& headers, const Consensus::Params& consensusParams, Peer& peer);
    /** Calculate an anti-DoS work threshold for headers chains */
    arith_uint256 GetAntiDoSWorkThreshold();
    /** Deal with state tracking and headers sync for peers that send the
     * occasional non-connecting header (this can happen due to BIP 130 headers
     * announcements for blocks interacting with the 2hr (MAX_FUTURE_BLOCK_TIME) rule). */
    void HandleFewUnconnectingHeaders(CNode& pfrom, Peer& peer, const std::vector<CBlockHeader>& headers) EXCLUSIVE_LOCKS_REQUIRED(g_msgproc_mutex);
    /** Return true if the headers connect to each other, false otherwise */
    bool CheckHeadersAreContinuous(const std::vector<CBlockHeader>& headers) const;
    /** Try to continue a low-work headers sync that has already begun.
     * Assumes the caller has already verified the headers connect, and has
     * checked that each header satisfies the proof-of-work target included in
     * the header.
     *  @param[in]  peer                            The peer we're syncing with.
     *  @param[in]  pfrom                           CNode of the peer
     *  @param[in,out] headers                      The headers to be processed.
     *  @return     True if the passed in headers were successfully processed
     *              as the continuation of a low-work headers sync in progress;
     *              false otherwise.
     *              If false, the passed in headers will be returned back to
     *              the caller.
     *              If true, the returned headers may be empty, indicating
     *              there is no more work for the caller to do; or the headers
     *              may be populated with entries that have passed anti-DoS
     *              checks (and therefore may be validated for block index
     *              acceptance by the caller).
     */
    bool IsContinuationOfLowWorkHeadersSync(Peer& peer, CNode& pfrom,
            std::vector<CBlockHeader>& headers)
        EXCLUSIVE_LOCKS_REQUIRED(peer.m_headers_sync_mutex, !m_headers_presync_mutex, g_msgproc_mutex);
    /** Check work on a headers chain to be processed, and if insufficient,
     * initiate our anti-DoS headers sync mechanism.
     *
     * @param[in]   peer                The peer whose headers we're processing.
     * @param[in]   pfrom               CNode of the peer
     * @param[in]   chain_start_header  Where these headers connect in our index.
     * @param[in,out]   headers             The headers to be processed.
     *
     * @return      True if chain was low work and a headers sync was
     *              initiated (and headers will be empty after calling); false
     *              otherwise.
     */
    bool TryLowWorkHeadersSync(Peer& peer, CNode& pfrom,
                                  const CBlockIndex* chain_start_header,
                                  std::vector<CBlockHeader>& headers)
        EXCLUSIVE_LOCKS_REQUIRED(!peer.m_headers_sync_mutex, !m_peer_mutex, !m_headers_presync_mutex, g_msgproc_mutex);

    /** Return true if the given header is an ancestor of
     *  m_chainman.m_best_header or our current tip */
    bool IsAncestorOfBestHeaderOrTip(const CBlockIndex* header) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /** Request further headers from this peer with a given locator.
     * We don't issue a getheaders message if we have a recent one outstanding.
     * This returns true if a getheaders is actually sent, and false otherwise.
     */
    bool MaybeSendGetHeaders(CNode& pfrom, const CBlockLocator& locator, Peer& peer) EXCLUSIVE_LOCKS_REQUIRED(g_msgproc_mutex);
    /** Potentially fetch blocks from this peer upon receipt of a new headers tip */
    void HeadersDirectFetchBlocks(CNode& pfrom, const Peer& peer, const CBlockIndex* pindexLast);
    /** Update peer state based on received headers message */
    void UpdatePeerStateForReceivedHeaders(CNode& pfrom, const CBlockIndex *pindexLast, bool received_new_header, bool may_have_more_headers);

    void SendBlockTransactions(CNode& pfrom, Peer& peer, const CBlock& block, const BlockTransactionsRequest& req);

    /** Register with TxRequestTracker that an INV has been received from a
     *  peer. The announcement parameters are decided in PeerManager and then
     *  passed to TxRequestTracker. */
    void AddTxAnnouncement(const CNode& node, const GenTxid& gtxid, std::chrono::microseconds current_time)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /** Send a version message to a peer */
    void PushNodeVersion(CNode& pnode, const Peer& peer);

    /** Send a ping message every PING_INTERVAL or if requested via RPC. May
     *  mark the peer to be disconnected if a ping has timed out.
     *  We use mockable time for ping timeouts, so setmocktime may cause pings
     *  to time out. */
    void MaybeSendPing(CNode& node_to, Peer& peer, std::chrono::microseconds now);

    /** Send `addr` messages on a regular schedule. */
    void MaybeSendAddr(CNode& node, Peer& peer, std::chrono::microseconds current_time) EXCLUSIVE_LOCKS_REQUIRED(g_msgproc_mutex);

    /** Send a single `sendheaders` message, after we have completed headers sync with a peer. */
    void MaybeSendSendHeaders(CNode& node, Peer& peer) EXCLUSIVE_LOCKS_REQUIRED(g_msgproc_mutex);

    /** Relay (gossip) an address to a few randomly chosen nodes.
     *
     * @param[in] originator   The id of the peer that sent us the address. We don't want to relay it back.
     * @param[in] addr         Address to relay.
     * @param[in] fReachable   Whether the address' network is reachable. We relay unreachable
     *                         addresses less.
     */
    void RelayAddress(NodeId originator, const CAddress& addr, bool fReachable) EXCLUSIVE_LOCKS_REQUIRED(!m_peer_mutex, g_msgproc_mutex);

    /** Send `feefilter` message. */
    void MaybeSendFeefilter(CNode& node, Peer& peer, std::chrono::microseconds current_time) EXCLUSIVE_LOCKS_REQUIRED(g_msgproc_mutex);

    const CChainParams& m_chainparams;
    CConnman& m_connman;
    AddrMan& m_addrman;
    /** Pointer to this node's banman. May be nullptr - check existence before dereferencing. */
    BanMan* const m_banman;
    ChainstateManager& m_chainman;
    CTxMemPool& m_mempool;
    TxRequestTracker m_txrequest GUARDED_BY(::cs_main);

    /** The height of the best chain */
    std::atomic<int> m_best_height{-1};

    /** Next time to check for stale tip */
    std::chrono::seconds m_stale_tip_check_time GUARDED_BY(cs_main){0s};

    /** Whether this node is running in -blocksonly mode */
    const bool m_ignore_incoming_txs;

    bool RejectIncomingTxs(const CNode& peer) const;

    /** Whether we've completed initial sync yet, for determining when to turn
      * on extra block-relay-only peers. */
    bool m_initial_sync_finished GUARDED_BY(cs_main){false};

    /** Protects m_peer_map. This mutex must not be locked while holding a lock
     *  on any of the mutexes inside a Peer object. */
    mutable Mutex m_peer_mutex;
    /**
     * Map of all Peer objects, keyed by peer id. This map is protected
     * by the m_peer_mutex. Once a shared pointer reference is
     * taken, the lock may be released. Individual fields are protected by
     * their own locks.
     */
    std::map<NodeId, PeerRef> m_peer_map GUARDED_BY(m_peer_mutex);

    /** Map maintaining per-node state. */
    std::map<NodeId, CNodeState> m_node_states GUARDED_BY(cs_main);

    /** Get a pointer to a const CNodeState, used when not mutating the CNodeState object. */
    const CNodeState* State(NodeId pnode) const EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    /** Get a pointer to a mutable CNodeState. */
    CNodeState* State(NodeId pnode) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    uint32_t GetFetchFlags(const Peer& peer) const;

    std::atomic<std::chrono::microseconds> m_next_inv_to_inbounds{0us};

    /** Number of nodes with fSyncStarted. */
    int nSyncStarted GUARDED_BY(cs_main) = 0;

    /** Hash of the last block we received via INV */
    uint256 m_last_block_inv_triggering_headers_sync GUARDED_BY(g_msgproc_mutex){};

    /**
     * Sources of received blocks, saved to be able punish them when processing
     * happens afterwards.
     * Set mapBlockSource[hash].second to false if the node should not be
     * punished if the block is invalid.
     */
    std::map<uint256, std::pair<NodeId, bool>> mapBlockSource GUARDED_BY(cs_main);

    /** Number of peers with wtxid relay. */
    std::atomic<int> m_wtxid_relay_peers{0};

    /** Number of outbound peers with m_chain_sync.m_protect. */
    int m_outbound_peers_with_protect_from_disconnect GUARDED_BY(cs_main) = 0;

    /** Number of preferable block download peers. */
    int m_num_preferred_download_peers GUARDED_BY(cs_main){0};

    bool AlreadyHaveTx(const GenTxid& gtxid)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main, !m_recent_confirmed_transactions_mutex);

    /**
     * Filter for transactions that were recently rejected by the mempool.
     * These are not rerequested until the chain tip changes, at which point
     * the entire filter is reset.
     *
     * Without this filter we'd be re-requesting txs from each of our peers,
     * increasing bandwidth consumption considerably. For instance, with 100
     * peers, half of which relay a tx we don't accept, that might be a 50x
     * bandwidth increase. A flooding attacker attempting to roll-over the
     * filter using minimum-sized, 60byte, transactions might manage to send
     * 1000/sec if we have fast peers, so we pick 120,000 to give our peers a
     * two minute window to send invs to us.
     *
     * Decreasing the false positive rate is fairly cheap, so we pick one in a
     * million to make it highly unlikely for users to have issues with this
     * filter.
     *
     * We typically only add wtxids to this filter. For non-segwit
     * transactions, the txid == wtxid, so this only prevents us from
     * re-downloading non-segwit transactions when communicating with
     * non-wtxidrelay peers -- which is important for avoiding malleation
     * attacks that could otherwise interfere with transaction relay from
     * non-wtxidrelay peers. For communicating with wtxidrelay peers, having
     * the reject filter store wtxids is exactly what we want to avoid
     * redownload of a rejected transaction.
     *
     * In cases where we can tell that a segwit transaction will fail
     * validation no matter the witness, we may add the txid of such
     * transaction to the filter as well. This can be helpful when
     * communicating with txid-relay peers or if we were to otherwise fetch a
     * transaction via txid (eg in our orphan handling).
     *
     * Memory used: 1.3 MB
     */
    CRollingBloomFilter m_recent_rejects GUARDED_BY(::cs_main){120'000, 0.000'001};
    uint256 hashRecentRejectsChainTip GUARDED_BY(cs_main);

    /*
     * Filter for transactions that have been recently confirmed.
     * We use this to avoid requesting transactions that have already been
     * confirnmed.
     *
     * Blocks don't typically have more than 4000 transactions, so this should
     * be at least six blocks (~1 hr) worth of transactions that we can store,
     * inserting both a txid and wtxid for every observed transaction.
     * If the number of transactions appearing in a block goes up, or if we are
     * seeing getdata requests more than an hour after initial announcement, we
     * can increase this number.
     * The false positive rate of 1/1M should come out to less than 1
     * transaction per day that would be inadvertently ignored (which is the
     * same probability that we have in the reject filter).
     */
    Mutex m_recent_confirmed_transactions_mutex;
    CRollingBloomFilter m_recent_confirmed_transactions GUARDED_BY(m_recent_confirmed_transactions_mutex){48'000, 0.000'001};

    /**
     * For sending `inv`s to inbound peers, we use a single (exponentially
     * distributed) timer for all peers. If we used a separate timer for each
     * peer, a spy node could make multiple inbound connections to us to
     * accurately determine when we received the transaction (and potentially
     * determine the transaction's origin). */
    std::chrono::microseconds NextInvToInbounds(std::chrono::microseconds now,
                                                std::chrono::seconds average_interval);


    // All of the following cache a recent block, and are protected by m_most_recent_block_mutex
    Mutex m_most_recent_block_mutex;
    std::shared_ptr<const CBlock> m_most_recent_block GUARDED_BY(m_most_recent_block_mutex);
    std::shared_ptr<const CBlockHeaderAndShortTxIDs> m_most_recent_compact_block GUARDED_BY(m_most_recent_block_mutex);
    uint256 m_most_recent_block_hash GUARDED_BY(m_most_recent_block_mutex);

    // Data about the low-work headers synchronization, aggregated from all peers' HeadersSyncStates.
    /** Mutex guarding the other m_headers_presync_* variables. */
    Mutex m_headers_presync_mutex;
    /** A type to represent statistics about a peer's low-work headers sync.
     *
     * - The first field is the total verified amount of work in that synchronization.
     * - The second is:
     *   - nullopt: the sync is in REDOWNLOAD phase (phase 2).
     *   - {height, timestamp}: the sync has the specified tip height and block timestamp (phase 1).
     */
    using HeadersPresyncStats = std::pair<arith_uint256, std::optional<std::pair<int64_t, uint32_t>>>;
    /** Statistics for all peers in low-work headers sync. */
    std::map<NodeId, HeadersPresyncStats> m_headers_presync_stats GUARDED_BY(m_headers_presync_mutex) {};
    /** The peer with the most-work entry in m_headers_presync_stats. */
    NodeId m_headers_presync_bestpeer GUARDED_BY(m_headers_presync_mutex) {-1};
    /** The m_headers_presync_stats improved, and needs signalling. */
    std::atomic_bool m_headers_presync_should_signal{false};

    /** Height of the highest block announced using BIP 152 high-bandwidth mode. */
    int m_highest_fast_announce GUARDED_BY(::cs_main){0};

    /** Have we requested this block from a peer */
    bool IsBlockRequested(const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /** Remove this block from our tracked requested blocks. Called if:
     *  - the block has been received from a peer
     *  - the request for the block has timed out
     */
    void RemoveBlockRequest(const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /* Mark a block as in flight
     * Returns false, still setting pit, if the block was already in flight from the same peer
     * pit will only be valid as long as the same cs_main lock is being held
     */
    bool BlockRequested(NodeId nodeid, const CBlockIndex& block, std::list<QueuedBlock>::iterator** pit = nullptr) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    bool TipMayBeStale() EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /** Update pindexLastCommonBlock and add not-in-flight missing successors to vBlocks, until it has
     *  at most count entries.
     */
    void FindNextBlocksToDownload(const Peer& peer, unsigned int count, std::vector<const CBlockIndex*>& vBlocks, NodeId& nodeStaller) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    std::map<uint256, std::pair<NodeId, std::list<QueuedBlock>::iterator> > mapBlocksInFlight GUARDED_BY(cs_main);

    /** When our tip was last updated. */
    std::atomic<std::chrono::seconds> m_last_tip_update{0s};

    /** Determine whether or not a peer can request a transaction, and return it (or nullptr if not found or not allowed). */
    CTransactionRef FindTxForGetData(const CNode& peer, const GenTxid& gtxid, const std::chrono::seconds mempool_req, const std::chrono::seconds now) LOCKS_EXCLUDED(cs_main);

    void ProcessGetData(CNode& pfrom, Peer& peer, const std::atomic<bool>& interruptMsgProc)
        EXCLUSIVE_LOCKS_REQUIRED(!m_most_recent_block_mutex, peer.m_getdata_requests_mutex) LOCKS_EXCLUDED(::cs_main);

    /** Process a new block. Perform any post-processing housekeeping */
    void ProcessBlock(CNode& node, const std::shared_ptr<const CBlock>& block, bool force_processing, bool min_pow_checked);

    /** Relay map (txid or wtxid -> CTransactionRef) */
    typedef std::map<uint256, CTransactionRef> MapRelay;
    MapRelay mapRelay GUARDED_BY(cs_main);
    /** Expiration-time ordered list of (expire time, relay map entry) pairs. */
    std::deque<std::pair<std::chrono::microseconds, MapRelay::iterator>> g_relay_expiration GUARDED_BY(cs_main);

    /**
     * When a peer sends us a valid block, instruct it to announce blocks to us
     * using CMPCTBLOCK if possible by adding its nodeid to the end of
     * lNodesAnnouncingHeaderAndIDs, and keeping that list under a certain size by
     * removing the first element if necessary.
     */
    void MaybeSetPeerAsAnnouncingHeaderAndIDs(NodeId nodeid) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /** Stack of nodes which we have set to announce using compact blocks */
    std::list<NodeId> lNodesAnnouncingHeaderAndIDs GUARDED_BY(cs_main);

    /** Number of peers from which we're downloading blocks. */
    int m_peers_downloading_from GUARDED_BY(cs_main) = 0;

    /** Storage for orphan information */
    TxOrphanage m_orphanage;

    void AddToCompactExtraTransactions(const CTransactionRef& tx) EXCLUSIVE_LOCKS_REQUIRED(g_cs_orphans);

    /** Orphan/conflicted/etc transactions that are kept for compact block reconstruction.
     *  The last -blockreconstructionextratxn/DEFAULT_BLOCK_RECONSTRUCTION_EXTRA_TXN of
     *  these are kept in a ring buffer */
    std::vector<std::pair<uint256, CTransactionRef>> vExtraTxnForCompact GUARDED_BY(g_cs_orphans);
    /** Offset into vExtraTxnForCompact to insert the next tx */
    size_t vExtraTxnForCompactIt GUARDED_BY(g_cs_orphans) = 0;

    /** Check whether the last unknown block a peer advertised is not yet known. */
    void ProcessBlockAvailability(NodeId nodeid) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    /** Update tracking information about which blocks a peer is assumed to have. */
    void UpdateBlockAvailability(NodeId nodeid, const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    bool CanDirectFetch() EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /**
     * To prevent fingerprinting attacks, only send blocks/headers outside of
     * the active chain if they are no more than a month older (both in time,
     * and in best equivalent proof of work) than the best header chain we know
     * about and we fully-validated them at some point.
     */
    bool BlockRequestAllowed(const CBlockIndex* pindex) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    bool AlreadyHaveBlock(const uint256& block_hash) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    void ProcessGetBlockData(CNode& pfrom, Peer& peer, const CInv& inv)
        EXCLUSIVE_LOCKS_REQUIRED(!m_most_recent_block_mutex);

    /**
     * Validation logic for compact filters request handling.
     *
     * May disconnect from the peer in the case of a bad request.
     *
     * @param[in]   node            The node that we received the request from
     * @param[in]   peer            The peer that we received the request from
     * @param[in]   filter_type     The filter type the request is for. Must be basic filters.
     * @param[in]   start_height    The start height for the request
     * @param[in]   stop_hash       The stop_hash for the request
     * @param[in]   max_height_diff The maximum number of items permitted to request, as specified in BIP 157
     * @param[out]  stop_index      The CBlockIndex for the stop_hash block, if the request can be serviced.
     * @param[out]  filter_index    The filter index, if the request can be serviced.
     * @return                      True if the request can be serviced.
     */
    bool PrepareBlockFilterRequest(CNode& node, Peer& peer,
                                   BlockFilterType filter_type, uint32_t start_height,
                                   const uint256& stop_hash, uint32_t max_height_diff,
                                   const CBlockIndex*& stop_index,
                                   BlockFilterIndex*& filter_index);

    /**
     * Handle a cfilters request.
     *
     * May disconnect from the peer in the case of a bad request.
     *
     * @param[in]   node            The node that we received the request from
     * @param[in]   peer            The peer that we received the request from
     * @param[in]   vRecv           The raw message received
     */
    void ProcessGetCFilters(CNode& node, Peer& peer, CDataStream& vRecv);

    /**
     * Handle a cfheaders request.
     *
     * May disconnect from the peer in the case of a bad request.
     *
     * @param[in]   node            The node that we received the request from
     * @param[in]   peer            The peer that we received the request from
     * @param[in]   vRecv           The raw message received
     */
    void ProcessGetCFHeaders(CNode& node, Peer& peer, CDataStream& vRecv);

    /**
     * Handle a getcfcheckpt request.
     *
     * May disconnect from the peer in the case of a bad request.
     *
     * @param[in]   node            The node that we received the request from
     * @param[in]   peer            The peer that we received the request from
     * @param[in]   vRecv           The raw message received
     */
    void ProcessGetCFCheckPt(CNode& node, Peer& peer, CDataStream& vRecv);

    /** Checks if address relay is permitted with peer. If needed, initializes
     * the m_addr_known bloom filter and sets m_addr_relay_enabled to true.
     *
     *  @return   True if address relay is enabled with peer
     *            False if address relay is disallowed
     */
    bool SetupAddressRelay(const CNode& node, Peer& peer) EXCLUSIVE_LOCKS_REQUIRED(g_msgproc_mutex);

    void AddAddressKnown(Peer& peer, const CAddress& addr) EXCLUSIVE_LOCKS_REQUIRED(g_msgproc_mutex);
    void PushAddress(Peer& peer, const CAddress& addr, FastRandomContext& insecure_rand) EXCLUSIVE_LOCKS_REQUIRED(g_msgproc_mutex);
};

const CNodeState* PeerManagerImpl::State(NodeId pnode) const EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    std::map<NodeId, CNodeState>::const_iterator it = m_node_states.find(pnode);
    if (it == m_node_states.end())
        return nullptr;
    return &it->second;
}

CNodeState* PeerManagerImpl::State(NodeId pnode) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    return const_cast<CNodeState*>(std::as_const(*this).State(pnode));
}

/**
 * Whether the peer supports the address. For example, a peer that does not
 * implement BIP155 cannot receive Tor v3 addresses because it requires
 * ADDRv2 (BIP155) encoding.
 */
static bool IsAddrCompatible(const Peer& peer, const CAddress& addr)
{
    return peer.m_wants_addrv2 || addr.IsAddrV1Compatible();
}

void PeerManagerImpl::AddAddressKnown(Peer& peer, const CAddress& addr)
{
    assert(peer.m_addr_known);
    peer.m_addr_known->insert(addr.GetKey());
}

void PeerManagerImpl::PushAddress(Peer& peer, const CAddress& addr, FastRandomContext& insecure_rand)
{
    // Known checking here is only to save space from duplicates.
    // Before sending, we'll filter it again for known addresses that were
    // added after addresses were pushed.
    assert(peer.m_addr_known);
    if (addr.IsValid() && !peer.m_addr_known->contains(addr.GetKey()) && IsAddrCompatible(peer, addr)) {
        if (peer.m_addrs_to_send.size() >= MAX_ADDR_TO_SEND) {
            peer.m_addrs_to_send[insecure_rand.randrange(peer.m_addrs_to_send.size())] = addr;
        } else {
            peer.m_addrs_to_send.push_back(addr);
        }
    }
}

static void AddKnownTx(Peer& peer, const uint256& hash)
{
    auto tx_relay = peer.GetTxRelay();
    if (!tx_relay) return;

    LOCK(tx_relay->m_tx_inventory_mutex);
    tx_relay->m_tx_inventory_known_filter.insert(hash);
}

/** Whether this peer can serve us blocks. */
static bool CanServeBlocks(const Peer& peer)
{
    return peer.m_their_services & (NODE_NETWORK|NODE_NETWORK_LIMITED);
}

/** Whether this peer can only serve limited recent blocks (e.g. because
 *  it prunes old blocks) */
static bool IsLimitedPeer(const Peer& peer)
{
    return (!(peer.m_their_services & NODE_NETWORK) &&
             (peer.m_their_services & NODE_NETWORK_LIMITED));
}

/** Whether this peer can serve us witness data */
static bool CanServeWitnesses(const Peer& peer)
{
    return peer.m_their_services & NODE_WITNESS;
}

std::chrono::microseconds PeerManagerImpl::NextInvToInbounds(std::chrono::microseconds now,
                                                             std::chrono::seconds average_interval)
{
    if (m_next_inv_to_inbounds.load() < now) {
        // If this function were called from multiple threads simultaneously
        // it would possible that both update the next send variable, and return a different result to their caller.
        // This is not possible in practice as only the net processing thread invokes this function.
        m_next_inv_to_inbounds = GetExponentialRand(now, average_interval);
    }
    return m_next_inv_to_inbounds;
}

bool PeerManagerImpl::IsBlockRequested(const uint256& hash)
{
    return mapBlocksInFlight.find(hash) != mapBlocksInFlight.end();
}

void PeerManagerImpl::RemoveBlockRequest(const uint256& hash)
{
    auto it = mapBlocksInFlight.find(hash);
    if (it == mapBlocksInFlight.end()) {
        // Block was not requested
        return;
    }

    auto [node_id, list_it] = it->second;
    CNodeState *state = State(node_id);
    assert(state != nullptr);

    if (state->vBlocksInFlight.begin() == list_it) {
        // First block on the queue was received, update the start download time for the next one
        state->m_downloading_since = std::max(state->m_downloading_since, GetTime<std::chrono::microseconds>());
    }
    state->vBlocksInFlight.erase(list_it);

    state->nBlocksInFlight--;
    if (state->nBlocksInFlight == 0) {
        // Last validated block on the queue was received.
        m_peers_downloading_from--;
    }
    state->m_stalling_since = 0us;
    mapBlocksInFlight.erase(it);
}

bool PeerManagerImpl::BlockRequested(NodeId nodeid, const CBlockIndex& block, std::list<QueuedBlock>::iterator** pit)
{
    const uint256& hash{block.GetBlockHash()};

    CNodeState *state = State(nodeid);
    assert(state != nullptr);

    // Short-circuit most stuff in case it is from the same node
    std::map<uint256, std::pair<NodeId, std::list<QueuedBlock>::iterator> >::iterator itInFlight = mapBlocksInFlight.find(hash);
    if (itInFlight != mapBlocksInFlight.end() && itInFlight->second.first == nodeid) {
        if (pit) {
            *pit = &itInFlight->second.second;
        }
        return false;
    }

    // Make sure it's not listed somewhere already.
    RemoveBlockRequest(hash);

    std::list<QueuedBlock>::iterator it = state->vBlocksInFlight.insert(state->vBlocksInFlight.end(),
            {&block, std::unique_ptr<PartiallyDownloadedBlock>(pit ? new PartiallyDownloadedBlock(&m_mempool) : nullptr)});
    state->nBlocksInFlight++;
    if (state->nBlocksInFlight == 1) {
        // We're starting a block download (batch) from this peer.
        state->m_downloading_since = GetTime<std::chrono::microseconds>();
        m_peers_downloading_from++;
    }
    itInFlight = mapBlocksInFlight.insert(std::make_pair(hash, std::make_pair(nodeid, it))).first;
    if (pit) {
        *pit = &itInFlight->second.second;
    }
    return true;
}

void PeerManagerImpl::MaybeSetPeerAsAnnouncingHeaderAndIDs(NodeId nodeid)
{
    AssertLockHeld(cs_main);

    // When in -blocksonly mode, never request high-bandwidth mode from peers. Our
    // mempool will not contain the transactions necessary to reconstruct the
    // compact block.
    if (m_ignore_incoming_txs) return;

    CNodeState* nodestate = State(nodeid);
    if (!nodestate || !nodestate->m_provides_cmpctblocks) {
        // Don't request compact blocks if the peer has not signalled support
        return;
    }

    int num_outbound_hb_peers = 0;
    for (std::list<NodeId>::iterator it = lNodesAnnouncingHeaderAndIDs.begin(); it != lNodesAnnouncingHeaderAndIDs.end(); it++) {
        if (*it == nodeid) {
            lNodesAnnouncingHeaderAndIDs.erase(it);
            lNodesAnnouncingHeaderAndIDs.push_back(nodeid);
            return;
        }
        CNodeState *state = State(*it);
        if (state != nullptr && !state->m_is_inbound) ++num_outbound_hb_peers;
    }
    if (nodestate->m_is_inbound) {
        // If we're adding an inbound HB peer, make sure we're not removing
        // our last outbound HB peer in the process.
        if (lNodesAnnouncingHeaderAndIDs.size() >= 3 && num_outbound_hb_peers == 1) {
            CNodeState *remove_node = State(lNodesAnnouncingHeaderAndIDs.front());
            if (remove_node != nullptr && !remove_node->m_is_inbound) {
                // Put the HB outbound peer in the second slot, so that it
                // doesn't get removed.
                std::swap(lNodesAnnouncingHeaderAndIDs.front(), *std::next(lNodesAnnouncingHeaderAndIDs.begin()));
            }
        }
    }
    m_connman.ForNode(nodeid, [this](CNode* pfrom) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        AssertLockHeld(::cs_main);
        if (lNodesAnnouncingHeaderAndIDs.size() >= 3) {
            // As per BIP152, we only get 3 of our peers to announce
            // blocks using compact encodings.
            m_connman.ForNode(lNodesAnnouncingHeaderAndIDs.front(), [this](CNode* pnodeStop){
                m_connman.PushMessage(pnodeStop, CNetMsgMaker(pnodeStop->GetCommonVersion()).Make(NetMsgType::SENDCMPCT, /*high_bandwidth=*/false, /*version=*/CMPCTBLOCKS_VERSION));
                // save BIP152 bandwidth state: we select peer to be low-bandwidth
                pnodeStop->m_bip152_highbandwidth_to = false;
                return true;
            });
            lNodesAnnouncingHeaderAndIDs.pop_front();
        }
        m_connman.PushMessage(pfrom, CNetMsgMaker(pfrom->GetCommonVersion()).Make(NetMsgType::SENDCMPCT, /*high_bandwidth=*/true, /*version=*/CMPCTBLOCKS_VERSION));
        // save BIP152 bandwidth state: we select peer to be high-bandwidth
        pfrom->m_bip152_highbandwidth_to = true;
        lNodesAnnouncingHeaderAndIDs.push_back(pfrom->GetId());
        return true;
    });
}

bool PeerManagerImpl::TipMayBeStale()
{
    AssertLockHeld(cs_main);
    const Consensus::Params& consensusParams = m_chainparams.GetConsensus();
    if (m_last_tip_update.load() == 0s) {
        m_last_tip_update = GetTime<std::chrono::seconds>();
    }
    return m_last_tip_update.load() < GetTime<std::chrono::seconds>() - std::chrono::seconds{consensusParams.nPowTargetSpacing * 3} && mapBlocksInFlight.empty();
}

bool PeerManagerImpl::CanDirectFetch()
{
    return m_chainman.ActiveChain().Tip()->Time() > GetAdjustedTime() - m_chainparams.GetConsensus().PowTargetSpacing() * 20;
}

static bool PeerHasHeader(CNodeState *state, const CBlockIndex *pindex) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    if (state->pindexBestKnownBlock && pindex == state->pindexBestKnownBlock->GetAncestor(pindex->nHeight))
        return true;
    if (state->pindexBestHeaderSent && pindex == state->pindexBestHeaderSent->GetAncestor(pindex->nHeight))
        return true;
    return false;
}

void PeerManagerImpl::ProcessBlockAvailability(NodeId nodeid) {
    CNodeState *state = State(nodeid);
    assert(state != nullptr);

    if (!state->hashLastUnknownBlock.IsNull()) {
        const CBlockIndex* pindex = m_chainman.m_blockman.LookupBlockIndex(state->hashLastUnknownBlock);
        if (pindex && pindex->nChainWork > 0) {
            if (state->pindexBestKnownBlock == nullptr || pindex->nChainWork >= state->pindexBestKnownBlock->nChainWork) {
                state->pindexBestKnownBlock = pindex;
            }
            state->hashLastUnknownBlock.SetNull();
        }
    }
}

void PeerManagerImpl::UpdateBlockAvailability(NodeId nodeid, const uint256 &hash) {
    CNodeState *state = State(nodeid);
    assert(state != nullptr);

    ProcessBlockAvailability(nodeid);

    const CBlockIndex* pindex = m_chainman.m_blockman.LookupBlockIndex(hash);
    if (pindex && pindex->nChainWork > 0) {
        // An actually better block was announced.
        if (state->pindexBestKnownBlock == nullptr || pindex->nChainWork >= state->pindexBestKnownBlock->nChainWork) {
            state->pindexBestKnownBlock = pindex;
        }
    } else {
        // An unknown block was announced; just assume that the latest one is the best one.
        state->hashLastUnknownBlock = hash;
    }
}

void PeerManagerImpl::FindNextBlocksToDownload(const Peer& peer, unsigned int count, std::vector<const CBlockIndex*>& vBlocks, NodeId& nodeStaller)
{
    if (count == 0)
        return;

    vBlocks.reserve(vBlocks.size() + count);
    CNodeState *state = State(peer.m_id);
    assert(state != nullptr);

    // Make sure pindexBestKnownBlock is up to date, we'll need it.
    ProcessBlockAvailability(peer.m_id);

    if (state->pindexBestKnownBlock == nullptr || state->pindexBestKnownBlock->nChainWork < m_chainman.ActiveChain().Tip()->nChainWork || state->pindexBestKnownBlock->nChainWork < nMinimumChainWork) {
        // This peer has nothing interesting.
        return;
    }

    if (state->pindexLastCommonBlock == nullptr) {
        // Bootstrap quickly by guessing a parent of our best tip is the forking point.
        // Guessing wrong in either direction is not a problem.
        state->pindexLastCommonBlock = m_chainman.ActiveChain()[std::min(state->pindexBestKnownBlock->nHeight, m_chainman.ActiveChain().Height())];
    }

    // If the peer reorganized, our previous pindexLastCommonBlock may not be an ancestor
    // of its current tip anymore. Go back enough to fix that.
    state->pindexLastCommonBlock = LastCommonAncestor(state->pindexLastCommonBlock, state->pindexBestKnownBlock);
    if (state->pindexLastCommonBlock == state->pindexBestKnownBlock)
        return;

    std::vector<const CBlockIndex*> vToFetch;
    const CBlockIndex *pindexWalk = state->pindexLastCommonBlock;
    // Never fetch further than the best block we know the peer has, or more than BLOCK_DOWNLOAD_WINDOW + 1 beyond the last
    // linked block we have in common with this peer. The +1 is so we can detect stalling, namely if we would be able to
    // download that next block if the window were 1 larger.
    int nWindowEnd = state->pindexLastCommonBlock->nHeight + BLOCK_DOWNLOAD_WINDOW;
    int nMaxHeight = std::min<int>(state->pindexBestKnownBlock->nHeight, nWindowEnd + 1);
    NodeId waitingfor = -1;
    while (pindexWalk->nHeight < nMaxHeight) {
        // Read up to 128 (or more, if more blocks than that are needed) successors of pindexWalk (towards
        // pindexBestKnownBlock) into vToFetch. We fetch 128, because CBlockIndex::GetAncestor may be as expensive
        // as iterating over ~100 CBlockIndex* entries anyway.
        int nToFetch = std::min(nMaxHeight - pindexWalk->nHeight, std::max<int>(count - vBlocks.size(), 128));
        vToFetch.resize(nToFetch);
        pindexWalk = state->pindexBestKnownBlock->GetAncestor(pindexWalk->nHeight + nToFetch);
        vToFetch[nToFetch - 1] = pindexWalk;
        for (unsigned int i = nToFetch - 1; i > 0; i--) {
            vToFetch[i - 1] = vToFetch[i]->pprev;
        }

        // Iterate over those blocks in vToFetch (in forward direction), adding the ones that
        // are not yet downloaded and not in flight to vBlocks. In the meantime, update
        // pindexLastCommonBlock as long as all ancestors are already downloaded, or if it's
        // already part of our chain (and therefore don't need it even if pruned).
        for (const CBlockIndex* pindex : vToFetch) {
            if (!pindex->IsValid(BLOCK_VALID_TREE)) {
                // We consider the chain that this peer is on invalid.
                return;
            }
            if (!CanServeWitnesses(peer) && DeploymentActiveAt(*pindex, m_chainman, Consensus::DEPLOYMENT_SEGWIT)) {
                // We wouldn't download this block or its descendants from this peer.
                return;
            }
            if (pindex->nStatus & BLOCK_HAVE_DATA || m_chainman.ActiveChain().Contains(pindex)) {
                if (pindex->HaveTxsDownloaded())
                    state->pindexLastCommonBlock = pindex;
            } else if (!IsBlockRequested(pindex->GetBlockHash())) {
                // The block is not already downloaded, and not yet in flight.
                if (pindex->nHeight > nWindowEnd) {
                    // We reached the end of the window.
                    if (vBlocks.size() == 0 && waitingfor != peer.m_id) {
                        // We aren't able to fetch anything, but we would be if the download window was one larger.
                        nodeStaller = waitingfor;
                    }
                    return;
                }
                vBlocks.push_back(pindex);
                if (vBlocks.size() == count) {
                    return;
                }
            } else if (waitingfor == -1) {
                // This is the first already-in-flight block.
                waitingfor = mapBlocksInFlight[pindex->GetBlockHash()].first;
            }
        }
    }
}

} // namespace

void PeerManagerImpl::PushNodeVersion(CNode& pnode, const Peer& peer)
{
    uint64_t my_services{peer.m_our_services};
    const int64_t nTime{count_seconds(GetTime<std::chrono::seconds>())};
    uint64_t nonce = pnode.GetLocalNonce();
    const int nNodeStartingHeight{m_best_height};
    NodeId nodeid = pnode.GetId();
    CAddress addr = pnode.addr;

    CService addr_you = addr.IsRoutable() && !IsProxy(addr) && addr.IsAddrV1Compatible() ? addr : CService();
    uint64_t your_services{addr.nServices};

    const bool tx_relay = !m_ignore_incoming_txs && !pnode.IsBlockOnlyConn() && !pnode.IsFeelerConn();
    m_connman.PushMessage(&pnode, CNetMsgMaker(INIT_PROTO_VERSION).Make(NetMsgType::VERSION, PROTOCOL_VERSION, my_services, nTime,
            your_services, addr_you, // Together the pre-version-31402 serialization of CAddress "addrYou" (without nTime)
            my_services, CService(), // Together the pre-version-31402 serialization of CAddress "addrMe" (without nTime)
            nonce, strSubVersion, nNodeStartingHeight, tx_relay));

    if (fLogIPs) {
        LogPrint(BCLog::NET, "send version message: version %d, blocks=%d, them=%s, txrelay=%d, peer=%d\n", PROTOCOL_VERSION, nNodeStartingHeight, addr_you.ToString(), tx_relay, nodeid);
    } else {
        LogPrint(BCLog::NET, "send version message: version %d, blocks=%d, txrelay=%d, peer=%d\n", PROTOCOL_VERSION, nNodeStartingHeight, tx_relay, nodeid);
    }
}

void PeerManagerImpl::AddTxAnnouncement(const CNode& node, const GenTxid& gtxid, std::chrono::microseconds current_time)
{
    AssertLockHeld(::cs_main); // For m_txrequest
    NodeId nodeid = node.GetId();
    if (!node.HasPermission(NetPermissionFlags::Relay) && m_txrequest.Count(nodeid) >= MAX_PEER_TX_ANNOUNCEMENTS) {
        // Too many queued announcements from this peer
        return;
    }
    const CNodeState* state = State(nodeid);

    // Decide the TxRequestTracker parameters for this announcement:
    // - "preferred": if fPreferredDownload is set (= outbound, or NetPermissionFlags::NoBan permission)
    // - "reqtime": current time plus delays for:
    //   - NONPREF_PEER_TX_DELAY for announcements from non-preferred connections
    //   - TXID_RELAY_DELAY for txid announcements while wtxid peers are available
    //   - OVERLOADED_PEER_TX_DELAY for announcements from peers which have at least
    //     MAX_PEER_TX_REQUEST_IN_FLIGHT requests in flight (and don't have NetPermissionFlags::Relay).
    auto delay{0us};
    const bool preferred = state->fPreferredDownload;
    if (!preferred) delay += NONPREF_PEER_TX_DELAY;
    if (!gtxid.IsWtxid() && m_wtxid_relay_peers > 0) delay += TXID_RELAY_DELAY;
    const bool overloaded = !node.HasPermission(NetPermissionFlags::Relay) &&
        m_txrequest.CountInFlight(nodeid) >= MAX_PEER_TX_REQUEST_IN_FLIGHT;
    if (overloaded) delay += OVERLOADED_PEER_TX_DELAY;
    m_txrequest.ReceivedInv(nodeid, gtxid, preferred, current_time + delay);
}

void PeerManagerImpl::UpdateLastBlockAnnounceTime(NodeId node, int64_t time_in_seconds)
{
    LOCK(cs_main);
    CNodeState *state = State(node);
    if (state) state->m_last_block_announcement = time_in_seconds;
}

void PeerManagerImpl::InitializeNode(CNode& node, ServiceFlags our_services)
{
    NodeId nodeid = node.GetId();
    {
        LOCK(cs_main);
        m_node_states.emplace_hint(m_node_states.end(), std::piecewise_construct, std::forward_as_tuple(nodeid), std::forward_as_tuple(node.IsInboundConn()));
        assert(m_txrequest.Count(nodeid) == 0);
    }
    PeerRef peer = std::make_shared<Peer>(nodeid, our_services);
    {
        LOCK(m_peer_mutex);
        m_peer_map.emplace_hint(m_peer_map.end(), nodeid, peer);
    }
    if (!node.IsInboundConn()) {
        PushNodeVersion(node, *peer);
    }
}

void PeerManagerImpl::ReattemptInitialBroadcast(CScheduler& scheduler)
{
    std::set<uint256> unbroadcast_txids = m_mempool.GetUnbroadcastTxs();

    for (const auto& txid : unbroadcast_txids) {
        CTransactionRef tx = m_mempool.get(txid);

        if (tx != nullptr) {
            RelayTransaction(txid, tx->GetWitnessHash());
        } else {
            m_mempool.RemoveUnbroadcastTx(txid, true);
        }
    }

    // Schedule next run for 10-15 minutes in the future.
    // We add randomness on every cycle to avoid the possibility of P2P fingerprinting.
    const std::chrono::milliseconds delta = 10min + GetRandMillis(5min);
    scheduler.scheduleFromNow([&] { ReattemptInitialBroadcast(scheduler); }, delta);
}

void PeerManagerImpl::FinalizeNode(const CNode& node)
{
    NodeId nodeid = node.GetId();
    int misbehavior{0};
    {
    LOCK(cs_main);
    {
        // We remove the PeerRef from g_peer_map here, but we don't always
        // destruct the Peer. Sometimes another thread is still holding a
        // PeerRef, so the refcount is >= 1. Be careful not to do any
        // processing here that assumes Peer won't be changed before it's
        // destructed.
        PeerRef peer = RemovePeer(nodeid);
        assert(peer != nullptr);
        misbehavior = WITH_LOCK(peer->m_misbehavior_mutex, return peer->m_misbehavior_score);
        m_wtxid_relay_peers -= peer->m_wtxid_relay;
        assert(m_wtxid_relay_peers >= 0);
    }
    CNodeState *state = State(nodeid);
    assert(state != nullptr);

    if (state->fSyncStarted)
        nSyncStarted--;

    for (const QueuedBlock& entry : state->vBlocksInFlight) {
        mapBlocksInFlight.erase(entry.pindex->GetBlockHash());
    }
    WITH_LOCK(g_cs_orphans, m_orphanage.EraseForPeer(nodeid));
    m_txrequest.DisconnectedPeer(nodeid);
    m_num_preferred_download_peers -= state->fPreferredDownload;
    m_peers_downloading_from -= (state->nBlocksInFlight != 0);
    assert(m_peers_downloading_from >= 0);
    m_outbound_peers_with_protect_from_disconnect -= state->m_chain_sync.m_protect;
    assert(m_outbound_peers_with_protect_from_disconnect >= 0);

    m_node_states.erase(nodeid);

    if (m_node_states.empty()) {
        // Do a consistency check after the last peer is removed.
        assert(mapBlocksInFlight.empty());
        assert(m_num_preferred_download_peers == 0);
        assert(m_peers_downloading_from == 0);
        assert(m_outbound_peers_with_protect_from_disconnect == 0);
        assert(m_wtxid_relay_peers == 0);
        assert(m_txrequest.Size() == 0);
        assert(m_orphanage.Size() == 0);
    }
    } // cs_main
    if (node.fSuccessfullyConnected && misbehavior == 0 &&
        !node.IsBlockOnlyConn() && !node.IsInboundConn()) {
        // Only change visible addrman state for full outbound peers.  We don't
        // call Connected() for feeler connections since they don't have
        // fSuccessfullyConnected set.
        m_addrman.Connected(node.addr);
    }
    {
        LOCK(m_headers_presync_mutex);
        m_headers_presync_stats.erase(nodeid);
    }
    LogPrint(BCLog::NET, "Cleared nodestate for peer=%d\n", nodeid);
}

PeerRef PeerManagerImpl::GetPeerRef(NodeId id) const
{
    LOCK(m_peer_mutex);
    auto it = m_peer_map.find(id);
    return it != m_peer_map.end() ? it->second : nullptr;
}

PeerRef PeerManagerImpl::RemovePeer(NodeId id)
{
    PeerRef ret;
    LOCK(m_peer_mutex);
    auto it = m_peer_map.find(id);
    if (it != m_peer_map.end()) {
        ret = std::move(it->second);
        m_peer_map.erase(it);
    }
    return ret;
}

bool PeerManagerImpl::GetNodeStateStats(NodeId nodeid, CNodeStateStats& stats) const
{
    {
        LOCK(cs_main);
        const CNodeState* state = State(nodeid);
        if (state == nullptr)
            return false;
        stats.nSyncHeight = state->pindexBestKnownBlock ? state->pindexBestKnownBlock->nHeight : -1;
        stats.nCommonHeight = state->pindexLastCommonBlock ? state->pindexLastCommonBlock->nHeight : -1;
        for (const QueuedBlock& queue : state->vBlocksInFlight) {
            if (queue.pindex)
                stats.vHeightInFlight.push_back(queue.pindex->nHeight);
        }
    }

    PeerRef peer = GetPeerRef(nodeid);
    if (peer == nullptr) return false;
    stats.their_services = peer->m_their_services;
    stats.m_starting_height = peer->m_starting_height;
    // It is common for nodes with good ping times to suddenly become lagged,
    // due to a new block arriving or other large transfer.
    // Merely reporting pingtime might fool the caller into thinking the node was still responsive,
    // since pingtime does not update until the ping is complete, which might take a while.
    // So, if a ping is taking an unusually long time in flight,
    // the caller can immediately detect that this is happening.
    auto ping_wait{0us};
    if ((0 != peer->m_ping_nonce_sent) && (0 != peer->m_ping_start.load().count())) {
        ping_wait = GetTime<std::chrono::microseconds>() - peer->m_ping_start.load();
    }

    if (auto tx_relay = peer->GetTxRelay(); tx_relay != nullptr) {
        stats.m_relay_txs = WITH_LOCK(tx_relay->m_bloom_filter_mutex, return tx_relay->m_relay_txs);
        stats.m_fee_filter_received = tx_relay->m_fee_filter_received.load();
    } else {
        stats.m_relay_txs = false;
        stats.m_fee_filter_received = 0;
    }

    stats.m_ping_wait = ping_wait;
    stats.m_addr_processed = peer->m_addr_processed.load();
    stats.m_addr_rate_limited = peer->m_addr_rate_limited.load();
    stats.m_addr_relay_enabled = peer->m_addr_relay_enabled.load();
    {
        LOCK(peer->m_headers_sync_mutex);
        if (peer->m_headers_sync) {
            stats.presync_height = peer->m_headers_sync->GetPresyncHeight();
        }
    }

    return true;
}

void PeerManagerImpl::AddToCompactExtraTransactions(const CTransactionRef& tx)
{
    size_t max_extra_txn = gArgs.GetIntArg("-blockreconstructionextratxn", DEFAULT_BLOCK_RECONSTRUCTION_EXTRA_TXN);
    if (max_extra_txn <= 0)
        return;
    if (!vExtraTxnForCompact.size())
        vExtraTxnForCompact.resize(max_extra_txn);
    vExtraTxnForCompact[vExtraTxnForCompactIt] = std::make_pair(tx->GetWitnessHash(), tx);
    vExtraTxnForCompactIt = (vExtraTxnForCompactIt + 1) % max_extra_txn;
}

void PeerManagerImpl::Misbehaving(Peer& peer, int howmuch, const std::string& message)
{
    assert(howmuch > 0);

    LOCK(peer.m_misbehavior_mutex);
    const int score_before{peer.m_misbehavior_score};
    peer.m_misbehavior_score += howmuch;
    const int score_now{peer.m_misbehavior_score};

    const std::string message_prefixed = message.empty() ? "" : (": " + message);
    std::string warning;

    if (score_now >= DISCOURAGEMENT_THRESHOLD && score_before < DISCOURAGEMENT_THRESHOLD) {
        warning = " DISCOURAGE THRESHOLD EXCEEDED";
        peer.m_should_discourage = true;
    }

    LogPrint(BCLog::NET, "Misbehaving: peer=%d (%d -> %d)%s%s\n",
             peer.m_id, score_before, score_now, warning, message_prefixed);
}

bool PeerManagerImpl::MaybePunishNodeForBlock(NodeId nodeid, const BlockValidationState& state,
                                              bool via_compact_block, const std::string& message)
{
    PeerRef peer{GetPeerRef(nodeid)};
    switch (state.GetResult()) {
    case BlockValidationResult::BLOCK_RESULT_UNSET:
        break;
    case BlockValidationResult::BLOCK_HEADER_LOW_WORK:
        // We didn't try to process the block because the header chain may have
        // too little work.
        break;
    // The node is providing invalid data:
    case BlockValidationResult::BLOCK_CONSENSUS:
    case BlockValidationResult::BLOCK_MUTATED:
        if (!via_compact_block) {
            if (peer) Misbehaving(*peer, 100, message);
            return true;
        }
        break;
    case BlockValidationResult::BLOCK_CACHED_INVALID:
        {
            LOCK(cs_main);
            CNodeState *node_state = State(nodeid);
            if (node_state == nullptr) {
                break;
            }

            // Discourage outbound (but not inbound) peers if on an invalid chain.
            // Exempt HB compact block peers. Manual connections are always protected from discouragement.
            if (!via_compact_block && !node_state->m_is_inbound) {
                if (peer) Misbehaving(*peer, 100, message);
                return true;
            }
            break;
        }
    case BlockValidationResult::BLOCK_INVALID_HEADER:
    case BlockValidationResult::BLOCK_CHECKPOINT:
    case BlockValidationResult::BLOCK_INVALID_PREV:
        if (peer) Misbehaving(*peer, 100, message);
        return true;
    // Conflicting (but not necessarily invalid) data or different policy:
    case BlockValidationResult::BLOCK_MISSING_PREV:
        // TODO: Handle this much more gracefully (10 DoS points is super arbitrary)
        if (peer) Misbehaving(*peer, 10, message);
        return true;
    case BlockValidationResult::BLOCK_RECENT_CONSENSUS_CHANGE:
    case BlockValidationResult::BLOCK_TIME_FUTURE:
        break;
    }
    if (message != "") {
        LogPrint(BCLog::NET, "peer=%d: %s\n", nodeid, message);
    }
    return false;
}

bool PeerManagerImpl::MaybePunishNodeForTx(NodeId nodeid, const TxValidationState& state, const std::string& message)
{
    PeerRef peer{GetPeerRef(nodeid)};
    switch (state.GetResult()) {
    case TxValidationResult::TX_RESULT_UNSET:
        break;
    // The node is providing invalid data:
    case TxValidationResult::TX_CONSENSUS:
        if (peer) Misbehaving(*peer, 100, message);
        return true;
    // Conflicting (but not necessarily invalid) data or different policy:
    case TxValidationResult::TX_RECENT_CONSENSUS_CHANGE:
    case TxValidationResult::TX_INPUTS_NOT_STANDARD:
    case TxValidationResult::TX_NOT_STANDARD:
    case TxValidationResult::TX_MISSING_INPUTS:
    case TxValidationResult::TX_PREMATURE_SPEND:
    case TxValidationResult::TX_WITNESS_MUTATED:
    case TxValidationResult::TX_WITNESS_STRIPPED:
    case TxValidationResult::TX_CONFLICT:
    case TxValidationResult::TX_MEMPOOL_POLICY:
    case TxValidationResult::TX_NO_MEMPOOL:
        break;
    }
    if (message != "") {
        LogPrint(BCLog::NET, "peer=%d: %s\n", nodeid, message);
    }
    return false;
}

bool PeerManagerImpl::BlockRequestAllowed(const CBlockIndex* pindex)
{
    AssertLockHeld(cs_main);
    if (m_chainman.ActiveChain().Contains(pindex)) return true;
    return pindex->IsValid(BLOCK_VALID_SCRIPTS) && (m_chainman.m_best_header != nullptr) &&
           (m_chainman.m_best_header->GetBlockTime() - pindex->GetBlockTime() < STALE_RELAY_AGE_LIMIT) &&
           (GetBlockProofEquivalentTime(*m_chainman.m_best_header, *pindex, *m_chainman.m_best_header, m_chainparams.GetConsensus()) < STALE_RELAY_AGE_LIMIT);
}

std::optional<std::string> PeerManagerImpl::FetchBlock(NodeId peer_id, const CBlockIndex& block_index)
{
    if (fImporting) return "Importing...";
    if (fReindex) return "Reindexing...";

    // Ensure this peer exists and hasn't been disconnected
    PeerRef peer = GetPeerRef(peer_id);
    if (peer == nullptr) return "Peer does not exist";

    // Ignore pre-segwit peers
    if (!CanServeWitnesses(*peer)) return "Pre-SegWit peer";

    LOCK(cs_main);

    // Mark block as in-flight unless it already is (for this peer).
    // If a block was already in-flight for a different peer, its BLOCKTXN
    // response will be dropped.
    if (!BlockRequested(peer_id, block_index)) return "Already requested from this peer";

    // Construct message to request the block
    const uint256& hash{block_index.GetBlockHash()};
    std::vector<CInv> invs{CInv(MSG_BLOCK | MSG_WITNESS_FLAG, hash)};

    // Send block request message to the peer
    bool success = m_connman.ForNode(peer_id, [this, &invs](CNode* node) {
        const CNetMsgMaker msgMaker(node->GetCommonVersion());
        this->m_connman.PushMessage(node, msgMaker.Make(NetMsgType::GETDATA, invs));
        return true;
    });

    if (!success) return "Peer not fully connected";

    LogPrint(BCLog::NET, "Requesting block %s from peer=%d\n",
                 hash.ToString(), peer_id);
    return std::nullopt;
}

std::unique_ptr<PeerManager> PeerManager::make(CConnman& connman, AddrMan& addrman,
                                               BanMan* banman, ChainstateManager& chainman,
                                               CTxMemPool& pool, bool ignore_incoming_txs)
{
    return std::make_unique<PeerManagerImpl>(connman, addrman, banman, chainman, pool, ignore_incoming_txs);
}

PeerManagerImpl::PeerManagerImpl(CConnman& connman, AddrMan& addrman,
                                 BanMan* banman, ChainstateManager& chainman,
                                 CTxMemPool& pool, bool ignore_incoming_txs)
    : m_chainparams(chainman.GetParams()),
      m_connman(connman),
      m_addrman(addrman),
      m_banman(banman),
      m_chainman(chainman),
      m_mempool(pool),
      m_ignore_incoming_txs(ignore_incoming_txs)
{
}

void PeerManagerImpl::StartScheduledTasks(CScheduler& scheduler)
{
    // Stale tip checking and peer eviction are on two different timers, but we
    // don't want them to get out of sync due to drift in the scheduler, so we
    // combine them in one function and schedule at the quicker (peer-eviction)
    // timer.
    static_assert(EXTRA_PEER_CHECK_INTERVAL < STALE_CHECK_INTERVAL, "peer eviction timer should be less than stale tip check timer");
    scheduler.scheduleEvery([this] { this->CheckForStaleTipAndEvictPeers(); }, std::chrono::seconds{EXTRA_PEER_CHECK_INTERVAL});

    // schedule next run for 10-15 minutes in the future
    const std::chrono::milliseconds delta = 10min + GetRandMillis(5min);
    scheduler.scheduleFromNow([&] { ReattemptInitialBroadcast(scheduler); }, delta);
}

/**
 * Evict orphan txn pool entries based on a newly connected
 * block, remember the recently confirmed transactions, and delete tracked
 * announcements for them. Also save the time of the last tip update.
 */
void PeerManagerImpl::BlockConnected(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex* pindex)
{
    m_orphanage.EraseForBlock(*pblock);
    m_last_tip_update = GetTime<std::chrono::seconds>();

    {
        LOCK(m_recent_confirmed_transactions_mutex);
        for (const auto& ptx : pblock->vtx) {
            m_recent_confirmed_transactions.insert(ptx->GetHash());
            if (ptx->GetHash() != ptx->GetWitnessHash()) {
                m_recent_confirmed_transactions.insert(ptx->GetWitnessHash());
            }
        }
    }
    {
        LOCK(cs_main);
        for (const auto& ptx : pblock->vtx) {
            m_txrequest.ForgetTxHash(ptx->GetHash());
            m_txrequest.ForgetTxHash(ptx->GetWitnessHash());
        }
    }
}

void PeerManagerImpl::BlockDisconnected(const std::shared_ptr<const CBlock> &block, const CBlockIndex* pindex)
{
    // To avoid relay problems with transactions that were previously
    // confirmed, clear our filter of recently confirmed transactions whenever
    // there's a reorg.
    // This means that in a 1-block reorg (where 1 block is disconnected and
    // then another block reconnected), our filter will drop to having only one
    // block's worth of transactions in it, but that should be fine, since
    // presumably the most common case of relaying a confirmed transaction
    // should be just after a new block containing it is found.
    LOCK(m_recent_confirmed_transactions_mutex);
    m_recent_confirmed_transactions.reset();
}

/**
 * Maintain state about the best-seen block and fast-announce a compact block
 * to compatible peers.
 */
void PeerManagerImpl::NewPoWValidBlock(const CBlockIndex *pindex, const std::shared_ptr<const CBlock>& pblock)
{
    auto pcmpctblock = std::make_shared<const CBlockHeaderAndShortTxIDs>(*pblock);
    const CNetMsgMaker msgMaker(PROTOCOL_VERSION);

    LOCK(cs_main);

    if (pindex->nHeight <= m_highest_fast_announce)
        return;
    m_highest_fast_announce = pindex->nHeight;

    if (!DeploymentActiveAt(*pindex, m_chainman, Consensus::DEPLOYMENT_SEGWIT)) return;

    uint256 hashBlock(pblock->GetHash());
    const std::shared_future<CSerializedNetMsg> lazy_ser{
        std::async(std::launch::deferred, [&] { return msgMaker.Make(NetMsgType::CMPCTBLOCK, *pcmpctblock); })};

    {
        LOCK(m_most_recent_block_mutex);
        m_most_recent_block_hash = hashBlock;
        m_most_recent_block = pblock;
        m_most_recent_compact_block = pcmpctblock;
    }

    m_connman.ForEachNode([this, pindex, &lazy_ser, &hashBlock](CNode* pnode) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        AssertLockHeld(::cs_main);

        if (pnode->GetCommonVersion() < INVALID_CB_NO_BAN_VERSION || pnode->fDisconnect)
            return;
        ProcessBlockAvailability(pnode->GetId());
        CNodeState &state = *State(pnode->GetId());
        // If the peer has, or we announced to them the previous block already,
        // but we don't think they have this one, go ahead and announce it
        if (state.m_requested_hb_cmpctblocks && !PeerHasHeader(&state, pindex) && PeerHasHeader(&state, pindex->pprev)) {

            LogPrint(BCLog::NET, "%s sending header-and-ids %s to peer=%d\n", "PeerManager::NewPoWValidBlock",
                    hashBlock.ToString(), pnode->GetId());

            const CSerializedNetMsg& ser_cmpctblock{lazy_ser.get()};
            m_connman.PushMessage(pnode, ser_cmpctblock.Copy());
            state.pindexBestHeaderSent = pindex;
        }
    });
}

/**
 * Update our best height and announce any block hashes which weren't previously
 * in m_chainman.ActiveChain() to our peers.
 */
void PeerManagerImpl::UpdatedBlockTip(const CBlockIndex *pindexNew, const CBlockIndex *pindexFork, bool fInitialDownload)
{
    SetBestHeight(pindexNew->nHeight);
    SetServiceFlagsIBDCache(!fInitialDownload);

    // Don't relay inventory during initial block download.
    if (fInitialDownload) return;

    // Find the hashes of all blocks that weren't previously in the best chain.
    std::vector<uint256> vHashes;
    const CBlockIndex *pindexToAnnounce = pindexNew;
    while (pindexToAnnounce != pindexFork) {
        vHashes.push_back(pindexToAnnounce->GetBlockHash());
        pindexToAnnounce = pindexToAnnounce->pprev;
        if (vHashes.size() == MAX_BLOCKS_TO_ANNOUNCE) {
            // Limit announcements in case of a huge reorganization.
            // Rely on the peer's synchronization mechanism in that case.
            break;
        }
    }

    {
        LOCK(m_peer_mutex);
        for (auto& it : m_peer_map) {
            Peer& peer = *it.second;
            LOCK(peer.m_block_inv_mutex);
            for (const uint256& hash : reverse_iterate(vHashes)) {
                peer.m_blocks_for_headers_relay.push_back(hash);
            }
        }
    }

    m_connman.WakeMessageHandler();
}

/**
 * Handle invalid block rejection and consequent peer discouragement, maintain which
 * peers announce compact blocks.
 */
void PeerManagerImpl::BlockChecked(const CBlock& block, const BlockValidationState& state)
{
    LOCK(cs_main);

    const uint256 hash(block.GetHash());
    std::map<uint256, std::pair<NodeId, bool>>::iterator it = mapBlockSource.find(hash);

    // If the block failed validation, we know where it came from and we're still connected
    // to that peer, maybe punish.
    if (state.IsInvalid() &&
        it != mapBlockSource.end() &&
        State(it->second.first)) {
            MaybePunishNodeForBlock(/*nodeid=*/ it->second.first, state, /*via_compact_block=*/ !it->second.second);
    }
    // Check that:
    // 1. The block is valid
    // 2. We're not in initial block download
    // 3. This is currently the best block we're aware of. We haven't updated
    //    the tip yet so we have no way to check this directly here. Instead we
    //    just check that there are currently no other blocks in flight.
    else if (state.IsValid() &&
             !m_chainman.ActiveChainstate().IsInitialBlockDownload() &&
             mapBlocksInFlight.count(hash) == mapBlocksInFlight.size()) {
        if (it != mapBlockSource.end()) {
            MaybeSetPeerAsAnnouncingHeaderAndIDs(it->second.first);
        }
    }
    if (it != mapBlockSource.end())
        mapBlockSource.erase(it);
}

//////////////////////////////////////////////////////////////////////////////
//
// Messages
//


bool PeerManagerImpl::AlreadyHaveTx(const GenTxid& gtxid)
{
    if (m_chainman.ActiveChain().Tip()->GetBlockHash() != hashRecentRejectsChainTip) {
        // If the chain tip has changed previously rejected transactions
        // might be now valid, e.g. due to a nLockTime'd tx becoming valid,
        // or a double-spend. Reset the rejects filter and give those
        // txs a second chance.
        hashRecentRejectsChainTip = m_chainman.ActiveChain().Tip()->GetBlockHash();
        m_recent_rejects.reset();
    }

    const uint256& hash = gtxid.GetHash();

    if (m_orphanage.HaveTx(gtxid)) return true;

    {
        LOCK(m_recent_confirmed_transactions_mutex);
        if (m_recent_confirmed_transactions.contains(hash)) return true;
    }

    return m_recent_rejects.contains(hash) || m_mempool.exists(gtxid);
}

bool PeerManagerImpl::AlreadyHaveBlock(const uint256& block_hash)
{
    return m_chainman.m_blockman.LookupBlockIndex(block_hash) != nullptr;
}

void PeerManagerImpl::SendPings()
{
    LOCK(m_peer_mutex);
    for(auto& it : m_peer_map) it.second->m_ping_queued = true;
}

void PeerManagerImpl::RelayTransaction(const uint256& txid, const uint256& wtxid)
{
    LOCK(m_peer_mutex);
    for(auto& it : m_peer_map) {
        Peer& peer = *it.second;
        auto tx_relay = peer.GetTxRelay();
        if (!tx_relay) continue;

        const uint256& hash{peer.m_wtxid_relay ? wtxid : txid};
        LOCK(tx_relay->m_tx_inventory_mutex);
        if (!tx_relay->m_tx_inventory_known_filter.contains(hash)) {
            tx_relay->m_tx_inventory_to_send.insert(hash);
        }
    };
}

void PeerManagerImpl::RelayAddress(NodeId originator,
                                   const CAddress& addr,
                                   bool fReachable)
{
    // We choose the same nodes within a given 24h window (if the list of connected
    // nodes does not change) and we don't relay to nodes that already know an
    // address. So within 24h we will likely relay a given address once. This is to
    // prevent a peer from unjustly giving their address better propagation by sending
    // it to us repeatedly.

    if (!fReachable && !addr.IsRelayable()) return;

    // Relay to a limited number of other nodes
    // Use deterministic randomness to send to the same nodes for 24 hours
    // at a time so the m_addr_knowns of the chosen nodes prevent repeats
    const uint64_t hash_addr{CServiceHash(0, 0)(addr)};
    const auto current_time{GetTime<std::chrono::seconds>()};
    // Adding address hash makes exact rotation time different per address, while preserving periodicity.
    const uint64_t time_addr{(static_cast<uint64_t>(count_seconds(current_time)) + hash_addr) / count_seconds(ROTATE_ADDR_RELAY_DEST_INTERVAL)};
    const CSipHasher hasher{m_connman.GetDeterministicRandomizer(RANDOMIZER_ID_ADDRESS_RELAY)
                                .Write(hash_addr)
                                .Write(time_addr)};
    FastRandomContext insecure_rand;

    // Relay reachable addresses to 2 peers. Unreachable addresses are relayed randomly to 1 or 2 peers.
    unsigned int nRelayNodes = (fReachable || (hasher.Finalize() & 1)) ? 2 : 1;

    std::array<std::pair<uint64_t, Peer*>, 2> best{{{0, nullptr}, {0, nullptr}}};
    assert(nRelayNodes <= best.size());

    LOCK(m_peer_mutex);

    for (auto& [id, peer] : m_peer_map) {
        if (peer->m_addr_relay_enabled && id != originator && IsAddrCompatible(*peer, addr)) {
            uint64_t hashKey = CSipHasher(hasher).Write(id).Finalize();
            for (unsigned int i = 0; i < nRelayNodes; i++) {
                 if (hashKey > best[i].first) {
                     std::copy(best.begin() + i, best.begin() + nRelayNodes - 1, best.begin() + i + 1);
                     best[i] = std::make_pair(hashKey, peer.get());
                     break;
                 }
            }
        }
    };

    for (unsigned int i = 0; i < nRelayNodes && best[i].first != 0; i++) {
        PushAddress(*best[i].second, addr, insecure_rand);
    }
}

void PeerManagerImpl::ProcessGetBlockData(CNode& pfrom, Peer& peer, const CInv& inv)
{
    std::shared_ptr<const CBlock> a_recent_block;
    std::shared_ptr<const CBlockHeaderAndShortTxIDs> a_recent_compact_block;
    {
        LOCK(m_most_recent_block_mutex);
        a_recent_block = m_most_recent_block;
        a_recent_compact_block = m_most_recent_compact_block;
    }

    bool need_activate_chain = false;
    {
        LOCK(cs_main);
        const CBlockIndex* pindex = m_chainman.m_blockman.LookupBlockIndex(inv.hash);
        if (pindex) {
            if (pindex->HaveTxsDownloaded() && !pindex->IsValid(BLOCK_VALID_SCRIPTS) &&
                    pindex->IsValid(BLOCK_VALID_TREE)) {
                // If we have the block and all of its parents, but have not yet validated it,
                // we might be in the middle of connecting it (ie in the unlock of cs_main
                // before ActivateBestChain but after AcceptBlock).
                // In this case, we need to run ActivateBestChain prior to checking the relay
                // conditions below.
                need_activate_chain = true;
            }
        }
    } // release cs_main before calling ActivateBestChain
    if (need_activate_chain) {
        BlockValidationState state;
        if (!m_chainman.ActiveChainstate().ActivateBestChain(state, a_recent_block)) {
            LogPrint(BCLog::NET, "failed to activate chain (%s)\n", state.ToString());
        }
    }

    LOCK(cs_main);
    const CBlockIndex* pindex = m_chainman.m_blockman.LookupBlockIndex(inv.hash);
    if (!pindex) {
        return;
    }
    if (!BlockRequestAllowed(pindex)) {
        LogPrint(BCLog::NET, "%s: ignoring request from peer=%i for old block that isn't in the main chain\n", __func__, pfrom.GetId());
        return;
    }
    const CNetMsgMaker msgMaker(pfrom.GetCommonVersion());
    // disconnect node in case we have reached the outbound limit for serving historical blocks
    if (m_connman.OutboundTargetReached(true) &&
        (((m_chainman.m_best_header != nullptr) && (m_chainman.m_best_header->GetBlockTime() - pindex->GetBlockTime() > HISTORICAL_BLOCK_AGE)) || inv.IsMsgFilteredBlk()) &&
        !pfrom.HasPermission(NetPermissionFlags::Download) // nodes with the download permission may exceed target
    ) {
        LogPrint(BCLog::NET, "historical block serving limit reached, disconnect peer=%d\n", pfrom.GetId());
        pfrom.fDisconnect = true;
        return;
    }
    // Avoid leaking prune-height by never sending blocks below the NODE_NETWORK_LIMITED threshold
    if (!pfrom.HasPermission(NetPermissionFlags::NoBan) && (
            (((peer.m_our_services & NODE_NETWORK_LIMITED) == NODE_NETWORK_LIMITED) && ((peer.m_our_services & NODE_NETWORK) != NODE_NETWORK) && (m_chainman.ActiveChain().Tip()->nHeight - pindex->nHeight > (int)NODE_NETWORK_LIMITED_MIN_BLOCKS + 2 /* add two blocks buffer extension for possible races */) )
       )) {
        LogPrint(BCLog::NET, "Ignore block request below NODE_NETWORK_LIMITED threshold, disconnect peer=%d\n", pfrom.GetId());
        //disconnect node and prevent it from stalling (would otherwise wait for the missing block)
        pfrom.fDisconnect = true;
        return;
    }
    // Pruned nodes may have deleted the block, so check whether
    // it's available before trying to send.
    if (!(pindex->nStatus & BLOCK_HAVE_DATA)) {
        return;
    }
    std::shared_ptr<const CBlock> pblock;
    if (a_recent_block && a_recent_block->GetHash() == pindex->GetBlockHash()) {
        pblock = a_recent_block;
    } else if (inv.IsMsgWitnessBlk()) {
        // Fast-path: in this case it is possible to serve the block directly from disk,
        // as the network format matches the format on disk
        std::vector<uint8_t> block_data;
        if (!ReadRawBlockFromDisk(block_data, pindex->GetBlockPos(), m_chainparams.MessageStart())) {
            assert(!"cannot load block from disk");
        }
        m_connman.PushMessage(&pfrom, msgMaker.Make(NetMsgType::BLOCK, Span{block_data}));
        // Don't set pblock as we've sent the block
    } else {
        // Send block from disk
        std::shared_ptr<CBlock> pblockRead = std::make_shared<CBlock>();
        if (!ReadBlockFromDisk(*pblockRead, pindex, m_chainparams.GetConsensus())) {
            assert(!"cannot load block from disk");
        }
        pblock = pblockRead;
    }
    if (pblock) {
        if (inv.IsMsgBlk()) {
            m_connman.PushMessage(&pfrom, msgMaker.Make(SERIALIZE_TRANSACTION_NO_WITNESS, NetMsgType::BLOCK, *pblock));
        } else if (inv.IsMsgWitnessBlk()) {
            m_connman.PushMessage(&pfrom, msgMaker.Make(NetMsgType::BLOCK, *pblock));
        } else if (inv.IsMsgFilteredBlk()) {
            bool sendMerkleBlock = false;
            CMerkleBlock merkleBlock;
            if (auto tx_relay = peer.GetTxRelay(); tx_relay != nullptr) {
                LOCK(tx_relay->m_bloom_filter_mutex);
                if (tx_relay->m_bloom_filter) {
                    sendMerkleBlock = true;
                    merkleBlock = CMerkleBlock(*pblock, *tx_relay->m_bloom_filter);
                }
            }
            if (sendMerkleBlock) {
                m_connman.PushMessage(&pfrom, msgMaker.Make(NetMsgType::MERKLEBLOCK, merkleBlock));
                // CMerkleBlock just contains hashes, so also push any transactions in the block the client did not see
                // This avoids hurting performance by pointlessly requiring a round-trip
                // Note that there is currently no way for a node to request any single transactions we didn't send here -
                // they must either disconnect and retry or request the full block.
                // Thus, the protocol spec specified allows for us to provide duplicate txn here,
                // however we MUST always provide at least what the remote peer needs
                typedef std::pair<unsigned int, uint256> PairType;
                for (PairType& pair : merkleBlock.vMatchedTxn)
                    m_connman.PushMessage(&pfrom, msgMaker.Make(SERIALIZE_TRANSACTION_NO_WITNESS, NetMsgType::TX, *pblock->vtx[pair.first]));
            }
            // else
            // no response
        } else if (inv.IsMsgCmpctBlk()) {
            // If a peer is asking for old blocks, we're almost guaranteed
            // they won't have a useful mempool to match against a compact block,
            // and we don't feel like constructing the object for them, so
            // instead we respond with the full, non-compact block.
            if (CanDirectFetch() && pindex->nHeight >= m_chainman.ActiveChain().Height() - MAX_CMPCTBLOCK_DEPTH) {
                if (a_recent_compact_block && a_recent_compact_block->header.GetHash() == pindex->GetBlockHash()) {
                    m_connman.PushMessage(&pfrom, msgMaker.Make(NetMsgType::CMPCTBLOCK, *a_recent_compact_block));
                } else {
                    CBlockHeaderAndShortTxIDs cmpctblock{*pblock};
                    m_connman.PushMessage(&pfrom, msgMaker.Make(NetMsgType::CMPCTBLOCK, cmpctblock));
                }
            } else {
                m_connman.PushMessage(&pfrom, msgMaker.Make(NetMsgType::BLOCK, *pblock));
            }
        }
    }

    {
        LOCK(peer.m_block_inv_mutex);
        // Trigger the peer node to send a getblocks request for the next batch of inventory
        if (inv.hash == peer.m_continuation_block) {
            // Send immediately. This must send even if redundant,
            // and we want it right after the last block so they don't
            // wait for other stuff first.
            std::vector<CInv> vInv;
            vInv.push_back(CInv(MSG_BLOCK, m_chainman.ActiveChain().Tip()->GetBlockHash()));
            m_connman.PushMessage(&pfrom, msgMaker.Make(NetMsgType::INV, vInv));
            peer.m_continuation_block.SetNull();
        }
    }
}

CTransactionRef PeerManagerImpl::FindTxForGetData(const CNode& peer, const GenTxid& gtxid, const std::chrono::seconds mempool_req, const std::chrono::seconds now)
{
    auto txinfo = m_mempool.info(gtxid);
    if (txinfo.tx) {
        // If a TX could have been INVed in reply to a MEMPOOL request,
        // or is older than UNCONDITIONAL_RELAY_DELAY, permit the request
        // unconditionally.
        if ((mempool_req.count() && txinfo.m_time <= mempool_req) || txinfo.m_time <= now - UNCONDITIONAL_RELAY_DELAY) {
            return std::move(txinfo.tx);
        }
    }

    {
        LOCK(cs_main);
        // Otherwise, the transaction must have been announced recently.
        if (State(peer.GetId())->m_recently_announced_invs.contains(gtxid.GetHash())) {
            // If it was, it can be relayed from either the mempool...
            if (txinfo.tx) return std::move(txinfo.tx);
            // ... or the relay pool.
            auto mi = mapRelay.find(gtxid.GetHash());
            if (mi != mapRelay.end()) return mi->second;
        }
    }

    return {};
}

void PeerManagerImpl::ProcessGetData(CNode& pfrom, Peer& peer, const std::atomic<bool>& interruptMsgProc)
{
    AssertLockNotHeld(cs_main);

    auto tx_relay = peer.GetTxRelay();

    std::deque<CInv>::iterator it = peer.m_getdata_requests.begin();
    std::vector<CInv> vNotFound;
    const CNetMsgMaker msgMaker(pfrom.GetCommonVersion());

    const auto now{GetTime<std::chrono::seconds>()};
    // Get last mempool request time
    const auto mempool_req = tx_relay != nullptr ? tx_relay->m_last_mempool_req.load() : std::chrono::seconds::min();

    // Process as many TX items from the front of the getdata queue as
    // possible, since they're common and it's efficient to batch process
    // them.
    while (it != peer.m_getdata_requests.end() && it->IsGenTxMsg()) {
        if (interruptMsgProc) return;
        // The send buffer provides backpressure. If there's no space in
        // the buffer, pause processing until the next call.
        if (pfrom.fPauseSend) break;

        const CInv &inv = *it++;

        if (tx_relay == nullptr) {
            // Ignore GETDATA requests for transactions from block-relay-only
            // peers and peers that asked us not to announce transactions.
            continue;
        }

        CTransactionRef tx = FindTxForGetData(pfrom, ToGenTxid(inv), mempool_req, now);
        if (tx) {
            // WTX and WITNESS_TX imply we serialize with witness
            int nSendFlags = (inv.IsMsgTx() ? SERIALIZE_TRANSACTION_NO_WITNESS : 0);
            m_connman.PushMessage(&pfrom, msgMaker.Make(nSendFlags, NetMsgType::TX, *tx));
            m_mempool.RemoveUnbroadcastTx(tx->GetHash());
            // As we're going to send tx, make sure its unconfirmed parents are made requestable.
            std::vector<uint256> parent_ids_to_add;
            {
                LOCK(m_mempool.cs);
                auto txiter = m_mempool.GetIter(tx->GetHash());
                if (txiter) {
                    const CTxMemPoolEntry::Parents& parents = (*txiter)->GetMemPoolParentsConst();
                    parent_ids_to_add.reserve(parents.size());
                    for (const CTxMemPoolEntry& parent : parents) {
                        if (parent.GetTime() > now - UNCONDITIONAL_RELAY_DELAY) {
                            parent_ids_to_add.push_back(parent.GetTx().GetHash());
                        }
                    }
                }
            }
            for (const uint256& parent_txid : parent_ids_to_add) {
                // Relaying a transaction with a recent but unconfirmed parent.
                if (WITH_LOCK(tx_relay->m_tx_inventory_mutex, return !tx_relay->m_tx_inventory_known_filter.contains(parent_txid))) {
                    LOCK(cs_main);
                    State(pfrom.GetId())->m_recently_announced_invs.insert(parent_txid);
                }
            }
        } else {
            vNotFound.push_back(inv);
        }
    }

    // Only process one BLOCK item per call, since they're uncommon and can be
    // expensive to process.
    if (it != peer.m_getdata_requests.end() && !pfrom.fPauseSend) {
        const CInv &inv = *it++;
        if (inv.IsGenBlkMsg()) {
            ProcessGetBlockData(pfrom, peer, inv);
        }
        // else: If the first item on the queue is an unknown type, we erase it
        // and continue processing the queue on the next call.
    }

    peer.m_getdata_requests.erase(peer.m_getdata_requests.begin(), it);

    if (!vNotFound.empty()) {
        // Let the peer know that we didn't find what it asked for, so it doesn't
        // have to wait around forever.
        // SPV clients care about this message: it's needed when they are
        // recursively walking the dependencies of relevant unconfirmed
        // transactions. SPV clients want to do that because they want to know
        // about (and store and rebroadcast and risk analyze) the dependencies
        // of transactions relevant to them, without having to download the
        // entire memory pool.
        // Also, other nodes can use these messages to automatically request a
        // transaction from some other peer that annnounced it, and stop
        // waiting for us to respond.
        // In normal operation, we often send NOTFOUND messages for parents of
        // transactions that we relay; if a peer is missing a parent, they may
        // assume we have them and request the parents from us.
        m_connman.PushMessage(&pfrom, msgMaker.Make(NetMsgType::NOTFOUND, vNotFound));
    }
}

uint32_t PeerManagerImpl::GetFetchFlags(const Peer& peer) const
{
    uint32_t nFetchFlags = 0;
    if (CanServeWitnesses(peer)) {
        nFetchFlags |= MSG_WITNESS_FLAG;
    }
    return nFetchFlags;
}

void PeerManagerImpl::SendBlockTransactions(CNode& pfrom, Peer& peer, const CBlock& block, const BlockTransactionsRequest& req)
{
    BlockTransactions resp(req);
    for (size_t i = 0; i < req.indexes.size(); i++) {
        if (req.indexes[i] >= block.vtx.size()) {
            Misbehaving(peer, 100, "getblocktxn with out-of-bounds tx indices");
            return;
        }
        resp.txn[i] = block.vtx[req.indexes[i]];
    }

    const CNetMsgMaker msgMaker(pfrom.GetCommonVersion());
    m_connman.PushMessage(&pfrom, msgMaker.Make(NetMsgType::BLOCKTXN, resp));
}

bool PeerManagerImpl::CheckHeadersPoW(const std::vector<CBlockHeader>& headers, const Consensus::Params& consensusParams, Peer& peer)
{
    // Do these headers have proof-of-work matching what's claimed?
    if (!HasValidProofOfWork(headers, consensusParams)) {
        Misbehaving(peer, 100, "header with invalid proof of work");
        return false;
    }

    // Are these headers connected to each other?
    if (!CheckHeadersAreContinuous(headers)) {
        Misbehaving(peer, 20, "non-continuous headers sequence");
        return false;
    }
    return true;
}

arith_uint256 PeerManagerImpl::GetAntiDoSWorkThreshold()
{
    arith_uint256 near_chaintip_work = 0;
    LOCK(cs_main);
    if (m_chainman.ActiveChain().Tip() != nullptr) {
        const CBlockIndex *tip = m_chainman.ActiveChain().Tip();
        // Use a 144 block buffer, so that we'll accept headers that fork from
        // near our tip.
        near_chaintip_work = tip->nChainWork - std::min<arith_uint256>(144*GetBlockProof(*tip), tip->nChainWork);
    }
    return std::max(near_chaintip_work, arith_uint256(nMinimumChainWork));
}

/**
 * Special handling for unconnecting headers that might be part of a block
 * announcement.
 *
 * We'll send a getheaders message in response to try to connect the chain.
 *
 * The peer can send up to MAX_UNCONNECTING_HEADERS in a row that
 * don't connect before given DoS points.
 *
 * Once a headers message is received that is valid and does connect,
 * nUnconnectingHeaders gets reset back to 0.
 */
void PeerManagerImpl::HandleFewUnconnectingHeaders(CNode& pfrom, Peer& peer,
        const std::vector<CBlockHeader>& headers)
{
    const CNetMsgMaker msgMaker(pfrom.GetCommonVersion());

    LOCK(cs_main);
    CNodeState *nodestate = State(pfrom.GetId());

    nodestate->nUnconnectingHeaders++;
    // Try to fill in the missing headers.
    if (MaybeSendGetHeaders(pfrom, GetLocator(m_chainman.m_best_header), peer)) {
        LogPrint(BCLog::NET, "received header %s: missing prev block %s, sending getheaders (%d) to end (peer=%d, nUnconnectingHeaders=%d)\n",
            headers[0].GetHash().ToString(),
            headers[0].hashPrevBlock.ToString(),
            m_chainman.m_best_header->nHeight,
            pfrom.GetId(), nodestate->nUnconnectingHeaders);
    }
    // Set hashLastUnknownBlock for this peer, so that if we
    // eventually get the headers - even from a different peer -
    // we can use this peer to download.
    UpdateBlockAvailability(pfrom.GetId(), headers.back().GetHash());

    // The peer may just be broken, so periodically assign DoS points if this
    // condition persists.
    if (nodestate->nUnconnectingHeaders % MAX_UNCONNECTING_HEADERS == 0) {
        Misbehaving(peer, 20, strprintf("%d non-connecting headers", nodestate->nUnconnectingHeaders));
    }
}

bool PeerManagerImpl::CheckHeadersAreContinuous(const std::vector<CBlockHeader>& headers) const
{
    uint256 hashLastBlock;
    for (const CBlockHeader& header : headers) {
        if (!hashLastBlock.IsNull() && header.hashPrevBlock != hashLastBlock) {
            return false;
        }
        hashLastBlock = header.GetHash();
    }
    return true;
}

bool PeerManagerImpl::IsContinuationOfLowWorkHeadersSync(Peer& peer, CNode& pfrom, std::vector<CBlockHeader>& headers)
{
    if (peer.m_headers_sync) {
        auto result = peer.m_headers_sync->ProcessNextHeaders(headers, headers.size() == MAX_HEADERS_RESULTS);
        if (result.request_more) {
            auto locator = peer.m_headers_sync->NextHeadersRequestLocator();
            // If we were instructed to ask for a locator, it should not be empty.
            Assume(!locator.vHave.empty());
            if (!locator.vHave.empty()) {
                // It should be impossible for the getheaders request to fail,
                // because we should have cleared the last getheaders timestamp
                // when processing the headers that triggered this call. But
                // it may be possible to bypass this via compactblock
                // processing, so check the result before logging just to be
                // safe.
                bool sent_getheaders = MaybeSendGetHeaders(pfrom, locator, peer);
                if (sent_getheaders) {
                    LogPrint(BCLog::NET, "more getheaders (from %s) to peer=%d\n",
                            locator.vHave.front().ToString(), pfrom.GetId());
                } else {
                    LogPrint(BCLog::NET, "error sending next getheaders (from %s) to continue sync with peer=%d\n",
                            locator.vHave.front().ToString(), pfrom.GetId());
                }
            }
        }

        if (peer.m_headers_sync->GetState() == HeadersSyncState::State::FINAL) {
            peer.m_headers_sync.reset(nullptr);

            // Delete this peer's entry in m_headers_presync_stats.
            // If this is m_headers_presync_bestpeer, it will be replaced later
            // by the next peer that triggers the else{} branch below.
            LOCK(m_headers_presync_mutex);
            m_headers_presync_stats.erase(pfrom.GetId());
        } else {
            // Build statistics for this peer's sync.
            HeadersPresyncStats stats;
            stats.first = peer.m_headers_sync->GetPresyncWork();
            if (peer.m_headers_sync->GetState() == HeadersSyncState::State::PRESYNC) {
                stats.second = {peer.m_headers_sync->GetPresyncHeight(),
                                peer.m_headers_sync->GetPresyncTime()};
            }

            // Update statistics in stats.
            LOCK(m_headers_presync_mutex);
            m_headers_presync_stats[pfrom.GetId()] = stats;
            auto best_it = m_headers_presync_stats.find(m_headers_presync_bestpeer);
            bool best_updated = false;
            if (best_it == m_headers_presync_stats.end()) {
                // If the cached best peer is outdated, iterate over all remaining ones (including
                // newly updated one) to find the best one.
                NodeId peer_best{-1};
                const HeadersPresyncStats* stat_best{nullptr};
                for (const auto& [peer, stat] : m_headers_presync_stats) {
                    if (!stat_best || stat > *stat_best) {
                        peer_best = peer;
                        stat_best = &stat;
                    }
                }
                m_headers_presync_bestpeer = peer_best;
                best_updated = (peer_best == pfrom.GetId());
            } else if (best_it->first == pfrom.GetId() || stats > best_it->second) {
                // pfrom was and remains the best peer, or pfrom just became best.
                m_headers_presync_bestpeer = pfrom.GetId();
                best_updated = true;
            }
            if (best_updated && stats.second.has_value()) {
                // If the best peer updated, and it is in its first phase, signal.
                m_headers_presync_should_signal = true;
            }
        }

        if (result.success) {
            // We only overwrite the headers passed in if processing was
            // successful.
            headers.swap(result.pow_validated_headers);
        }

        return result.success;
    }
    // Either we didn't have a sync in progress, or something went wrong
    // processing these headers, or we are returning headers to the caller to
    // process.
    return false;
}

bool PeerManagerImpl::TryLowWorkHeadersSync(Peer& peer, CNode& pfrom, const CBlockIndex* chain_start_header, std::vector<CBlockHeader>& headers)
{
    // Calculate the total work on this chain.
    arith_uint256 total_work = chain_start_header->nChainWork + CalculateHeadersWork(headers);

    // Our dynamic anti-DoS threshold (minimum work required on a headers chain
    // before we'll store it)
    arith_uint256 minimum_chain_work = GetAntiDoSWorkThreshold();

    // Avoid DoS via low-difficulty-headers by only processing if the headers
    // are part of a chain with sufficient work.
    if (total_work < minimum_chain_work) {
        // Only try to sync with this peer if their headers message was full;
        // otherwise they don't have more headers after this so no point in
        // trying to sync their too-little-work chain.
        if (headers.size() == MAX_HEADERS_RESULTS) {
            // Note: we could advance to the last header in this set that is
            // known to us, rather than starting at the first header (which we
            // may already have); however this is unlikely to matter much since
            // ProcessHeadersMessage() already handles the case where all
            // headers in a received message are already known and are
            // ancestors of m_best_header or chainActive.Tip(), by skipping
            // this logic in that case. So even if the first header in this set
            // of headers is known, some header in this set must be new, so
            // advancing to the first unknown header would be a small effect.
            LOCK(peer.m_headers_sync_mutex);
            peer.m_headers_sync.reset(new HeadersSyncState(peer.m_id, m_chainparams.GetConsensus(),
                chain_start_header, minimum_chain_work));

            // Now a HeadersSyncState object for tracking this synchronization is created,
            // process the headers using it as normal.
            return IsContinuationOfLowWorkHeadersSync(peer, pfrom, headers);
        } else {
            LogPrint(BCLog::NET, "Ignoring low-work chain (height=%u) from peer=%d\n", chain_start_header->nHeight + headers.size(), pfrom.GetId());
            // Since this is a low-work headers chain, no further processing is required.
            headers = {};
            return true;
        }
    }
    return false;
}

bool PeerManagerImpl::IsAncestorOfBestHeaderOrTip(const CBlockIndex* header)
{
    if (header == nullptr) {
        return false;
    } else if (m_chainman.m_best_header != nullptr && header == m_chainman.m_best_header->GetAncestor(header->nHeight)) {
        return true;
    } else if (m_chainman.ActiveChain().Contains(header)) {
        return true;
    }
    return false;
}

bool PeerManagerImpl::MaybeSendGetHeaders(CNode& pfrom, const CBlockLocator& locator, Peer& peer)
{
    const CNetMsgMaker msgMaker(pfrom.GetCommonVersion());

    const auto current_time = NodeClock::now();

    // Only allow a new getheaders message to go out if we don't have a recent
    // one already in-flight
    if (current_time - peer.m_last_getheaders_timestamp > HEADERS_RESPONSE_TIME) {
        m_connman.PushMessage(&pfrom, msgMaker.Make(NetMsgType::GETHEADERS, locator, uint256()));
        peer.m_last_getheaders_timestamp = current_time;
        return true;
    }
    return false;
}

/*
 * Given a new headers tip ending in pindexLast, potentially request blocks towards that tip.
 * We require that the given tip have at least as much work as our tip, and for
 * our current tip to be "close to synced" (see CanDirectFetch()).
 */
void PeerManagerImpl::HeadersDirectFetchBlocks(CNode& pfrom, const Peer& peer, const CBlockIndex* pindexLast)
{
    const CNetMsgMaker msgMaker(pfrom.GetCommonVersion());

    LOCK(cs_main);
    CNodeState *nodestate = State(pfrom.GetId());

    if (CanDirectFetch() && pindexLast->IsValid(BLOCK_VALID_TREE) && m_chainman.ActiveChain().Tip()->nChainWork <= pindexLast->nChainWork) {

        std::vector<const CBlockIndex*> vToFetch;
        const CBlockIndex *pindexWalk = pindexLast;
        // Calculate all the blocks we'd need to switch to pindexLast, up to a limit.
        while (pindexWalk && !m_chainman.ActiveChain().Contains(pindexWalk) && vToFetch.size() <= MAX_BLOCKS_IN_TRANSIT_PER_PEER) {
            if (!(pindexWalk->nStatus & BLOCK_HAVE_DATA) &&
                    !IsBlockRequested(pindexWalk->GetBlockHash()) &&
                    (!DeploymentActiveAt(*pindexWalk, m_chainman, Consensus::DEPLOYMENT_SEGWIT) || CanServeWitnesses(peer))) {
                // We don't have this block, and it's not yet in flight.
                vToFetch.push_back(pindexWalk);
            }
            pindexWalk = pindexWalk->pprev;
        }
        // If pindexWalk still isn't on our main chain, we're looking at a
        // very large reorg at a time we think we're close to caught up to
        // the main chain -- this shouldn't really happen.  Bail out on the
        // direct fetch and rely on parallel download instead.
        if (!m_chainman.ActiveChain().Contains(pindexWalk)) {
            LogPrint(BCLog::NET, "Large reorg, won't direct fetch to %s (%d)\n",
                    pindexLast->GetBlockHash().ToString(),
                    pindexLast->nHeight);
        } else {
            std::vector<CInv> vGetData;
            // Download as much as possible, from earliest to latest.
            for (const CBlockIndex *pindex : reverse_iterate(vToFetch)) {
                if (nodestate->nBlocksInFlight >= MAX_BLOCKS_IN_TRANSIT_PER_PEER) {
                    // Can't download any more from this peer
                    break;
                }
                uint32_t nFetchFlags = GetFetchFlags(peer);
                vGetData.push_back(CInv(MSG_BLOCK | nFetchFlags, pindex->GetBlockHash()));
                BlockRequested(pfrom.GetId(), *pindex);
                LogPrint(BCLog::NET, "Requesting block %s from  peer=%d\n",
                        pindex->GetBlockHash().ToString(), pfrom.GetId());
            }
            if (vGetData.size() > 1) {
                LogPrint(BCLog::NET, "Downloading blocks toward %s (%d) via headers direct fetch\n",
                        pindexLast->GetBlockHash().ToString(), pindexLast->nHeight);
            }
            if (vGetData.size() > 0) {
                if (!m_ignore_incoming_txs &&
                        nodestate->m_provides_cmpctblocks &&
                        vGetData.size() == 1 &&
                        mapBlocksInFlight.size() == 1 &&
                        pindexLast->pprev->IsValid(BLOCK_VALID_CHAIN)) {
                    // In any case, we want to download using a compact block, not a regular one
                    vGetData[0] = CInv(MSG_CMPCT_BLOCK, vGetData[0].hash);
                }
                m_connman.PushMessage(&pfrom, msgMaker.Make(NetMsgType::GETDATA, vGetData));
            }
        }
    }
}

/**
 * Given receipt of headers from a peer ending in pindexLast, along with
 * whether that header was new and whether the headers message was full,
 * update the state we keep for the peer.
 */
void PeerManagerImpl::UpdatePeerStateForReceivedHeaders(CNode& pfrom,
        const CBlockIndex *pindexLast, bool received_new_header, bool may_have_more_headers)
{
    LOCK(cs_main);
    CNodeState *nodestate = State(pfrom.GetId());
    if (nodestate->nUnconnectingHeaders > 0) {
        LogPrint(BCLog::NET, "peer=%d: resetting nUnconnectingHeaders (%d -> 0)\n", pfrom.GetId(), nodestate->nUnconnectingHeaders);
    }
    nodestate->nUnconnectingHeaders = 0;

    assert(pindexLast);
    UpdateBlockAvailability(pfrom.GetId(), pindexLast->GetBlockHash());

    // From here, pindexBestKnownBlock should be guaranteed to be non-null,
    // because it is set in UpdateBlockAvailability. Some nullptr checks
    // are still present, however, as belt-and-suspenders.

    if (received_new_header && pindexLast->nChainWork > m_chainman.ActiveChain().Tip()->nChainWork) {
        nodestate->m_last_block_announcement = GetTime();
    }

    // If we're in IBD, we want outbound peers that will serve us a useful
    // chain. Disconnect peers that are on chains with insufficient work.
    if (m_chainman.ActiveChainstate().IsInitialBlockDownload() && !may_have_more_headers) {
        // If the peer has no more headers to give us, then we know we have
        // their tip.
        if (nodestate->pindexBestKnownBlock && nodestate->pindexBestKnownBlock->nChainWork < nMinimumChainWork) {
            // This peer has too little work on their headers chain to help
            // us sync -- disconnect if it is an outbound disconnection
            // candidate.
            // Note: We compare their tip to nMinimumChainWork (rather than
            // m_chainman.ActiveChain().Tip()) because we won't start block download
            // until we have a headers chain that has at least
            // nMinimumChainWork, even if a peer has a chain past our tip,
            // as an anti-DoS measure.
            if (pfrom.IsOutboundOrBlockRelayConn()) {
                LogPrintf("Disconnecting outbound peer %d -- headers chain has insufficient work\n", pfrom.GetId());
                pfrom.fDisconnect = true;
            }
        }
    }

    // If this is an outbound full-relay peer, check to see if we should protect
    // it from the bad/lagging chain logic.
    // Note that outbound block-relay peers are excluded from this protection, and
    // thus always subject to eviction under the bad/lagging chain logic.
    // See ChainSyncTimeoutState.
    if (!pfrom.fDisconnect && pfrom.IsFullOutboundConn() && nodestate->pindexBestKnownBlock != nullptr) {
        if (m_outbound_peers_with_protect_from_disconnect < MAX_OUTBOUND_PEERS_TO_PROTECT_FROM_DISCONNECT && nodestate->pindexBestKnownBlock->nChainWork >= m_chainman.ActiveChain().Tip()->nChainWork && !nodestate->m_chain_sync.m_protect) {
            LogPrint(BCLog::NET, "Protecting outbound peer=%d from eviction\n", pfrom.GetId());
            nodestate->m_chain_sync.m_protect = true;
            ++m_outbound_peers_with_protect_from_disconnect;
        }
    }
}

void PeerManagerImpl::ProcessHeadersMessage(CNode& pfrom, Peer& peer,
                                            std::vector<CBlockHeader>&& headers,
                                            bool via_compact_block)
{
    size_t nCount = headers.size();

    if (nCount == 0) {
        // Nothing interesting. Stop asking this peers for more headers.
        // If we were in the middle of headers sync, receiving an empty headers
        // message suggests that the peer suddenly has nothing to give us
        // (perhaps it reorged to our chain). Clear download state for this peer.
        LOCK(peer.m_headers_sync_mutex);
        if (peer.m_headers_sync) {
            peer.m_headers_sync.reset(nullptr);
            LOCK(m_headers_presync_mutex);
            m_headers_presync_stats.erase(pfrom.GetId());
        }
        return;
    }

    // Before we do any processing, make sure these pass basic sanity checks.
    // We'll rely on headers having valid proof-of-work further down, as an
    // anti-DoS criteria (note: this check is required before passing any
    // headers into HeadersSyncState).
    if (!CheckHeadersPoW(headers, m_chainparams.GetConsensus(), peer)) {
        // Misbehaving() calls are handled within CheckHeadersPoW(), so we can
        // just return. (Note that even if a header is announced via compact
        // block, the header itself should be valid, so this type of error can
        // always be punished.)
        return;
    }

    const CBlockIndex *pindexLast = nullptr;

    // We'll set already_validated_work to true if these headers are
    // successfully processed as part of a low-work headers sync in progress
    // (either in PRESYNC or REDOWNLOAD phase).
    // If true, this will mean that any headers returned to us (ie during
    // REDOWNLOAD) can be validated without further anti-DoS checks.
    bool already_validated_work = false;

    // If we're in the middle of headers sync, let it do its magic.
    bool have_headers_sync = false;
    {
        LOCK(peer.m_headers_sync_mutex);

        already_validated_work = IsContinuationOfLowWorkHeadersSync(peer, pfrom, headers);

        // The headers we passed in may have been:
        // - untouched, perhaps if no headers-sync was in progress, or some
        //   failure occurred
        // - erased, such as if the headers were successfully processed and no
        //   additional headers processing needs to take place (such as if we
        //   are still in PRESYNC)
        // - replaced with headers that are now ready for validation, such as
        //   during the REDOWNLOAD phase of a low-work headers sync.
        // So just check whether we still have headers that we need to process,
        // or not.
        if (headers.empty()) {
            return;
        }

        have_headers_sync = !!peer.m_headers_sync;
    }

    // Do these headers connect to something in our block index?
    const CBlockIndex *chain_start_header{WITH_LOCK(::cs_main, return m_chainman.m_blockman.LookupBlockIndex(headers[0].hashPrevBlock))};
    bool headers_connect_blockindex{chain_start_header != nullptr};

    if (!headers_connect_blockindex) {
        if (nCount <= MAX_BLOCKS_TO_ANNOUNCE) {
            // If this looks like it could be a BIP 130 block announcement, use
            // special logic for handling headers that don't connect, as this
            // could be benign.
            HandleFewUnconnectingHeaders(pfrom, peer, headers);
        } else {
            Misbehaving(peer, 10, "invalid header received");
        }
        return;
    }

    // If the headers we received are already in memory and an ancestor of
    // m_best_header or our tip, skip anti-DoS checks. These headers will not
    // use any more memory (and we are not leaking information that could be
    // used to fingerprint us).
    const CBlockIndex *last_received_header{nullptr};
    {
        LOCK(cs_main);
        last_received_header = m_chainman.m_blockman.LookupBlockIndex(headers.back().GetHash());
        if (IsAncestorOfBestHeaderOrTip(last_received_header)) {
            already_validated_work = true;
        }
    }

    // If our peer has NetPermissionFlags::NoBan privileges, then bypass our
    // anti-DoS logic (this saves bandwidth when we connect to a trusted peer
    // on startup).
    if (pfrom.HasPermission(NetPermissionFlags::NoBan)) {
        already_validated_work = true;
    }

    // At this point, the headers connect to something in our block index.
    // Do anti-DoS checks to determine if we should process or store for later
    // processing.
    if (!already_validated_work && TryLowWorkHeadersSync(peer, pfrom,
                chain_start_header, headers)) {
        // If we successfully started a low-work headers sync, then there
        // should be no headers to process any further.
        Assume(headers.empty());
        return;
    }

    // At this point, we have a set of headers with sufficient work on them
    // which can be processed.

    // If we don't have the last header, then this peer will have given us
    // something new (if these headers are valid).
    bool received_new_header{last_received_header == nullptr};

    // Now process all the headers.
    BlockValidationState state;
    if (!m_chainman.ProcessNewBlockHeaders(headers, /*min_pow_checked=*/true, state, &pindexLast)) {
        if (state.IsInvalid()) {
            MaybePunishNodeForBlock(pfrom.GetId(), state, via_compact_block, "invalid header received");
            return;
        }
    }
    Assume(pindexLast);

    // Consider fetching more headers if we are not using our headers-sync mechanism.
    if (nCount == MAX_HEADERS_RESULTS && !have_headers_sync) {
        // Headers message had its maximum size; the peer may have more headers.
        if (MaybeSendGetHeaders(pfrom, GetLocator(pindexLast), peer)) {
            LogPrint(BCLog::NET, "more getheaders (%d) to end to peer=%d (startheight:%d)\n",
                    pindexLast->nHeight, pfrom.GetId(), peer.m_starting_height);
        }
    }

    UpdatePeerStateForReceivedHeaders(pfrom, pindexLast, received_new_header, nCount == MAX_HEADERS_RESULTS);

    // Consider immediately downloading blocks.
    HeadersDirectFetchBlocks(pfrom, peer, pindexLast);

    return;
}

/**
 * Reconsider orphan transactions after a parent has been accepted to the mempool.
 *
 * @param[in,out]  orphan_work_set  The set of orphan transactions to reconsider. Generally only one
 *                                  orphan will be reconsidered on each call of this function. This set
 *                                  may be added to if accepting an orphan causes its children to be
 *                                  reconsidered.
 */
void PeerManagerImpl::ProcessOrphanTx(std::set<uint256>& orphan_work_set)
{
    AssertLockHeld(cs_main);
    AssertLockHeld(g_cs_orphans);

    while (!orphan_work_set.empty()) {
        const uint256 orphanHash = *orphan_work_set.begin();
        orphan_work_set.erase(orphan_work_set.begin());

        const auto [porphanTx, from_peer] = m_orphanage.GetTx(orphanHash);
        if (porphanTx == nullptr) continue;

        const MempoolAcceptResult result = m_chainman.ProcessTransaction(porphanTx);
        const TxValidationState& state = result.m_state;

        if (result.m_result_type == MempoolAcceptResult::ResultType::VALID) {
            LogPrint(BCLog::MEMPOOL, "   accepted orphan tx %s\n", orphanHash.ToString());
            RelayTransaction(orphanHash, porphanTx->GetWitnessHash());
            m_orphanage.AddChildrenToWorkSet(*porphanTx, orphan_work_set);
            m_orphanage.EraseTx(orphanHash);
            for (const CTransactionRef& removedTx : result.m_replaced_transactions.value()) {
                AddToCompactExtraTransactions(removedTx);
            }
            break;
        } else if (state.GetResult() != TxValidationResult::TX_MISSING_INPUTS) {
            if (state.IsInvalid()) {
                LogPrint(BCLog::MEMPOOL, "   invalid orphan tx %s from peer=%d. %s\n",
                    orphanHash.ToString(),
                    from_peer,
                    state.ToString());
                // Maybe punish peer that gave us an invalid orphan tx
                MaybePunishNodeForTx(from_peer, state);
            }
            // Has inputs but not accepted to mempool
            // Probably non-standard or insufficient fee
            LogPrint(BCLog::MEMPOOL, "   removed orphan tx %s\n", orphanHash.ToString());
            if (state.GetResult() != TxValidationResult::TX_WITNESS_STRIPPED) {
                // We can add the wtxid of this transaction to our reject filter.
                // Do not add txids of witness transactions or witness-stripped
                // transactions to the filter, as they can have been malleated;
                // adding such txids to the reject filter would potentially
                // interfere with relay of valid transactions from peers that
                // do not support wtxid-based relay. See
                // https://github.com/bitcoin/bitcoin/issues/8279 for details.
                // We can remove this restriction (and always add wtxids to
                // the filter even for witness stripped transactions) once
                // wtxid-based relay is broadly deployed.
                // See also comments in https://github.com/bitcoin/bitcoin/pull/18044#discussion_r443419034
                // for concerns around weakening security of unupgraded nodes
                // if we start doing this too early.
                m_recent_rejects.insert(porphanTx->GetWitnessHash());
                // If the transaction failed for TX_INPUTS_NOT_STANDARD,
                // then we know that the witness was irrelevant to the policy
                // failure, since this check depends only on the txid
                // (the scriptPubKey being spent is covered by the txid).
                // Add the txid to the reject filter to prevent repeated
                // processing of this transaction in the event that child
                // transactions are later received (resulting in
                // parent-fetching by txid via the orphan-handling logic).
                if (state.GetResult() == TxValidationResult::TX_INPUTS_NOT_STANDARD && porphanTx->GetWitnessHash() != porphanTx->GetHash()) {
                    // We only add the txid if it differs from the wtxid, to
                    // avoid wasting entries in the rolling bloom filter.
                    m_recent_rejects.insert(porphanTx->GetHash());
                }
            }
            m_orphanage.EraseTx(orphanHash);
            break;
        }
    }
}

bool PeerManagerImpl::PrepareBlockFilterRequest(CNode& node, Peer& peer,
                                                BlockFilterType filter_type, uint32_t start_height,
                                                const uint256& stop_hash, uint32_t max_height_diff,
                                                const CBlockIndex*& stop_index,
                                                BlockFilterIndex*& filter_index)
{
    const bool supported_filter_type =
        (filter_type == BlockFilterType::BASIC &&
         (peer.m_our_services & NODE_COMPACT_FILTERS));
    if (!supported_filter_type) {
        LogPrint(BCLog::NET, "peer %d requested unsupported block filter type: %d\n",
                 node.GetId(), static_cast<uint8_t>(filter_type));
        node.fDisconnect = true;
        return false;
    }

    {
        LOCK(cs_main);
        stop_index = m_chainman.m_blockman.LookupBlockIndex(stop_hash);

        // Check that the stop block exists and the peer would be allowed to fetch it.
        if (!stop_index || !BlockRequestAllowed(stop_index)) {
            LogPrint(BCLog::NET, "peer %d requested invalid block hash: %s\n",
                     node.GetId(), stop_hash.ToString());
            node.fDisconnect = true;
            return false;
        }
    }

    uint32_t stop_height = stop_index->nHeight;
    if (start_height > stop_height) {
        LogPrint(BCLog::NET, "peer %d sent invalid getcfilters/getcfheaders with " /* Continued */
                 "start height %d and stop height %d\n",
                 node.GetId(), start_height, stop_height);
        node.fDisconnect = true;
        return false;
    }
    if (stop_height - start_height >= max_height_diff) {
        LogPrint(BCLog::NET, "peer %d requested too many cfilters/cfheaders: %d / %d\n",
                 node.GetId(), stop_height - start_height + 1, max_height_diff);
        node.fDisconnect = true;
        return false;
    }

    filter_index = GetBlockFilterIndex(filter_type);
    if (!filter_index) {
        LogPrint(BCLog::NET, "Filter index for supported type %s not found\n", BlockFilterTypeName(filter_type));
        return false;
    }

    return true;
}

void PeerManagerImpl::ProcessGetCFilters(CNode& node,Peer& peer, CDataStream& vRecv)
{
    uint8_t filter_type_ser;
    uint32_t start_height;
    uint256 stop_hash;

    vRecv >> filter_type_ser >> start_height >> stop_hash;

    const BlockFilterType filter_type = static_cast<BlockFilterType>(filter_type_ser);

    const CBlockIndex* stop_index;
    BlockFilterIndex* filter_index;
    if (!PrepareBlockFilterRequest(node, peer, filter_type, start_height, stop_hash,
                                   MAX_GETCFILTERS_SIZE, stop_index, filter_index)) {
        return;
    }

    std::vector<BlockFilter> filters;
    if (!filter_index->LookupFilterRange(start_height, stop_index, filters)) {
        LogPrint(BCLog::NET, "Failed to find block filter in index: filter_type=%s, start_height=%d, stop_hash=%s\n",
                     BlockFilterTypeName(filter_type), start_height, stop_hash.ToString());
        return;
    }

    for (const auto& filter : filters) {
        CSerializedNetMsg msg = CNetMsgMaker(node.GetCommonVersion())
            .Make(NetMsgType::CFILTER, filter);
        m_connman.PushMessage(&node, std::move(msg));
    }
}

void PeerManagerImpl::ProcessGetCFHeaders(CNode& node, Peer& peer, CDataStream& vRecv)
{
    uint8_t filter_type_ser;
    uint32_t start_height;
    uint256 stop_hash;

    vRecv >> filter_type_ser >> start_height >> stop_hash;

    const BlockFilterType filter_type = static_cast<BlockFilterType>(filter_type_ser);

    const CBlockIndex* stop_index;
    BlockFilterIndex* filter_index;
    if (!PrepareBlockFilterRequest(node, peer, filter_type, start_height, stop_hash,
                                   MAX_GETCFHEADERS_SIZE, stop_index, filter_index)) {
        return;
    }

    uint256 prev_header;
    if (start_height > 0) {
        const CBlockIndex* const prev_block =
            stop_index->GetAncestor(static_cast<int>(start_height - 1));
        if (!filter_index->LookupFilterHeader(prev_block, prev_header)) {
            LogPrint(BCLog::NET, "Failed to find block filter header in index: filter_type=%s, block_hash=%s\n",
                         BlockFilterTypeName(filter_type), prev_block->GetBlockHash().ToString());
            return;
        }
    }

    std::vector<uint256> filter_hashes;
    if (!filter_index->LookupFilterHashRange(start_height, stop_index, filter_hashes)) {
        LogPrint(BCLog::NET, "Failed to find block filter hashes in index: filter_type=%s, start_height=%d, stop_hash=%s\n",
                     BlockFilterTypeName(filter_type), start_height, stop_hash.ToString());
        return;
    }

    CSerializedNetMsg msg = CNetMsgMaker(node.GetCommonVersion())
        .Make(NetMsgType::CFHEADERS,
              filter_type_ser,
              stop_index->GetBlockHash(),
              prev_header,
              filter_hashes);
    m_connman.PushMessage(&node, std::move(msg));
}

void PeerManagerImpl::ProcessGetCFCheckPt(CNode& node, Peer& peer, CDataStream& vRecv)
{
    uint8_t filter_type_ser;
    uint256 stop_hash;

    vRecv >> filter_type_ser >> stop_hash;

    const BlockFilterType filter_type = static_cast<BlockFilterType>(filter_type_ser);

    const CBlockIndex* stop_index;
    BlockFilterIndex* filter_index;
    if (!PrepareBlockFilterRequest(node, peer, filter_type, /*start_height=*/0, stop_hash,
                                   /*max_height_diff=*/std::numeric_limits<uint32_t>::max(),
                                   stop_index, filter_index)) {
        return;
    }

    std::vector<uint256> headers(stop_index->nHeight / CFCHECKPT_INTERVAL);

    // Populate headers.
    const CBlockIndex* block_index = stop_index;
    for (int i = headers.size() - 1; i >= 0; i--) {
        int height = (i + 1) * CFCHECKPT_INTERVAL;
        block_index = block_index->GetAncestor(height);

        if (!filter_index->LookupFilterHeader(block_index, headers[i])) {
            LogPrint(BCLog::NET, "Failed to find block filter header in index: filter_type=%s, block_hash=%s\n",
                         BlockFilterTypeName(filter_type), block_index->GetBlockHash().ToString());
            return;
        }
    }

    CSerializedNetMsg msg = CNetMsgMaker(node.GetCommonVersion())
        .Make(NetMsgType::CFCHECKPT,
              filter_type_ser,
              stop_index->GetBlockHash(),
              headers);
    m_connman.PushMessage(&node, std::move(msg));
}

void PeerManagerImpl::ProcessBlock(CNode& node, const std::shared_ptr<const CBlock>& block, bool force_processing, bool min_pow_checked)
{
    bool new_block{false};
    m_chainman.ProcessNewBlock(block, force_processing, min_pow_checked, &new_block);
    if (new_block) {
        node.m_last_block_time = GetTime<std::chrono::seconds>();
    } else {
        LOCK(cs_main);
        mapBlockSource.erase(block->GetHash());
    }
}

void PeerManagerImpl::ProcessMessage(CNode& pfrom, const std::string& msg_type, CDataStream& vRecv,
                                     const std::chrono::microseconds time_received,
                                     const std::atomic<bool>& interruptMsgProc)
{
    AssertLockHeld(g_msgproc_mutex);

    LogPrint(BCLog::NET, "received: %s (%u bytes) peer=%d\n", SanitizeString(msg_type), vRecv.size(), pfrom.GetId());

    PeerRef peer = GetPeerRef(pfrom.GetId());
    if (peer == nullptr) return;

    if (msg_type == NetMsgType::VERSION) {
        if (pfrom.nVersion != 0) {
            LogPrint(BCLog::NET, "redundant version message from peer=%d\n", pfrom.GetId());
            return;
        }

        int64_t nTime;
        CService addrMe;
        uint64_t nNonce = 1;
        ServiceFlags nServices;
        int nVersion;
        std::string cleanSubVer;
        int starting_height = -1;
        bool fRelay = true;

        vRecv >> nVersion >> Using<CustomUintFormatter<8>>(nServices) >> nTime;
        if (nTime < 0) {
            nTime = 0;
        }
        vRecv.ignore(8); // Ignore the addrMe service bits sent by the peer
        vRecv >> addrMe;
        if (!pfrom.IsInboundConn())
        {
            m_addrman.SetServices(pfrom.addr, nServices);
        }
        if (pfrom.ExpectServicesFromConn() && !HasAllDesirableServiceFlags(nServices))
        {
            LogPrint(BCLog::NET, "peer=%d does not offer the expected services (%08x offered, %08x expected); disconnecting\n", pfrom.GetId(), nServices, GetDesirableServiceFlags(nServices));
            pfrom.fDisconnect = true;
            return;
        }

        if (nVersion < MIN_PEER_PROTO_VERSION) {
            // disconnect from peers older than this proto version
            LogPrint(BCLog::NET, "peer=%d using obsolete version %i; disconnecting\n", pfrom.GetId(), nVersion);
            pfrom.fDisconnect = true;
            return;
        }

        if (!vRecv.empty()) {
            // The version message includes information about the sending node which we don't use:
            //   - 8 bytes (service bits)
            //   - 16 bytes (ipv6 address)
            //   - 2 bytes (port)
            vRecv.ignore(26);
            vRecv >> nNonce;
        }
        if (!vRecv.empty()) {
            std::string strSubVer;
            vRecv >> LIMITED_STRING(strSubVer, MAX_SUBVERSION_LENGTH);
            cleanSubVer = SanitizeString(strSubVer);
        }
        if (!vRecv.empty()) {
            vRecv >> starting_height;
        }
        if (!vRecv.empty())
            vRecv >> fRelay;
        // Disconnect if we connected to ourself
        if (pfrom.IsInboundConn() && !m_connman.CheckIncomingNonce(nNonce))
        {
            LogPrintf("connected to self at %s, disconnecting\n", pfrom.addr.ToString());
            pfrom.fDisconnect = true;
            return;
        }

        if (pfrom.IsInboundConn() && addrMe.IsRoutable())
        {
            SeenLocal(addrMe);
        }

        // Inbound peers send us their version message when they connect.
        // We send our version message in response.
        if (pfrom.IsInboundConn()) {
            PushNodeVersion(pfrom, *peer);
        }

        // Change version
        const int greatest_common_version = std::min(nVersion, PROTOCOL_VERSION);
        pfrom.SetCommonVersion(greatest_common_version);
        pfrom.nVersion = nVersion;

        const CNetMsgMaker msg_maker(greatest_common_version);

        if (greatest_common_version >= WTXID_RELAY_VERSION) {
            m_connman.PushMessage(&pfrom, msg_maker.Make(NetMsgType::WTXIDRELAY));
        }

        // Signal ADDRv2 support (BIP155).
        if (greatest_common_version >= 70016) {
            // BIP155 defines addrv2 and sendaddrv2 for all protocol versions, but some
            // implementations reject messages they don't know. As a courtesy, don't send
            // it to nodes with a version before 70016, as no software is known to support
            // BIP155 that doesn't announce at least that protocol version number.
            m_connman.PushMessage(&pfrom, msg_maker.Make(NetMsgType::SENDADDRV2));
        }

        m_connman.PushMessage(&pfrom, msg_maker.Make(NetMsgType::VERACK));

        pfrom.m_has_all_wanted_services = HasAllDesirableServiceFlags(nServices);
        peer->m_their_services = nServices;
        pfrom.SetAddrLocal(addrMe);
        {
            LOCK(pfrom.m_subver_mutex);
            pfrom.cleanSubVer = cleanSubVer;
        }
        peer->m_starting_height = starting_height;

        // We only initialize the m_tx_relay data structure if:
        // - this isn't an outbound block-relay-only connection; and
        // - fRelay=true or we're offering NODE_BLOOM to this peer
        //   (NODE_BLOOM means that the peer may turn on tx relay later)
        if (!pfrom.IsBlockOnlyConn() &&
            (fRelay || (peer->m_our_services & NODE_BLOOM))) {
            auto* const tx_relay = peer->SetTxRelay();
            {
                LOCK(tx_relay->m_bloom_filter_mutex);
                tx_relay->m_relay_txs = fRelay; // set to true after we get the first filter* message
            }
            if (fRelay) pfrom.m_relays_txs = true;
        }

        // Potentially mark this peer as a preferred download peer.
        {
            LOCK(cs_main);
            CNodeState* state = State(pfrom.GetId());
            state->fPreferredDownload = (!pfrom.IsInboundConn() || pfrom.HasPermission(NetPermissionFlags::NoBan)) && !pfrom.IsAddrFetchConn() && CanServeBlocks(*peer);
            m_num_preferred_download_peers += state->fPreferredDownload;
        }

        if (!pfrom.IsInboundConn() && SetupAddressRelay(pfrom, *peer)) {
            // For outbound peers, we do a one-time address fetch
            // (to help populate/update our addrman).
            // If we're starting up for the first time, our addrman may be pretty
            // empty and no one will know who we are, so this mechanism is
            // important to help us connect to the network.
            //
            // We skip this for block-relay-only peers. We want to avoid
            // potentially leaking addr information and we do not want to
            // indicate to the peer that we will participate in addr relay.
            m_connman.PushMessage(&pfrom, CNetMsgMaker(greatest_common_version).Make(NetMsgType::GETADDR));
            peer->m_getaddr_sent = true;
            // When requesting a getaddr, accept an additional MAX_ADDR_TO_SEND addresses in response
            // (bypassing the MAX_ADDR_PROCESSING_TOKEN_BUCKET limit).
            peer->m_addr_token_bucket += MAX_ADDR_TO_SEND;
        }

        if (!pfrom.IsInboundConn()) {
            // For non-inbound connections, we update the addrman to record
            // connection success so that addrman will have an up-to-date
            // notion of which peers are online and available.
            //
            // While we strive to not leak information about block-relay-only
            // connections via the addrman, not moving an address to the tried
            // table is also potentially detrimental because new-table entries
            // are subject to eviction in the event of addrman collisions.  We
            // mitigate the information-leak by never calling
            // AddrMan::Connected() on block-relay-only peers; see
            // FinalizeNode().
            //
            // This moves an address from New to Tried table in Addrman,
            // resolves tried-table collisions, etc.
            m_addrman.Good(pfrom.addr);
        }

        std::string remoteAddr;
        if (fLogIPs)
            remoteAddr = ", peeraddr=" + pfrom.addr.ToString();

        LogPrint(BCLog::NET, "receive version message: %s: version %d, blocks=%d, us=%s, txrelay=%d, peer=%d%s\n",
                  cleanSubVer, pfrom.nVersion,
                  peer->m_starting_height, addrMe.ToString(), fRelay, pfrom.GetId(),
                  remoteAddr);

        int64_t nTimeOffset = nTime - GetTime();
        pfrom.nTimeOffset = nTimeOffset;
        if (!pfrom.IsInboundConn()) {
            // Don't use timedata samples from inbound peers to make it
            // harder for others to tamper with our adjusted time.
            AddTimeData(pfrom.addr, nTimeOffset);
        }

        // If the peer is old enough to have the old alert system, send it the final alert.
        if (greatest_common_version <= 70012) {
            CDataStream finalAlert(ParseHex("60010000000000000000000000ffffff7f00000000ffffff7ffeffff7f01ffffff7f00000000ffffff7f00ffffff7f002f555247454e543a20416c657274206b657920636f6d70726f6d697365642c2075706772616465207265717569726564004630440220653febd6410f470f6bae11cad19c48413becb1ac2c17f908fd0fd53bdc3abd5202206d0e9c96fe88d4a0f01ed9dedae2b6f9e00da94cad0fecaae66ecf689bf71b50"), SER_NETWORK, PROTOCOL_VERSION);
            m_connman.PushMessage(&pfrom, CNetMsgMaker(greatest_common_version).Make("alert", finalAlert));
        }

        // Feeler connections exist only to verify if address is online.
        if (pfrom.IsFeelerConn()) {
            LogPrint(BCLog::NET, "feeler connection completed peer=%d; disconnecting\n", pfrom.GetId());
            pfrom.fDisconnect = true;
        }
        return;
    }

    if (pfrom.nVersion == 0) {
        // Must have a version message before anything else
        LogPrint(BCLog::NET, "non-version message before version handshake. Message \"%s\" from peer=%d\n", SanitizeString(msg_type), pfrom.GetId());
        return;
    }

    // At this point, the outgoing message serialization version can't change.
    const CNetMsgMaker msgMaker(pfrom.GetCommonVersion());

    if (msg_type == NetMsgType::VERACK) {
        if (pfrom.fSuccessfullyConnected) {
            LogPrint(BCLog::NET, "ignoring redundant verack message from peer=%d\n", pfrom.GetId());
            return;
        }

        if (!pfrom.IsInboundConn()) {
            LogPrintf("New outbound peer connected: version: %d, blocks=%d, peer=%d%s (%s)\n",
                      pfrom.nVersion.load(), peer->m_starting_height,
                      pfrom.GetId(), (fLogIPs ? strprintf(", peeraddr=%s", pfrom.addr.ToString()) : ""),
                      pfrom.ConnectionTypeAsString());
        }

        if (pfrom.GetCommonVersion() >= SHORT_IDS_BLOCKS_VERSION) {
            // Tell our peer we are willing to provide version 2 cmpctblocks.
            // However, we do not request new block announcements using
            // cmpctblock messages.
            // We send this to non-NODE NETWORK peers as well, because
            // they may wish to request compact blocks from us
            m_connman.PushMessage(&pfrom, msgMaker.Make(NetMsgType::SENDCMPCT, /*high_bandwidth=*/false, /*version=*/CMPCTBLOCKS_VERSION));
        }
        pfrom.fSuccessfullyConnected = true;
        return;
    }

    if (msg_type == NetMsgType::SENDHEADERS) {
        LOCK(cs_main);
        State(pfrom.GetId())->fPreferHeaders = true;
        return;
    }

    if (msg_type == NetMsgType::SENDCMPCT) {
        bool sendcmpct_hb{false};
        uint64_t sendcmpct_version{0};
        vRecv >> sendcmpct_hb >> sendcmpct_version;

        // Only support compact block relay with witnesses
        if (sendcmpct_version != CMPCTBLOCKS_VERSION) return;

        LOCK(cs_main);
        CNodeState* nodestate = State(pfrom.GetId());
        nodestate->m_provides_cmpctblocks = true;
        nodestate->m_requested_hb_cmpctblocks = sendcmpct_hb;
        // save whether peer selects us as BIP152 high-bandwidth peer
        // (receiving sendcmpct(1) signals high-bandwidth, sendcmpct(0) low-bandwidth)
        pfrom.m_bip152_highbandwidth_from = sendcmpct_hb;
        return;
    }

    // BIP339 defines feature negotiation of wtxidrelay, which must happen between
    // VERSION and VERACK to avoid relay problems from switching after a connection is up.
    if (msg_type == NetMsgType::WTXIDRELAY) {
        if (pfrom.fSuccessfullyConnected) {
            // Disconnect peers that send a wtxidrelay message after VERACK.
            LogPrint(BCLog::NET, "wtxidrelay received after verack from peer=%d; disconnecting\n", pfrom.GetId());
            pfrom.fDisconnect = true;
            return;
        }
        if (pfrom.GetCommonVersion() >= WTXID_RELAY_VERSION) {
            if (!peer->m_wtxid_relay) {
                peer->m_wtxid_relay = true;
                m_wtxid_relay_peers++;
            } else {
                LogPrint(BCLog::NET, "ignoring duplicate wtxidrelay from peer=%d\n", pfrom.GetId());
            }
        } else {
            LogPrint(BCLog::NET, "ignoring wtxidrelay due to old common version=%d from peer=%d\n", pfrom.GetCommonVersion(), pfrom.GetId());
        }
        return;
    }

    // BIP155 defines feature negotiation of addrv2 and sendaddrv2, which must happen
    // between VERSION and VERACK.
    if (msg_type == NetMsgType::SENDADDRV2) {
        if (pfrom.fSuccessfullyConnected) {
            // Disconnect peers that send a SENDADDRV2 message after VERACK.
            LogPrint(BCLog::NET, "sendaddrv2 received after verack from peer=%d; disconnecting\n", pfrom.GetId());
            pfrom.fDisconnect = true;
            return;
        }
        peer->m_wants_addrv2 = true;
        return;
    }

    if (!pfrom.fSuccessfullyConnected) {
        LogPrint(BCLog::NET, "Unsupported message \"%s\" prior to verack from peer=%d\n", SanitizeString(msg_type), pfrom.GetId());
        return;
    }

    if (msg_type == NetMsgType::ADDR || msg_type == NetMsgType::ADDRV2) {
        int stream_version = vRecv.GetVersion();
        if (msg_type == NetMsgType::ADDRV2) {
            // Add ADDRV2_FORMAT to the version so that the CNetAddr and CAddress
            // unserialize methods know that an address in v2 format is coming.
            stream_version |= ADDRV2_FORMAT;
        }

        OverrideStream<CDataStream> s(&vRecv, vRecv.GetType(), stream_version);
        std::vector<CAddress> vAddr;

        s >> vAddr;

        if (!SetupAddressRelay(pfrom, *peer)) {
            LogPrint(BCLog::NET, "ignoring %s message from %s peer=%d\n", msg_type, pfrom.ConnectionTypeAsString(), pfrom.GetId());
            return;
        }

        if (vAddr.size() > MAX_ADDR_TO_SEND)
        {
            Misbehaving(*peer, 20, strprintf("%s message size = %u", msg_type, vAddr.size()));
            return;
        }

        // Store the new addresses
        std::vector<CAddress> vAddrOk;
        const auto current_a_time{Now<NodeSeconds>()};

        // Update/increment addr rate limiting bucket.
        const auto current_time{GetTime<std::chrono::microseconds>()};
        if (peer->m_addr_token_bucket < MAX_ADDR_PROCESSING_TOKEN_BUCKET) {
            // Don't increment bucket if it's already full
            const auto time_diff = std::max(current_time - peer->m_addr_token_timestamp, 0us);
            const double increment = Ticks<SecondsDouble>(time_diff) * MAX_ADDR_RATE_PER_SECOND;
            peer->m_addr_token_bucket = std::min<double>(peer->m_addr_token_bucket + increment, MAX_ADDR_PROCESSING_TOKEN_BUCKET);
        }
        peer->m_addr_token_timestamp = current_time;

        const bool rate_limited = !pfrom.HasPermission(NetPermissionFlags::Addr);
        uint64_t num_proc = 0;
        uint64_t num_rate_limit = 0;
        Shuffle(vAddr.begin(), vAddr.end(), FastRandomContext());
        for (CAddress& addr : vAddr)
        {
            if (interruptMsgProc)
                return;

            // Apply rate limiting.
            if (peer->m_addr_token_bucket < 1.0) {
                if (rate_limited) {
                    ++num_rate_limit;
                    continue;
                }
            } else {
                peer->m_addr_token_bucket -= 1.0;
            }
            // We only bother storing full nodes, though this may include
            // things which we would not make an outbound connection to, in
            // part because we may make feeler connections to them.
            if (!MayHaveUsefulAddressDB(addr.nServices) && !HasAllDesirableServiceFlags(addr.nServices))
                continue;

            if (addr.nTime <= NodeSeconds{100000000s} || addr.nTime > current_a_time + 10min) {
                addr.nTime = current_a_time - 5 * 24h;
            }
            AddAddressKnown(*peer, addr);
            if (m_banman && (m_banman->IsDiscouraged(addr) || m_banman->IsBanned(addr))) {
                // Do not process banned/discouraged addresses beyond remembering we received them
                continue;
            }
            ++num_proc;
            bool fReachable = IsReachable(addr);
            if (addr.nTime > current_a_time - 10min && !peer->m_getaddr_sent && vAddr.size() <= 10 && addr.IsRoutable()) {
                // Relay to a limited number of other nodes
                RelayAddress(pfrom.GetId(), addr, fReachable);
            }
            // Do not store addresses outside our network
            if (fReachable)
                vAddrOk.push_back(addr);
        }
        peer->m_addr_processed += num_proc;
        peer->m_addr_rate_limited += num_rate_limit;
        LogPrint(BCLog::NET, "Received addr: %u addresses (%u processed, %u rate-limited) from peer=%d\n",
                 vAddr.size(), num_proc, num_rate_limit, pfrom.GetId());

        m_addrman.Add(vAddrOk, pfrom.addr, 2h);
        if (vAddr.size() < 1000) peer->m_getaddr_sent = false;

        // AddrFetch: Require multiple addresses to avoid disconnecting on self-announcements
        if (pfrom.IsAddrFetchConn() && vAddr.size() > 1) {
            LogPrint(BCLog::NET, "addrfetch connection completed peer=%d; disconnecting\n", pfrom.GetId());
            pfrom.fDisconnect = true;
        }
        return;
    }

    if (msg_type == NetMsgType::INV) {
        std::vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ)
        {
            Misbehaving(*peer, 20, strprintf("inv message size = %u", vInv.size()));
            return;
        }

        const bool reject_tx_invs{RejectIncomingTxs(pfrom)};

        LOCK(cs_main);

        const auto current_time{GetTime<std::chrono::microseconds>()};
        uint256* best_block{nullptr};

        for (CInv& inv : vInv) {
            if (interruptMsgProc) return;

            // Ignore INVs that don't match wtxidrelay setting.
            // Note that orphan parent fetching always uses MSG_TX GETDATAs regardless of the wtxidrelay setting.
            // This is fine as no INV messages are involved in that process.
            if (peer->m_wtxid_relay) {
                if (inv.IsMsgTx()) continue;
            } else {
                if (inv.IsMsgWtx()) continue;
            }

            if (inv.IsMsgBlk()) {
                const bool fAlreadyHave = AlreadyHaveBlock(inv.hash);
                LogPrint(BCLog::NET, "got inv: %s  %s peer=%d\n", inv.ToString(), fAlreadyHave ? "have" : "new", pfrom.GetId());

                UpdateBlockAvailability(pfrom.GetId(), inv.hash);
                if (!fAlreadyHave && !fImporting && !fReindex && !IsBlockRequested(inv.hash)) {
                    // Headers-first is the primary method of announcement on
                    // the network. If a node fell back to sending blocks by
                    // inv, it may be for a re-org, or because we haven't
                    // completed initial headers sync. The final block hash
                    // provided should be the highest, so send a getheaders and
                    // then fetch the blocks we need to catch up.
                    best_block = &inv.hash;
                }
            } else if (inv.IsGenTxMsg()) {
                if (reject_tx_invs) {
                    LogPrint(BCLog::NET, "transaction (%s) inv sent in violation of protocol, disconnecting peer=%d\n", inv.hash.ToString(), pfrom.GetId());
                    pfrom.fDisconnect = true;
                    return;
                }
                const GenTxid gtxid = ToGenTxid(inv);
                const bool fAlreadyHave = AlreadyHaveTx(gtxid);
                LogPrint(BCLog::NET, "got inv: %s  %s peer=%d\n", inv.ToString(), fAlreadyHave ? "have" : "new", pfrom.GetId());

                AddKnownTx(*peer, inv.hash);
                if (!fAlreadyHave && !m_chainman.ActiveChainstate().IsInitialBlockDownload()) {
                    AddTxAnnouncement(pfrom, gtxid, current_time);
                }
            } else {
                LogPrint(BCLog::NET, "Unknown inv type \"%s\" received from peer=%d\n", inv.ToString(), pfrom.GetId());
            }
        }

        if (best_block != nullptr) {
            // If we haven't started initial headers-sync with this peer, then
            // consider sending a getheaders now. On initial startup, there's a
            // reliability vs bandwidth tradeoff, where we are only trying to do
            // initial headers sync with one peer at a time, with a long
            // timeout (at which point, if the sync hasn't completed, we will
            // disconnect the peer and then choose another). In the meantime,
            // as new blocks are found, we are willing to add one new peer per
            // block to sync with as well, to sync quicker in the case where
            // our initial peer is unresponsive (but less bandwidth than we'd
            // use if we turned on sync with all peers).
            CNodeState& state{*Assert(State(pfrom.GetId()))};
            if (state.fSyncStarted || (!peer->m_inv_triggered_getheaders_before_sync && *best_block != m_last_block_inv_triggering_headers_sync)) {
                if (MaybeSendGetHeaders(pfrom, GetLocator(m_chainman.m_best_header), *peer)) {
                    LogPrint(BCLog::NET, "getheaders (%d) %s to peer=%d\n",
                            m_chainman.m_best_header->nHeight, best_block->ToString(),
                            pfrom.GetId());
                }
                if (!state.fSyncStarted) {
                    peer->m_inv_triggered_getheaders_before_sync = true;
                    // Update the last block hash that triggered a new headers
                    // sync, so that we don't turn on headers sync with more
                    // than 1 new peer every new block.
                    m_last_block_inv_triggering_headers_sync = *best_block;
                }
            }
        }

        return;
    }

    if (msg_type == NetMsgType::GETDATA) {
        std::vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ)
        {
            Misbehaving(*peer, 20, strprintf("getdata message size = %u", vInv.size()));
            return;
        }

        LogPrint(BCLog::NET, "received getdata (%u invsz) peer=%d\n", vInv.size(), pfrom.GetId());

        if (vInv.size() > 0) {
            LogPrint(BCLog::NET, "received getdata for: %s peer=%d\n", vInv[0].ToString(), pfrom.GetId());
        }

        {
            LOCK(peer->m_getdata_requests_mutex);
            peer->m_getdata_requests.insert(peer->m_getdata_requests.end(), vInv.begin(), vInv.end());
            ProcessGetData(pfrom, *peer, interruptMsgProc);
        }

        return;
    }

    if (msg_type == NetMsgType::GETBLOCKS) {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        if (locator.vHave.size() > MAX_LOCATOR_SZ) {
            LogPrint(BCLog::NET, "getblocks locator size %lld > %d, disconnect peer=%d\n", locator.vHave.size(), MAX_LOCATOR_SZ, pfrom.GetId());
            pfrom.fDisconnect = true;
            return;
        }

        // We might have announced the currently-being-connected tip using a
        // compact block, which resulted in the peer sending a getblocks
        // request, which we would otherwise respond to without the new block.
        // To avoid this situation we simply verify that we are on our best
        // known chain now. This is super overkill, but we handle it better
        // for getheaders requests, and there are no known nodes which support
        // compact blocks but still use getblocks to request blocks.
        {
            std::shared_ptr<const CBlock> a_recent_block;
            {
                LOCK(m_most_recent_block_mutex);
                a_recent_block = m_most_recent_block;
            }
            BlockValidationState state;
            if (!m_chainman.ActiveChainstate().ActivateBestChain(state, a_recent_block)) {
                LogPrint(BCLog::NET, "failed to activate chain (%s)\n", state.ToString());
            }
        }

        LOCK(cs_main);

        // Find the last block the caller has in the main chain
        const CBlockIndex* pindex = m_chainman.ActiveChainstate().FindForkInGlobalIndex(locator);

        // Send the rest of the chain
        if (pindex)
            pindex = m_chainman.ActiveChain().Next(pindex);
        int nLimit = 500;
        LogPrint(BCLog::NET, "getblocks %d to %s limit %d from peer=%d\n", (pindex ? pindex->nHeight : -1), hashStop.IsNull() ? "end" : hashStop.ToString(), nLimit, pfrom.GetId());
        for (; pindex; pindex = m_chainman.ActiveChain().Next(pindex))
        {
            if (pindex->GetBlockHash() == hashStop)
            {
                LogPrint(BCLog::NET, "  getblocks stopping at %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
                break;
            }
            // If pruning, don't inv blocks unless we have on disk and are likely to still have
            // for some reasonable time window (1 hour) that block relay might require.
            const int nPrunedBlocksLikelyToHave = MIN_BLOCKS_TO_KEEP - 3600 / m_chainparams.GetConsensus().nPowTargetSpacing;
            if (fPruneMode && (!(pindex->nStatus & BLOCK_HAVE_DATA) || pindex->nHeight <= m_chainman.ActiveChain().Tip()->nHeight - nPrunedBlocksLikelyToHave))
            {
                LogPrint(BCLog::NET, " getblocks stopping, pruned or too old block at %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
                break;
            }
            WITH_LOCK(peer->m_block_inv_mutex, peer->m_blocks_for_inv_relay.push_back(pindex->GetBlockHash()));
            if (--nLimit <= 0) {
                // When this block is requested, we'll send an inv that'll
                // trigger the peer to getblocks the next batch of inventory.
                LogPrint(BCLog::NET, "  getblocks stopping at limit %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
                WITH_LOCK(peer->m_block_inv_mutex, {peer->m_continuation_block = pindex->GetBlockHash();});
                break;
            }
        }
        return;
    }

    if (msg_type == NetMsgType::GETBLOCKTXN) {
        BlockTransactionsRequest req;
        vRecv >> req;

        std::shared_ptr<const CBlock> recent_block;
        {
            LOCK(m_most_recent_block_mutex);
            if (m_most_recent_block_hash == req.blockhash)
                recent_block = m_most_recent_block;
            // Unlock m_most_recent_block_mutex to avoid cs_main lock inversion
        }
        if (recent_block) {
            SendBlockTransactions(pfrom, *peer, *recent_block, req);
            return;
        }

        {
            LOCK(cs_main);

            const CBlockIndex* pindex = m_chainman.m_blockman.LookupBlockIndex(req.blockhash);
            if (!pindex || !(pindex->nStatus & BLOCK_HAVE_DATA)) {
                LogPrint(BCLog::NET, "Peer %d sent us a getblocktxn for a block we don't have\n", pfrom.GetId());
                return;
            }

            if (pindex->nHeight >= m_chainman.ActiveChain().Height() - MAX_BLOCKTXN_DEPTH) {
                CBlock block;
                bool ret = ReadBlockFromDisk(block, pindex, m_chainparams.GetConsensus());
                assert(ret);

                SendBlockTransactions(pfrom, *peer, block, req);
                return;
            }
        }

        // If an older block is requested (should never happen in practice,
        // but can happen in tests) send a block response instead of a
        // blocktxn response. Sending a full block response instead of a
        // small blocktxn response is preferable in the case where a peer
        // might maliciously send lots of getblocktxn requests to trigger
        // expensive disk reads, because it will require the peer to
        // actually receive all the data read from disk over the network.
        LogPrint(BCLog::NET, "Peer %d sent us a getblocktxn for a block > %i deep\n", pfrom.GetId(), MAX_BLOCKTXN_DEPTH);
        CInv inv{MSG_WITNESS_BLOCK, req.blockhash};
        WITH_LOCK(peer->m_getdata_requests_mutex, peer->m_getdata_requests.push_back(inv));
        // The message processing loop will go around again (without pausing) and we'll respond then
        return;
    }

    if (msg_type == NetMsgType::GETHEADERS) {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        if (locator.vHave.size() > MAX_LOCATOR_SZ) {
            LogPrint(BCLog::NET, "getheaders locator size %lld > %d, disconnect peer=%d\n", locator.vHave.size(), MAX_LOCATOR_SZ, pfrom.GetId());
            pfrom.fDisconnect = true;
            return;
        }

        if (fImporting || fReindex) {
            LogPrint(BCLog::NET, "Ignoring getheaders from peer=%d while importing/reindexing\n", pfrom.GetId());
            return;
        }

        LOCK(cs_main);

        // Note that if we were to be on a chain that forks from the checkpointed
        // chain, then serving those headers to a peer that has seen the
        // checkpointed chain would cause that peer to disconnect us. Requiring
        // that our chainwork exceed nMinimumChainWork is a protection against
        // being fed a bogus chain when we started up for the first time and
        // getting partitioned off the honest network for serving that chain to
        // others.
        if (m_chainman.ActiveTip() == nullptr ||
                (m_chainman.ActiveTip()->nChainWork < nMinimumChainWork && !pfrom.HasPermission(NetPermissionFlags::Download))) {
            LogPrint(BCLog::NET, "Ignoring getheaders from peer=%d because active chain has too little work; sending empty response\n", pfrom.GetId());
            // Just respond with an empty headers message, to tell the peer to
            // go away but not treat us as unresponsive.
            m_connman.PushMessage(&pfrom, msgMaker.Make(NetMsgType::HEADERS, std::vector<CBlock>()));
            return;
        }

        CNodeState *nodestate = State(pfrom.GetId());
        const CBlockIndex* pindex = nullptr;
        if (locator.IsNull())
        {
            // If locator is null, return the hashStop block
            pindex = m_chainman.m_blockman.LookupBlockIndex(hashStop);
            if (!pindex) {
                return;
            }

            if (!BlockRequestAllowed(pindex)) {
                LogPrint(BCLog::NET, "%s: ignoring request from peer=%i for old block header that isn't in the main chain\n", __func__, pfrom.GetId());
                return;
            }
        }
        else
        {
            // Find the last block the caller has in the main chain
            pindex = m_chainman.ActiveChainstate().FindForkInGlobalIndex(locator);
            if (pindex)
                pindex = m_chainman.ActiveChain().Next(pindex);
        }

        // we must use CBlocks, as CBlockHeaders won't include the 0x00 nTx count at the end
        std::vector<CBlock> vHeaders;
        int nLimit = MAX_HEADERS_RESULTS;
        LogPrint(BCLog::NET, "getheaders %d to %s from peer=%d\n", (pindex ? pindex->nHeight : -1), hashStop.IsNull() ? "end" : hashStop.ToString(), pfrom.GetId());
        for (; pindex; pindex = m_chainman.ActiveChain().Next(pindex))
        {
            vHeaders.push_back(pindex->GetBlockHeader());
            if (--nLimit <= 0 || pindex->GetBlockHash() == hashStop)
                break;
        }
        // pindex can be nullptr either if we sent m_chainman.ActiveChain().Tip() OR
        // if our peer has m_chainman.ActiveChain().Tip() (and thus we are sending an empty
        // headers message). In both cases it's safe to update
        // pindexBestHeaderSent to be our tip.
        //
        // It is important that we simply reset the BestHeaderSent value here,
        // and not max(BestHeaderSent, newHeaderSent). We might have announced
        // the currently-being-connected tip using a compact block, which
        // resulted in the peer sending a headers request, which we respond to
        // without the new block. By resetting the BestHeaderSent, we ensure we
        // will re-announce the new block via headers (or compact blocks again)
        // in the SendMessages logic.
        nodestate->pindexBestHeaderSent = pindex ? pindex : m_chainman.ActiveChain().Tip();
        m_connman.PushMessage(&pfrom, msgMaker.Make(NetMsgType::HEADERS, vHeaders));
        return;
    }

    if (msg_type == NetMsgType::TX) {
        if (RejectIncomingTxs(pfrom)) {
            LogPrint(BCLog::NET, "transaction sent in violation of protocol peer=%d\n", pfrom.GetId());
            pfrom.fDisconnect = true;
            return;
        }

        // Stop processing the transaction early if we are still in IBD since we don't
        // have enough information to validate it yet. Sending unsolicited transactions
        // is not considered a protocol violation, so don't punish the peer.
        if (m_chainman.ActiveChainstate().IsInitialBlockDownload()) return;

        CTransactionRef ptx;
        vRecv >> ptx;
        const CTransaction& tx = *ptx;

        const uint256& txid = ptx->GetHash();
        const uint256& wtxid = ptx->GetWitnessHash();

        const uint256& hash = peer->m_wtxid_relay ? wtxid : txid;
        AddKnownTx(*peer, hash);
        if (peer->m_wtxid_relay && txid != wtxid) {
            // Insert txid into m_tx_inventory_known_filter, even for
            // wtxidrelay peers. This prevents re-adding of
            // unconfirmed parents to the recently_announced
            // filter, when a child tx is requested. See
            // ProcessGetData().
            AddKnownTx(*peer, txid);
        }

        LOCK2(cs_main, g_cs_orphans);

        m_txrequest.ReceivedResponse(pfrom.GetId(), txid);
        if (tx.HasWitness()) m_txrequest.ReceivedResponse(pfrom.GetId(), wtxid);

        // We do the AlreadyHaveTx() check using wtxid, rather than txid - in the
        // absence of witness malleation, this is strictly better, because the
        // recent rejects filter may contain the wtxid but rarely contains
        // the txid of a segwit transaction that has been rejected.
        // In the presence of witness malleation, it's possible that by only
        // doing the check with wtxid, we could overlook a transaction which
        // was confirmed with a different witness, or exists in our mempool
        // with a different witness, but this has limited downside:
        // mempool validation does its own lookup of whether we have the txid
        // already; and an adversary can already relay us old transactions
        // (older than our recency filter) if trying to DoS us, without any need
        // for witness malleation.
        if (AlreadyHaveTx(GenTxid::Wtxid(wtxid))) {
            if (pfrom.HasPermission(NetPermissionFlags::ForceRelay)) {
                // Always relay transactions received from peers with forcerelay
                // permission, even if they were already in the mempool, allowing
                // the node to function as a gateway for nodes hidden behind it.
                if (!m_mempool.exists(GenTxid::Txid(tx.GetHash()))) {
                    LogPrintf("Not relaying non-mempool transaction %s from forcerelay peer=%d\n", tx.GetHash().ToString(), pfrom.GetId());
                } else {
                    LogPrintf("Force relaying tx %s from peer=%d\n", tx.GetHash().ToString(), pfrom.GetId());
                    RelayTransaction(tx.GetHash(), tx.GetWitnessHash());
                }
            }
            return;
        }

        const MempoolAcceptResult result = m_chainman.ProcessTransaction(ptx);
        const TxValidationState& state = result.m_state;

        if (result.m_result_type == MempoolAcceptResult::ResultType::VALID) {
            // As this version of the transaction was acceptable, we can forget about any
            // requests for it.
            m_txrequest.ForgetTxHash(tx.GetHash());
            m_txrequest.ForgetTxHash(tx.GetWitnessHash());
            RelayTransaction(tx.GetHash(), tx.GetWitnessHash());
            m_orphanage.AddChildrenToWorkSet(tx, peer->m_orphan_work_set);

            pfrom.m_last_tx_time = GetTime<std::chrono::seconds>();

            LogPrint(BCLog::MEMPOOL, "AcceptToMemoryPool: peer=%d: accepted %s (poolsz %u txn, %u kB)\n",
                pfrom.GetId(),
                tx.GetHash().ToString(),
                m_mempool.size(), m_mempool.DynamicMemoryUsage() / 1000);

            for (const CTransactionRef& removedTx : result.m_replaced_transactions.value()) {
                AddToCompactExtraTransactions(removedTx);
            }

            // Recursively process any orphan transactions that depended on this one
            ProcessOrphanTx(peer->m_orphan_work_set);
        }
        else if (state.GetResult() == TxValidationResult::TX_MISSING_INPUTS)
        {
            bool fRejectedParents = false; // It may be the case that the orphans parents have all been rejected

            // Deduplicate parent txids, so that we don't have to loop over
            // the same parent txid more than once down below.
            std::vector<uint256> unique_parents;
            unique_parents.reserve(tx.vin.size());
            for (const CTxIn& txin : tx.vin) {
                // We start with all parents, and then remove duplicates below.
                unique_parents.push_back(txin.prevout.hash);
            }
            std::sort(unique_parents.begin(), unique_parents.end());
            unique_parents.erase(std::unique(unique_parents.begin(), unique_parents.end()), unique_parents.end());
            for (const uint256& parent_txid : unique_parents) {
                if (m_recent_rejects.contains(parent_txid)) {
                    fRejectedParents = true;
                    break;
                }
            }
            if (!fRejectedParents) {
                const auto current_time{GetTime<std::chrono::microseconds>()};

                for (const uint256& parent_txid : unique_parents) {
                    // Here, we only have the txid (and not wtxid) of the
                    // inputs, so we only request in txid mode, even for
                    // wtxidrelay peers.
                    // Eventually we should replace this with an improved
                    // protocol for getting all unconfirmed parents.
                    const auto gtxid{GenTxid::Txid(parent_txid)};
                    AddKnownTx(*peer, parent_txid);
                    if (!AlreadyHaveTx(gtxid)) AddTxAnnouncement(pfrom, gtxid, current_time);
                }

                if (m_orphanage.AddTx(ptx, pfrom.GetId())) {
                    AddToCompactExtraTransactions(ptx);
                }

                // Once added to the orphan pool, a tx is considered AlreadyHave, and we shouldn't request it anymore.
                m_txrequest.ForgetTxHash(tx.GetHash());
                m_txrequest.ForgetTxHash(tx.GetWitnessHash());

                // DoS prevention: do not allow m_orphanage to grow unbounded (see CVE-2012-3789)
                unsigned int nMaxOrphanTx = (unsigned int)std::max((int64_t)0, gArgs.GetIntArg("-maxorphantx", DEFAULT_MAX_ORPHAN_TRANSACTIONS));
                m_orphanage.LimitOrphans(nMaxOrphanTx);
            } else {
                LogPrint(BCLog::MEMPOOL, "not keeping orphan with rejected parents %s\n",tx.GetHash().ToString());
                // We will continue to reject this tx since it has rejected
                // parents so avoid re-requesting it from other peers.
                // Here we add both the txid and the wtxid, as we know that
                // regardless of what witness is provided, we will not accept
                // this, so we don't need to allow for redownload of this txid
                // from any of our non-wtxidrelay peers.
                m_recent_rejects.insert(tx.GetHash());
                m_recent_rejects.insert(tx.GetWitnessHash());
                m_txrequest.ForgetTxHash(tx.GetHash());
                m_txrequest.ForgetTxHash(tx.GetWitnessHash());
            }
        } else {
            if (state.GetResult() != TxValidationResult::TX_WITNESS_STRIPPED) {
                // We can add the wtxid of this transaction to our reject filter.
                // Do not add txids of witness transactions or witness-stripped
                // transactions to the filter, as they can have been malleated;
                // adding such txids to the reject filter would potentially
                // interfere with relay of valid transactions from peers that
                // do not support wtxid-based relay. See
                // https://github.com/bitcoin/bitcoin/issues/8279 for details.
                // We can remove this restriction (and always add wtxids to
                // the filter even for witness stripped transactions) once
                // wtxid-based relay is broadly deployed.
                // See also comments in https://github.com/bitcoin/bitcoin/pull/18044#discussion_r443419034
                // for concerns around weakening security of unupgraded nodes
                // if we start doing this too early.
                m_recent_rejects.insert(tx.GetWitnessHash());
                m_txrequest.ForgetTxHash(tx.GetWitnessHash());
                // If the transaction failed for TX_INPUTS_NOT_STANDARD,
                // then we know that the witness was irrelevant to the policy
                // failure, since this check depends only on the txid
                // (the scriptPubKey being spent is covered by the txid).
                // Add the txid to the reject filter to prevent repeated
                // processing of this transaction in the event that child
                // transactions are later received (resulting in
                // parent-fetching by txid via the orphan-handling logic).
                if (state.GetResult() == TxValidationResult::TX_INPUTS_NOT_STANDARD && tx.GetWitnessHash() != tx.GetHash()) {
                    m_recent_rejects.insert(tx.GetHash());
                    m_txrequest.ForgetTxHash(tx.GetHash());
                }
                if (RecursiveDynamicUsage(*ptx) < 100000) {
                    AddToCompactExtraTransactions(ptx);
                }
            }
        }

        // If a tx has been detected by m_recent_rejects, we will have reached
        // this point and the tx will have been ignored. Because we haven't
        // submitted the tx to our mempool, we won't have computed a DoS
        // score for it or determined exactly why we consider it invalid.
        //
        // This means we won't penalize any peer subsequently relaying a DoSy
        // tx (even if we penalized the first peer who gave it to us) because
        // we have to account for m_recent_rejects showing false positives. In
        // other words, we shouldn't penalize a peer if we aren't *sure* they
        // submitted a DoSy tx.
        //
        // Note that m_recent_rejects doesn't just record DoSy or invalid
        // transactions, but any tx not accepted by the mempool, which may be
        // due to node policy (vs. consensus). So we can't blanket penalize a
        // peer simply for relaying a tx that our m_recent_rejects has caught,
        // regardless of false positives.

        if (state.IsInvalid()) {
            LogPrint(BCLog::MEMPOOLREJ, "%s from peer=%d was not accepted: %s\n", tx.GetHash().ToString(),
                pfrom.GetId(),
                state.ToString());
            MaybePunishNodeForTx(pfrom.GetId(), state);
        }
        return;
    }

    if (msg_type == NetMsgType::CMPCTBLOCK)
    {
        // Ignore cmpctblock received while importing
        if (fImporting || fReindex) {
            LogPrint(BCLog::NET, "Unexpected cmpctblock message received from peer %d\n", pfrom.GetId());
            return;
        }

        CBlockHeaderAndShortTxIDs cmpctblock;
        vRecv >> cmpctblock;

        bool received_new_header = false;

        {
        LOCK(cs_main);

        const CBlockIndex* prev_block = m_chainman.m_blockman.LookupBlockIndex(cmpctblock.header.hashPrevBlock);
        if (!prev_block) {
            // Doesn't connect (or is genesis), instead of DoSing in AcceptBlockHeader, request deeper headers
            if (!m_chainman.ActiveChainstate().IsInitialBlockDownload()) {
                MaybeSendGetHeaders(pfrom, GetLocator(m_chainman.m_best_header), *peer);
            }
            return;
        } else if (prev_block->nChainWork + CalculateHeadersWork({cmpctblock.header}) < GetAntiDoSWorkThreshold()) {
            // If we get a low-work header in a compact block, we can ignore it.
            LogPrint(BCLog::NET, "Ignoring low-work compact block from peer %d\n", pfrom.GetId());
            return;
        }

        if (!m_chainman.m_blockman.LookupBlockIndex(cmpctblock.header.GetHash())) {
            received_new_header = true;
        }
        }

        const CBlockIndex *pindex = nullptr;
        BlockValidationState state;
        if (!m_chainman.ProcessNewBlockHeaders({cmpctblock.header}, /*min_pow_checked=*/true, state, &pindex)) {
            if (state.IsInvalid()) {
                MaybePunishNodeForBlock(pfrom.GetId(), state, /*via_compact_block=*/true, "invalid header via cmpctblock");
                return;
            }
        }

        // When we succeed in decoding a block's txids from a cmpctblock
        // message we typically jump to the BLOCKTXN handling code, with a
        // dummy (empty) BLOCKTXN message, to re-use the logic there in
        // completing processing of the putative block (without cs_main).
        bool fProcessBLOCKTXN = false;
        CDataStream blockTxnMsg(SER_NETWORK, PROTOCOL_VERSION);

        // If we end up treating this as a plain headers message, call that as well
        // without cs_main.
        bool fRevertToHeaderProcessing = false;

        // Keep a CBlock for "optimistic" compactblock reconstructions (see
        // below)
        std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>();
        bool fBlockReconstructed = false;

        {
        LOCK2(cs_main, g_cs_orphans);
        // If AcceptBlockHeader returned true, it set pindex
        assert(pindex);
        UpdateBlockAvailability(pfrom.GetId(), pindex->GetBlockHash());

        CNodeState *nodestate = State(pfrom.GetId());

        // If this was a new header with more work than our tip, update the
        // peer's last block announcement time
        if (received_new_header && pindex->nChainWork > m_chainman.ActiveChain().Tip()->nChainWork) {
            nodestate->m_last_block_announcement = GetTime();
        }

        std::map<uint256, std::pair<NodeId, std::list<QueuedBlock>::iterator> >::iterator blockInFlightIt = mapBlocksInFlight.find(pindex->GetBlockHash());
        bool fAlreadyInFlight = blockInFlightIt != mapBlocksInFlight.end();

        if (pindex->nStatus & BLOCK_HAVE_DATA) // Nothing to do here
            return;

        if (pindex->nChainWork <= m_chainman.ActiveChain().Tip()->nChainWork || // We know something better
                pindex->nTx != 0) { // We had this block at some point, but pruned it
            if (fAlreadyInFlight) {
                // We requested this block for some reason, but our mempool will probably be useless
                // so we just grab the block via normal getdata
                std::vector<CInv> vInv(1);
                vInv[0] = CInv(MSG_BLOCK | GetFetchFlags(*peer), cmpctblock.header.GetHash());
                m_connman.PushMessage(&pfrom, msgMaker.Make(NetMsgType::GETDATA, vInv));
            }
            return;
        }

        // If we're not close to tip yet, give up and let parallel block fetch work its magic
        if (!fAlreadyInFlight && !CanDirectFetch()) {
            return;
        }

        // We want to be a bit conservative just to be extra careful about DoS
        // possibilities in compact block processing...
        if (pindex->nHeight <= m_chainman.ActiveChain().Height() + 2) {
            if ((!fAlreadyInFlight && nodestate->nBlocksInFlight < MAX_BLOCKS_IN_TRANSIT_PER_PEER) ||
                 (fAlreadyInFlight && blockInFlightIt->second.first == pfrom.GetId())) {
                std::list<QueuedBlock>::iterator* queuedBlockIt = nullptr;
                if (!BlockRequested(pfrom.GetId(), *pindex, &queuedBlockIt)) {
                    if (!(*queuedBlockIt)->partialBlock)
                        (*queuedBlockIt)->partialBlock.reset(new PartiallyDownloadedBlock(&m_mempool));
                    else {
                        // The block was already in flight using compact blocks from the same peer
                        LogPrint(BCLog::NET, "Peer sent us compact block we were already syncing!\n");
                        return;
                    }
                }

                PartiallyDownloadedBlock& partialBlock = *(*queuedBlockIt)->partialBlock;
                ReadStatus status = partialBlock.InitData(cmpctblock, vExtraTxnForCompact);
                if (status == READ_STATUS_INVALID) {
                    RemoveBlockRequest(pindex->GetBlockHash()); // Reset in-flight state in case Misbehaving does not result in a disconnect
                    Misbehaving(*peer, 100, "invalid compact block");
                    return;
                } else if (status == READ_STATUS_FAILED) {
                    // Duplicate txindexes, the block is now in-flight, so just request it
                    std::vector<CInv> vInv(1);
                    vInv[0] = CInv(MSG_BLOCK | GetFetchFlags(*peer), cmpctblock.header.GetHash());
                    m_connman.PushMessage(&pfrom, msgMaker.Make(NetMsgType::GETDATA, vInv));
                    return;
                }

                BlockTransactionsRequest req;
                for (size_t i = 0; i < cmpctblock.BlockTxCount(); i++) {
                    if (!partialBlock.IsTxAvailable(i))
                        req.indexes.push_back(i);
                }
                if (req.indexes.empty()) {
                    // Dirty hack to jump to BLOCKTXN code (TODO: move message handling into their own functions)
                    BlockTransactions txn;
                    txn.blockhash = cmpctblock.header.GetHash();
                    blockTxnMsg << txn;
                    fProcessBLOCKTXN = true;
                } else {
                    req.blockhash = pindex->GetBlockHash();
                    m_connman.PushMessage(&pfrom, msgMaker.Make(NetMsgType::GETBLOCKTXN, req));
                }
            } else {
                // This block is either already in flight from a different
                // peer, or this peer has too many blocks outstanding to
                // download from.
                // Optimistically try to reconstruct anyway since we might be
                // able to without any round trips.
                PartiallyDownloadedBlock tempBlock(&m_mempool);
                ReadStatus status = tempBlock.InitData(cmpctblock, vExtraTxnForCompact);
                if (status != READ_STATUS_OK) {
                    // TODO: don't ignore failures
                    return;
                }
                std::vector<CTransactionRef> dummy;
                status = tempBlock.FillBlock(*pblock, dummy);
                if (status == READ_STATUS_OK) {
                    fBlockReconstructed = true;
                }
            }
        } else {
            if (fAlreadyInFlight) {
                // We requested this block, but its far into the future, so our
                // mempool will probably be useless - request the block normally
                std::vector<CInv> vInv(1);
                vInv[0] = CInv(MSG_BLOCK | GetFetchFlags(*peer), cmpctblock.header.GetHash());
                m_connman.PushMessage(&pfrom, msgMaker.Make(NetMsgType::GETDATA, vInv));
                return;
            } else {
                // If this was an announce-cmpctblock, we want the same treatment as a header message
                fRevertToHeaderProcessing = true;
            }
        }
        } // cs_main

        if (fProcessBLOCKTXN) {
            return ProcessMessage(pfrom, NetMsgType::BLOCKTXN, blockTxnMsg, time_received, interruptMsgProc);
        }

        if (fRevertToHeaderProcessing) {
            // Headers received from HB compact block peers are permitted to be
            // relayed before full validation (see BIP 152), so we don't want to disconnect
            // the peer if the header turns out to be for an invalid block.
            // Note that if a peer tries to build on an invalid chain, that
            // will be detected and the peer will be disconnected/discouraged.
            return ProcessHeadersMessage(pfrom, *peer, {cmpctblock.header}, /*via_compact_block=*/true);
        }

        if (fBlockReconstructed) {
            // If we got here, we were able to optimistically reconstruct a
            // block that is in flight from some other peer.
            {
                LOCK(cs_main);
                mapBlockSource.emplace(pblock->GetHash(), std::make_pair(pfrom.GetId(), false));
            }
            // Setting force_processing to true means that we bypass some of
            // our anti-DoS protections in AcceptBlock, which filters
            // unrequested blocks that might be trying to waste our resources
            // (eg disk space). Because we only try to reconstruct blocks when
            // we're close to caught up (via the CanDirectFetch() requirement
            // above, combined with the behavior of not requesting blocks until
            // we have a chain with at least nMinimumChainWork), and we ignore
            // compact blocks with less work than our tip, it is safe to treat
            // reconstructed compact blocks as having been requested.
            ProcessBlock(pfrom, pblock, /*force_processing=*/true, /*min_pow_checked=*/true);
            LOCK(cs_main); // hold cs_main for CBlockIndex::IsValid()
            if (pindex->IsValid(BLOCK_VALID_TRANSACTIONS)) {
                // Clear download state for this block, which is in
                // process from some other peer.  We do this after calling
                // ProcessNewBlock so that a malleated cmpctblock announcement
                // can't be used to interfere with block relay.
                RemoveBlockRequest(pblock->GetHash());
            }
        }
        return;
    }

    if (msg_type == NetMsgType::BLOCKTXN)
    {
        // Ignore blocktxn received while importing
        if (fImporting || fReindex) {
            LogPrint(BCLog::NET, "Unexpected blocktxn message received from peer %d\n", pfrom.GetId());
            return;
        }

        BlockTransactions resp;
        vRecv >> resp;

        std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>();
        bool fBlockRead = false;
        {
            LOCK(cs_main);

            std::map<uint256, std::pair<NodeId, std::list<QueuedBlock>::iterator> >::iterator it = mapBlocksInFlight.find(resp.blockhash);
            if (it == mapBlocksInFlight.end() || !it->second.second->partialBlock ||
                    it->second.first != pfrom.GetId()) {
                LogPrint(BCLog::NET, "Peer %d sent us block transactions for block we weren't expecting\n", pfrom.GetId());
                return;
            }

            PartiallyDownloadedBlock& partialBlock = *it->second.second->partialBlock;
            ReadStatus status = partialBlock.FillBlock(*pblock, resp.txn);
            if (status == READ_STATUS_INVALID) {
                RemoveBlockRequest(resp.blockhash); // Reset in-flight state in case Misbehaving does not result in a disconnect
                Misbehaving(*peer, 100, "invalid compact block/non-matching block transactions");
                return;
            } else if (status == READ_STATUS_FAILED) {
                // Might have collided, fall back to getdata now :(
                std::vector<CInv> invs;
                invs.push_back(CInv(MSG_BLOCK | GetFetchFlags(*peer), resp.blockhash));
                m_connman.PushMessage(&pfrom, msgMaker.Make(NetMsgType::GETDATA, invs));
            } else {
                // Block is either okay, or possibly we received
                // READ_STATUS_CHECKBLOCK_FAILED.
                // Note that CheckBlock can only fail for one of a few reasons:
                // 1. bad-proof-of-work (impossible here, because we've already
                //    accepted the header)
                // 2. merkleroot doesn't match the transactions given (already
                //    caught in FillBlock with READ_STATUS_FAILED, so
                //    impossible here)
                // 3. the block is otherwise invalid (eg invalid coinbase,
                //    block is too big, too many legacy sigops, etc).
                // So if CheckBlock failed, #3 is the only possibility.
                // Under BIP 152, we don't discourage the peer unless proof of work is
                // invalid (we don't require all the stateless checks to have
                // been run).  This is handled below, so just treat this as
                // though the block was successfully read, and rely on the
                // handling in ProcessNewBlock to ensure the block index is
                // updated, etc.
                RemoveBlockRequest(resp.blockhash); // it is now an empty pointer
                fBlockRead = true;
                // mapBlockSource is used for potentially punishing peers and
                // updating which peers send us compact blocks, so the race
                // between here and cs_main in ProcessNewBlock is fine.
                // BIP 152 permits peers to relay compact blocks after validating
                // the header only; we should not punish peers if the block turns
                // out to be invalid.
                mapBlockSource.emplace(resp.blockhash, std::make_pair(pfrom.GetId(), false));
            }
        } // Don't hold cs_main when we call into ProcessNewBlock
        if (fBlockRead) {
            // Since we requested this block (it was in mapBlocksInFlight), force it to be processed,
            // even if it would not be a candidate for new tip (missing previous block, chain not long enough, etc)
            // This bypasses some anti-DoS logic in AcceptBlock (eg to prevent
            // disk-space attacks), but this should be safe due to the
            // protections in the compact block handler -- see related comment
            // in compact block optimistic reconstruction handling.
            ProcessBlock(pfrom, pblock, /*force_processing=*/true, /*min_pow_checked=*/true);
        }
        return;
    }

    if (msg_type == NetMsgType::HEADERS)
    {
        // Ignore headers received while importing
        if (fImporting || fReindex) {
            LogPrint(BCLog::NET, "Unexpected headers message received from peer %d\n", pfrom.GetId());
            return;
        }

        // Assume that this is in response to any outstanding getheaders
        // request we may have sent, and clear out the time of our last request
        peer->m_last_getheaders_timestamp = {};

        std::vector<CBlockHeader> headers;

        // Bypass the normal CBlock deserialization, as we don't want to risk deserializing 2000 full blocks.
        unsigned int nCount = ReadCompactSize(vRecv);
        if (nCount > MAX_HEADERS_RESULTS) {
            Misbehaving(*peer, 20, strprintf("headers message size = %u", nCount));
            return;
        }
        headers.resize(nCount);
        for (unsigned int n = 0; n < nCount; n++) {
            vRecv >> headers[n];
            ReadCompactSize(vRecv); // ignore tx count; assume it is 0.
        }

        ProcessHeadersMessage(pfrom, *peer, std::move(headers), /*via_compact_block=*/false);

        // Check if the headers presync progress needs to be reported to validation.
        // This needs to be done without holding the m_headers_presync_mutex lock.
        if (m_headers_presync_should_signal.exchange(false)) {
            HeadersPresyncStats stats;
            {
                LOCK(m_headers_presync_mutex);
                auto it = m_headers_presync_stats.find(m_headers_presync_bestpeer);
                if (it != m_headers_presync_stats.end()) stats = it->second;
            }
            if (stats.second) {
                m_chainman.ReportHeadersPresync(stats.first, stats.second->first, stats.second->second);
            }
        }

        return;
    }

    if (msg_type == NetMsgType::BLOCK)
    {
        // Ignore block received while importing
        if (fImporting || fReindex) {
            LogPrint(BCLog::NET, "Unexpected block message received from peer %d\n", pfrom.GetId());
            return;
        }

        std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>();
        vRecv >> *pblock;

        LogPrint(BCLog::NET, "received block %s peer=%d\n", pblock->GetHash().ToString(), pfrom.GetId());

        bool forceProcessing = false;
        const uint256 hash(pblock->GetHash());
        bool min_pow_checked = false;
        {
            LOCK(cs_main);
            // Always process the block if we requested it, since we may
            // need it even when it's not a candidate for a new best tip.
            forceProcessing = IsBlockRequested(hash);
            RemoveBlockRequest(hash);
            // mapBlockSource is only used for punishing peers and setting
            // which peers send us compact blocks, so the race between here and
            // cs_main in ProcessNewBlock is fine.
            mapBlockSource.emplace(hash, std::make_pair(pfrom.GetId(), true));

            // Check work on this block against our anti-dos thresholds.
            const CBlockIndex* prev_block = m_chainman.m_blockman.LookupBlockIndex(pblock->hashPrevBlock);
            if (prev_block && prev_block->nChainWork + CalculateHeadersWork({pblock->GetBlockHeader()}) >= GetAntiDoSWorkThreshold()) {
                min_pow_checked = true;
            }
        }
        ProcessBlock(pfrom, pblock, forceProcessing, min_pow_checked);
        return;
    }

    if (msg_type == NetMsgType::GETADDR) {
        // This asymmetric behavior for inbound and outbound connections was introduced
        // to prevent a fingerprinting attack: an attacker can send specific fake addresses
        // to users' AddrMan and later request them by sending getaddr messages.
        // Making nodes which are behind NAT and can only make outgoing connections ignore
        // the getaddr message mitigates the attack.
        if (!pfrom.IsInboundConn()) {
            LogPrint(BCLog::NET, "Ignoring \"getaddr\" from %s connection. peer=%d\n", pfrom.ConnectionTypeAsString(), pfrom.GetId());
            return;
        }

        // Since this must be an inbound connection, SetupAddressRelay will
        // never fail.
        Assume(SetupAddressRelay(pfrom, *peer));

        // Only send one GetAddr response per connection to reduce resource waste
        // and discourage addr stamping of INV announcements.
        if (peer->m_getaddr_recvd) {
            LogPrint(BCLog::NET, "Ignoring repeated \"getaddr\". peer=%d\n", pfrom.GetId());
            return;
        }
        peer->m_getaddr_recvd = true;

        peer->m_addrs_to_send.clear();
        std::vector<CAddress> vAddr;
        if (pfrom.HasPermission(NetPermissionFlags::Addr)) {
            vAddr = m_connman.GetAddresses(MAX_ADDR_TO_SEND, MAX_PCT_ADDR_TO_SEND, /*network=*/std::nullopt);
        } else {
            vAddr = m_connman.GetAddresses(pfrom, MAX_ADDR_TO_SEND, MAX_PCT_ADDR_TO_SEND);
        }
        FastRandomContext insecure_rand;
        for (const CAddress &addr : vAddr) {
            PushAddress(*peer, addr, insecure_rand);
        }
        return;
    }

    if (msg_type == NetMsgType::MEMPOOL) {
        if (!(peer->m_our_services & NODE_BLOOM) && !pfrom.HasPermission(NetPermissionFlags::Mempool))
        {
            if (!pfrom.HasPermission(NetPermissionFlags::NoBan))
            {
                LogPrint(BCLog::NET, "mempool request with bloom filters disabled, disconnect peer=%d\n", pfrom.GetId());
                pfrom.fDisconnect = true;
            }
            return;
        }

        if (m_connman.OutboundTargetReached(false) && !pfrom.HasPermission(NetPermissionFlags::Mempool))
        {
            if (!pfrom.HasPermission(NetPermissionFlags::NoBan))
            {
                LogPrint(BCLog::NET, "mempool request with bandwidth limit reached, disconnect peer=%d\n", pfrom.GetId());
                pfrom.fDisconnect = true;
            }
            return;
        }

        if (auto tx_relay = peer->GetTxRelay(); tx_relay != nullptr) {
            LOCK(tx_relay->m_tx_inventory_mutex);
            tx_relay->m_send_mempool = true;
        }
        return;
    }

    if (msg_type == NetMsgType::PING) {
        if (pfrom.GetCommonVersion() > BIP0031_VERSION) {
            uint64_t nonce = 0;
            vRecv >> nonce;
            // Echo the message back with the nonce. This allows for two useful features:
            //
            // 1) A remote node can quickly check if the connection is operational
            // 2) Remote nodes can measure the latency of the network thread. If this node
            //    is overloaded it won't respond to pings quickly and the remote node can
            //    avoid sending us more work, like chain download requests.
            //
            // The nonce stops the remote getting confused between different pings: without
            // it, if the remote node sends a ping once per second and this node takes 5
            // seconds to respond to each, the 5th ping the remote sends would appear to
            // return very quickly.
            m_connman.PushMessage(&pfrom, msgMaker.Make(NetMsgType::PONG, nonce));
        }
        return;
    }

    if (msg_type == NetMsgType::PONG) {
        const auto ping_end = time_received;
        uint64_t nonce = 0;
        size_t nAvail = vRecv.in_avail();
        bool bPingFinished = false;
        std::string sProblem;

        if (nAvail >= sizeof(nonce)) {
            vRecv >> nonce;

            // Only process pong message if there is an outstanding ping (old ping without nonce should never pong)
            if (peer->m_ping_nonce_sent != 0) {
                if (nonce == peer->m_ping_nonce_sent) {
                    // Matching pong received, this ping is no longer outstanding
                    bPingFinished = true;
                    const auto ping_time = ping_end - peer->m_ping_start.load();
                    if (ping_time.count() >= 0) {
                        // Let connman know about this successful ping-pong
                        pfrom.PongReceived(ping_time);
                    } else {
                        // This should never happen
                        sProblem = "Timing mishap";
                    }
                } else {
                    // Nonce mismatches are normal when pings are overlapping
                    sProblem = "Nonce mismatch";
                    if (nonce == 0) {
                        // This is most likely a bug in another implementation somewhere; cancel this ping
                        bPingFinished = true;
                        sProblem = "Nonce zero";
                    }
                }
            } else {
                sProblem = "Unsolicited pong without ping";
            }
        } else {
            // This is most likely a bug in another implementation somewhere; cancel this ping
            bPingFinished = true;
            sProblem = "Short payload";
        }

        if (!(sProblem.empty())) {
            LogPrint(BCLog::NET, "pong peer=%d: %s, %x expected, %x received, %u bytes\n",
                pfrom.GetId(),
                sProblem,
                peer->m_ping_nonce_sent,
                nonce,
                nAvail);
        }
        if (bPingFinished) {
            peer->m_ping_nonce_sent = 0;
        }
        return;
    }

    if (msg_type == NetMsgType::FILTERLOAD) {
        if (!(peer->m_our_services & NODE_BLOOM)) {
            LogPrint(BCLog::NET, "filterload received despite not offering bloom services from peer=%d; disconnecting\n", pfrom.GetId());
            pfrom.fDisconnect = true;
            return;
        }
        CBloomFilter filter;
        vRecv >> filter;

        if (!filter.IsWithinSizeConstraints())
        {
            // There is no excuse for sending a too-large filter
            Misbehaving(*peer, 100, "too-large bloom filter");
        } else if (auto tx_relay = peer->GetTxRelay(); tx_relay != nullptr) {
            {
                LOCK(tx_relay->m_bloom_filter_mutex);
                tx_relay->m_bloom_filter.reset(new CBloomFilter(filter));
                tx_relay->m_relay_txs = true;
            }
            pfrom.m_bloom_filter_loaded = true;
            pfrom.m_relays_txs = true;
        }
        return;
    }

    if (msg_type == NetMsgType::FILTERADD) {
        if (!(peer->m_our_services & NODE_BLOOM)) {
            LogPrint(BCLog::NET, "filteradd received despite not offering bloom services from peer=%d; disconnecting\n", pfrom.GetId());
            pfrom.fDisconnect = true;
            return;
        }
        std::vector<unsigned char> vData;
        vRecv >> vData;

        // Nodes must NEVER send a data item > 520 bytes (the max size for a script data object,
        // and thus, the maximum size any matched object can have) in a filteradd message
        bool bad = false;
        if (vData.size() > MAX_SCRIPT_ELEMENT_SIZE) {
            bad = true;
        } else if (auto tx_relay = peer->GetTxRelay(); tx_relay != nullptr) {
            LOCK(tx_relay->m_bloom_filter_mutex);
            if (tx_relay->m_bloom_filter) {
                tx_relay->m_bloom_filter->insert(vData);
            } else {
                bad = true;
            }
        }
        if (bad) {
            Misbehaving(*peer, 100, "bad filteradd message");
        }
        return;
    }

    if (msg_type == NetMsgType::FILTERCLEAR) {
        if (!(peer->m_our_services & NODE_BLOOM)) {
            LogPrint(BCLog::NET, "filterclear received despite not offering bloom services from peer=%d; disconnecting\n", pfrom.GetId());
            pfrom.fDisconnect = true;
            return;
        }
        auto tx_relay = peer->GetTxRelay();
        if (!tx_relay) return;

        {
            LOCK(tx_relay->m_bloom_filter_mutex);
            tx_relay->m_bloom_filter = nullptr;
            tx_relay->m_relay_txs = true;
        }
        pfrom.m_bloom_filter_loaded = false;
        pfrom.m_relays_txs = true;
        return;
    }

    if (msg_type == NetMsgType::FEEFILTER) {
        CAmount newFeeFilter = 0;
        vRecv >> newFeeFilter;
        if (MoneyRange(newFeeFilter)) {
            if (auto tx_relay = peer->GetTxRelay(); tx_relay != nullptr) {
                tx_relay->m_fee_filter_received = newFeeFilter;
            }
            LogPrint(BCLog::NET, "received: feefilter of %s from peer=%d\n", CFeeRate(newFeeFilter).ToString(), pfrom.GetId());
        }
        return;
    }

    if (msg_type == NetMsgType::GETCFILTERS) {
        ProcessGetCFilters(pfrom, *peer, vRecv);
        return;
    }

    if (msg_type == NetMsgType::GETCFHEADERS) {
        ProcessGetCFHeaders(pfrom, *peer, vRecv);
        return;
    }

    if (msg_type == NetMsgType::GETCFCHECKPT) {
        ProcessGetCFCheckPt(pfrom, *peer, vRecv);
        return;
    }

    if (msg_type == NetMsgType::NOTFOUND) {
        std::vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() <= MAX_PEER_TX_ANNOUNCEMENTS + MAX_BLOCKS_IN_TRANSIT_PER_PEER) {
            LOCK(::cs_main);
            for (CInv &inv : vInv) {
                if (inv.IsGenTxMsg()) {
                    // If we receive a NOTFOUND message for a tx we requested, mark the announcement for it as
                    // completed in TxRequestTracker.
                    m_txrequest.ReceivedResponse(pfrom.GetId(), inv.hash);
                }
            }
        }
        return;
    }

    // Ignore unknown commands for extensibility
    LogPrint(BCLog::NET, "Unknown command \"%s\" from peer=%d\n", SanitizeString(msg_type), pfrom.GetId());
    return;
}

bool PeerManagerImpl::MaybeDiscourageAndDisconnect(CNode& pnode, Peer& peer)
{
    {
        LOCK(peer.m_misbehavior_mutex);

        // There's nothing to do if the m_should_discourage flag isn't set
        if (!peer.m_should_discourage) return false;

        peer.m_should_discourage = false;
    } // peer.m_misbehavior_mutex

    if (pnode.HasPermission(NetPermissionFlags::NoBan)) {
        // We never disconnect or discourage peers for bad behavior if they have NetPermissionFlags::NoBan permission
        LogPrintf("Warning: not punishing noban peer %d!\n", peer.m_id);
        return false;
    }

    if (pnode.IsManualConn()) {
        // We never disconnect or discourage manual peers for bad behavior
        LogPrintf("Warning: not punishing manually connected peer %d!\n", peer.m_id);
        return false;
    }

    if (pnode.addr.IsLocal()) {
        // We disconnect local peers for bad behavior but don't discourage (since that would discourage
        // all peers on the same local address)
        LogPrint(BCLog::NET, "Warning: disconnecting but not discouraging %s peer %d!\n",
                 pnode.m_inbound_onion ? "inbound onion" : "local", peer.m_id);
        pnode.fDisconnect = true;
        return true;
    }

    // Normal case: Disconnect the peer and discourage all nodes sharing the address
    LogPrint(BCLog::NET, "Disconnecting and discouraging peer %d!\n", peer.m_id);
    if (m_banman) m_banman->Discourage(pnode.addr);
    m_connman.DisconnectNode(pnode.addr);
    return true;
}

bool PeerManagerImpl::ProcessMessages(CNode* pfrom, std::atomic<bool>& interruptMsgProc)
{
    AssertLockHeld(g_msgproc_mutex);

    bool fMoreWork = false;

    PeerRef peer = GetPeerRef(pfrom->GetId());
    if (peer == nullptr) return false;

    {
        LOCK(peer->m_getdata_requests_mutex);
        if (!peer->m_getdata_requests.empty()) {
            ProcessGetData(*pfrom, *peer, interruptMsgProc);
        }
    }

    {
        LOCK2(cs_main, g_cs_orphans);
        if (!peer->m_orphan_work_set.empty()) {
            ProcessOrphanTx(peer->m_orphan_work_set);
        }
    }

    if (pfrom->fDisconnect)
        return false;

    // this maintains the order of responses
    // and prevents m_getdata_requests to grow unbounded
    {
        LOCK(peer->m_getdata_requests_mutex);
        if (!peer->m_getdata_requests.empty()) return true;
    }

    {
        LOCK(g_cs_orphans);
        if (!peer->m_orphan_work_set.empty()) return true;
    }

    // Don't bother if send buffer is too full to respond anyway
    if (pfrom->fPauseSend) return false;

    std::list<CNetMessage> msgs;
    {
        LOCK(pfrom->cs_vProcessMsg);
        if (pfrom->vProcessMsg.empty()) return false;
        // Just take one message
        msgs.splice(msgs.begin(), pfrom->vProcessMsg, pfrom->vProcessMsg.begin());
        pfrom->nProcessQueueSize -= msgs.front().m_raw_message_size;
        pfrom->fPauseRecv = pfrom->nProcessQueueSize > m_connman.GetReceiveFloodSize();
        fMoreWork = !pfrom->vProcessMsg.empty();
    }
    CNetMessage& msg(msgs.front());

    TRACE6(net, inbound_message,
        pfrom->GetId(),
        pfrom->m_addr_name.c_str(),
        pfrom->ConnectionTypeAsString().c_str(),
        msg.m_type.c_str(),
        msg.m_recv.size(),
        msg.m_recv.data()
    );

    if (gArgs.GetBoolArg("-capturemessages", false)) {
        CaptureMessage(pfrom->addr, msg.m_type, MakeUCharSpan(msg.m_recv), /*is_incoming=*/true);
    }

    msg.SetVersion(pfrom->GetCommonVersion());

    try {
        ProcessMessage(*pfrom, msg.m_type, msg.m_recv, msg.m_time, interruptMsgProc);
        if (interruptMsgProc) return false;
        {
            LOCK(peer->m_getdata_requests_mutex);
            if (!peer->m_getdata_requests.empty()) fMoreWork = true;
        }
    } catch (const std::exception& e) {
        LogPrint(BCLog::NET, "%s(%s, %u bytes): Exception '%s' (%s) caught\n", __func__, SanitizeString(msg.m_type), msg.m_message_size, e.what(), typeid(e).name());
    } catch (...) {
        LogPrint(BCLog::NET, "%s(%s, %u bytes): Unknown exception caught\n", __func__, SanitizeString(msg.m_type), msg.m_message_size);
    }

    return fMoreWork;
}

void PeerManagerImpl::ConsiderEviction(CNode& pto, Peer& peer, std::chrono::seconds time_in_seconds)
{
    AssertLockHeld(cs_main);

    CNodeState &state = *State(pto.GetId());
    const CNetMsgMaker msgMaker(pto.GetCommonVersion());

    if (!state.m_chain_sync.m_protect && pto.IsOutboundOrBlockRelayConn() && state.fSyncStarted) {
        // This is an outbound peer subject to disconnection if they don't
        // announce a block with as much work as the current tip within
        // CHAIN_SYNC_TIMEOUT + HEADERS_RESPONSE_TIME seconds (note: if
        // their chain has more work than ours, we should sync to it,
        // unless it's invalid, in which case we should find that out and
        // disconnect from them elsewhere).
        if (state.pindexBestKnownBlock != nullptr && state.pindexBestKnownBlock->nChainWork >= m_chainman.ActiveChain().Tip()->nChainWork) {
            if (state.m_chain_sync.m_timeout != 0s) {
                state.m_chain_sync.m_timeout = 0s;
                state.m_chain_sync.m_work_header = nullptr;
                state.m_chain_sync.m_sent_getheaders = false;
            }
        } else if (state.m_chain_sync.m_timeout == 0s || (state.m_chain_sync.m_work_header != nullptr && state.pindexBestKnownBlock != nullptr && state.pindexBestKnownBlock->nChainWork >= state.m_chain_sync.m_work_header->nChainWork)) {
            // Our best block known by this peer is behind our tip, and we're either noticing
            // that for the first time, OR this peer was able to catch up to some earlier point
            // where we checked against our tip.
            // Either way, set a new timeout based on current tip.
            state.m_chain_sync.m_timeout = time_in_seconds + CHAIN_SYNC_TIMEOUT;
            state.m_chain_sync.m_work_header = m_chainman.ActiveChain().Tip();
            state.m_chain_sync.m_sent_getheaders = false;
        } else if (state.m_chain_sync.m_timeout > 0s && time_in_seconds > state.m_chain_sync.m_timeout) {
            // No evidence yet that our peer has synced to a chain with work equal to that
            // of our tip, when we first detected it was behind. Send a single getheaders
            // message to give the peer a chance to update us.
            if (state.m_chain_sync.m_sent_getheaders) {
                // They've run out of time to catch up!
                LogPrintf("Disconnecting outbound peer %d for old chain, best known block = %s\n", pto.GetId(), state.pindexBestKnownBlock != nullptr ? state.pindexBestKnownBlock->GetBlockHash().ToString() : "<none>");
                pto.fDisconnect = true;
            } else {
                assert(state.m_chain_sync.m_work_header);
                // Here, we assume that the getheaders message goes out,
                // because it'll either go out or be skipped because of a
                // getheaders in-flight already, in which case the peer should
                // still respond to us with a sufficiently high work chain tip.
                MaybeSendGetHeaders(pto,
                        GetLocator(state.m_chain_sync.m_work_header->pprev),
                        peer);
                LogPrint(BCLog::NET, "sending getheaders to outbound peer=%d to verify chain work (current best known block:%s, benchmark blockhash: %s)\n", pto.GetId(), state.pindexBestKnownBlock != nullptr ? state.pindexBestKnownBlock->GetBlockHash().ToString() : "<none>", state.m_chain_sync.m_work_header->GetBlockHash().ToString());
                state.m_chain_sync.m_sent_getheaders = true;
                // Bump the timeout to allow a response, which could clear the timeout
                // (if the response shows the peer has synced), reset the timeout (if
                // the peer syncs to the required work but not to our tip), or result
                // in disconnect (if we advance to the timeout and pindexBestKnownBlock
                // has not sufficiently progressed)
                state.m_chain_sync.m_timeout = time_in_seconds + HEADERS_RESPONSE_TIME;
            }
        }
    }
}

void PeerManagerImpl::EvictExtraOutboundPeers(std::chrono::seconds now)
{
    // If we have any extra block-relay-only peers, disconnect the youngest unless
    // it's given us a block -- in which case, compare with the second-youngest, and
    // out of those two, disconnect the peer who least recently gave us a block.
    // The youngest block-relay-only peer would be the extra peer we connected
    // to temporarily in order to sync our tip; see net.cpp.
    // Note that we use higher nodeid as a measure for most recent connection.
    if (m_connman.GetExtraBlockRelayCount() > 0) {
        std::pair<NodeId, std::chrono::seconds> youngest_peer{-1, 0}, next_youngest_peer{-1, 0};

        m_connman.ForEachNode([&](CNode* pnode) {
            if (!pnode->IsBlockOnlyConn() || pnode->fDisconnect) return;
            if (pnode->GetId() > youngest_peer.first) {
                next_youngest_peer = youngest_peer;
                youngest_peer.first = pnode->GetId();
                youngest_peer.second = pnode->m_last_block_time;
            }
        });
        NodeId to_disconnect = youngest_peer.first;
        if (youngest_peer.second > next_youngest_peer.second) {
            // Our newest block-relay-only peer gave us a block more recently;
            // disconnect our second youngest.
            to_disconnect = next_youngest_peer.first;
        }
        m_connman.ForNode(to_disconnect, [&](CNode* pnode) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
            AssertLockHeld(::cs_main);
            // Make sure we're not getting a block right now, and that
            // we've been connected long enough for this eviction to happen
            // at all.
            // Note that we only request blocks from a peer if we learn of a
            // valid headers chain with at least as much work as our tip.
            CNodeState *node_state = State(pnode->GetId());
            if (node_state == nullptr ||
                (now - pnode->m_connected >= MINIMUM_CONNECT_TIME && node_state->nBlocksInFlight == 0)) {
                pnode->fDisconnect = true;
                LogPrint(BCLog::NET, "disconnecting extra block-relay-only peer=%d (last block received at time %d)\n",
                         pnode->GetId(), count_seconds(pnode->m_last_block_time));
                return true;
            } else {
                LogPrint(BCLog::NET, "keeping block-relay-only peer=%d chosen for eviction (connect time: %d, blocks_in_flight: %d)\n",
                         pnode->GetId(), count_seconds(pnode->m_connected), node_state->nBlocksInFlight);
            }
            return false;
        });
    }

    // Check whether we have too many outbound-full-relay peers
    if (m_connman.GetExtraFullOutboundCount() > 0) {
        // If we have more outbound-full-relay peers than we target, disconnect one.
        // Pick the outbound-full-relay peer that least recently announced
        // us a new block, with ties broken by choosing the more recent
        // connection (higher node id)
        NodeId worst_peer = -1;
        int64_t oldest_block_announcement = std::numeric_limits<int64_t>::max();

        m_connman.ForEachNode([&](CNode* pnode) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
            AssertLockHeld(::cs_main);

            // Only consider outbound-full-relay peers that are not already
            // marked for disconnection
            if (!pnode->IsFullOutboundConn() || pnode->fDisconnect) return;
            CNodeState *state = State(pnode->GetId());
            if (state == nullptr) return; // shouldn't be possible, but just in case
            // Don't evict our protected peers
            if (state->m_chain_sync.m_protect) return;
            if (state->m_last_block_announcement < oldest_block_announcement || (state->m_last_block_announcement == oldest_block_announcement && pnode->GetId() > worst_peer)) {
                worst_peer = pnode->GetId();
                oldest_block_announcement = state->m_last_block_announcement;
            }
        });
        if (worst_peer != -1) {
            bool disconnected = m_connman.ForNode(worst_peer, [&](CNode* pnode) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
                AssertLockHeld(::cs_main);

                // Only disconnect a peer that has been connected to us for
                // some reasonable fraction of our check-frequency, to give
                // it time for new information to have arrived.
                // Also don't disconnect any peer we're trying to download a
                // block from.
                CNodeState &state = *State(pnode->GetId());
                if (now - pnode->m_connected > MINIMUM_CONNECT_TIME && state.nBlocksInFlight == 0) {
                    LogPrint(BCLog::NET, "disconnecting extra outbound peer=%d (last block announcement received at time %d)\n", pnode->GetId(), oldest_block_announcement);
                    pnode->fDisconnect = true;
                    return true;
                } else {
                    LogPrint(BCLog::NET, "keeping outbound peer=%d chosen for eviction (connect time: %d, blocks_in_flight: %d)\n",
                             pnode->GetId(), count_seconds(pnode->m_connected), state.nBlocksInFlight);
                    return false;
                }
            });
            if (disconnected) {
                // If we disconnected an extra peer, that means we successfully
                // connected to at least one peer after the last time we
                // detected a stale tip. Don't try any more extra peers until
                // we next detect a stale tip, to limit the load we put on the
                // network from these extra connections.
                m_connman.SetTryNewOutboundPeer(false);
            }
        }
    }
}

void PeerManagerImpl::CheckForStaleTipAndEvictPeers()
{
    LOCK(cs_main);

    auto now{GetTime<std::chrono::seconds>()};

    EvictExtraOutboundPeers(now);

    if (now > m_stale_tip_check_time) {
        // Check whether our tip is stale, and if so, allow using an extra
        // outbound peer
        if (!fImporting && !fReindex && m_connman.GetNetworkActive() && m_connman.GetUseAddrmanOutgoing() && TipMayBeStale()) {
            LogPrintf("Potential stale tip detected, will try using extra outbound peer (last tip update: %d seconds ago)\n",
                      count_seconds(now - m_last_tip_update.load()));
            m_connman.SetTryNewOutboundPeer(true);
        } else if (m_connman.GetTryNewOutboundPeer()) {
            m_connman.SetTryNewOutboundPeer(false);
        }
        m_stale_tip_check_time = now + STALE_CHECK_INTERVAL;
    }

    if (!m_initial_sync_finished && CanDirectFetch()) {
        m_connman.StartExtraBlockRelayPeers();
        m_initial_sync_finished = true;
    }
}

void PeerManagerImpl::MaybeSendPing(CNode& node_to, Peer& peer, std::chrono::microseconds now)
{
    if (m_connman.ShouldRunInactivityChecks(node_to, std::chrono::duration_cast<std::chrono::seconds>(now)) &&
        peer.m_ping_nonce_sent &&
        now > peer.m_ping_start.load() + TIMEOUT_INTERVAL)
    {
        // The ping timeout is using mocktime. To disable the check during
        // testing, increase -peertimeout.
        LogPrint(BCLog::NET, "ping timeout: %fs peer=%d\n", 0.000001 * count_microseconds(now - peer.m_ping_start.load()), peer.m_id);
        node_to.fDisconnect = true;
        return;
    }

    const CNetMsgMaker msgMaker(node_to.GetCommonVersion());
    bool pingSend = false;

    if (peer.m_ping_queued) {
        // RPC ping request by user
        pingSend = true;
    }

    if (peer.m_ping_nonce_sent == 0 && now > peer.m_ping_start.load() + PING_INTERVAL) {
        // Ping automatically sent as a latency probe & keepalive.
        pingSend = true;
    }

    if (pingSend) {
        uint64_t nonce;
        do {
            nonce = GetRand<uint64_t>();
        } while (nonce == 0);
        peer.m_ping_queued = false;
        peer.m_ping_start = now;
        if (node_to.GetCommonVersion() > BIP0031_VERSION) {
            peer.m_ping_nonce_sent = nonce;
            m_connman.PushMessage(&node_to, msgMaker.Make(NetMsgType::PING, nonce));
        } else {
            // Peer is too old to support ping command with nonce, pong will never arrive.
            peer.m_ping_nonce_sent = 0;
            m_connman.PushMessage(&node_to, msgMaker.Make(NetMsgType::PING));
        }
    }
}

void PeerManagerImpl::MaybeSendAddr(CNode& node, Peer& peer, std::chrono::microseconds current_time)
{
    // Nothing to do for non-address-relay peers
    if (!peer.m_addr_relay_enabled) return;

    LOCK(peer.m_addr_send_times_mutex);
    // Periodically advertise our local address to the peer.
    if (fListen && !m_chainman.ActiveChainstate().IsInitialBlockDownload() &&
        peer.m_next_local_addr_send < current_time) {
        // If we've sent before, clear the bloom filter for the peer, so that our
        // self-announcement will actually go out.
        // This might be unnecessary if the bloom filter has already rolled
        // over since our last self-announcement, but there is only a small
        // bandwidth cost that we can incur by doing this (which happens
        // once a day on average).
        if (peer.m_next_local_addr_send != 0us) {
            peer.m_addr_known->reset();
        }
        if (std::optional<CService> local_service = GetLocalAddrForPeer(node)) {
            CAddress local_addr{*local_service, peer.m_our_services, Now<NodeSeconds>()};
            FastRandomContext insecure_rand;
            PushAddress(peer, local_addr, insecure_rand);
        }
        peer.m_next_local_addr_send = GetExponentialRand(current_time, AVG_LOCAL_ADDRESS_BROADCAST_INTERVAL);
    }

    // We sent an `addr` message to this peer recently. Nothing more to do.
    if (current_time <= peer.m_next_addr_send) return;

    peer.m_next_addr_send = GetExponentialRand(current_time, AVG_ADDRESS_BROADCAST_INTERVAL);

    if (!Assume(peer.m_addrs_to_send.size() <= MAX_ADDR_TO_SEND)) {
        // Should be impossible since we always check size before adding to
        // m_addrs_to_send. Recover by trimming the vector.
        peer.m_addrs_to_send.resize(MAX_ADDR_TO_SEND);
    }

    // Remove addr records that the peer already knows about, and add new
    // addrs to the m_addr_known filter on the same pass.
    auto addr_already_known = [&peer](const CAddress& addr) EXCLUSIVE_LOCKS_REQUIRED(g_msgproc_mutex) {
        bool ret = peer.m_addr_known->contains(addr.GetKey());
        if (!ret) peer.m_addr_known->insert(addr.GetKey());
        return ret;
    };
    peer.m_addrs_to_send.erase(std::remove_if(peer.m_addrs_to_send.begin(), peer.m_addrs_to_send.end(), addr_already_known),
                           peer.m_addrs_to_send.end());

    // No addr messages to send
    if (peer.m_addrs_to_send.empty()) return;

    const char* msg_type;
    int make_flags;
    if (peer.m_wants_addrv2) {
        msg_type = NetMsgType::ADDRV2;
        make_flags = ADDRV2_FORMAT;
    } else {
        msg_type = NetMsgType::ADDR;
        make_flags = 0;
    }
    m_connman.PushMessage(&node, CNetMsgMaker(node.GetCommonVersion()).Make(make_flags, msg_type, peer.m_addrs_to_send));
    peer.m_addrs_to_send.clear();

    // we only send the big addr message once
    if (peer.m_addrs_to_send.capacity() > 40) {
        peer.m_addrs_to_send.shrink_to_fit();
    }
}

void PeerManagerImpl::MaybeSendSendHeaders(CNode& node, Peer& peer)
{
    // Delay sending SENDHEADERS (BIP 130) until we're done with an
    // initial-headers-sync with this peer. Receiving headers announcements for
    // new blocks while trying to sync their headers chain is problematic,
    // because of the state tracking done.
    if (!peer.m_sent_sendheaders && node.GetCommonVersion() >= SENDHEADERS_VERSION) {
        LOCK(cs_main);
        CNodeState &state = *State(node.GetId());
        if (state.pindexBestKnownBlock != nullptr &&
                state.pindexBestKnownBlock->nChainWork > nMinimumChainWork) {
            // Tell our peer we prefer to receive headers rather than inv's
            // We send this to non-NODE NETWORK peers as well, because even
            // non-NODE NETWORK peers can announce blocks (such as pruning
            // nodes)
            m_connman.PushMessage(&node, CNetMsgMaker(node.GetCommonVersion()).Make(NetMsgType::SENDHEADERS));
            peer.m_sent_sendheaders = true;
        }
    }
}

void PeerManagerImpl::MaybeSendFeefilter(CNode& pto, Peer& peer, std::chrono::microseconds current_time)
{
    if (m_ignore_incoming_txs) return;
    if (pto.GetCommonVersion() < FEEFILTER_VERSION) return;
    // peers with the forcerelay permission should not filter txs to us
    if (pto.HasPermission(NetPermissionFlags::ForceRelay)) return;
    // Don't send feefilter messages to outbound block-relay-only peers since they should never announce
    // transactions to us, regardless of feefilter state.
    if (pto.IsBlockOnlyConn()) return;

    CAmount currentFilter = m_mempool.GetMinFee().GetFeePerK();
    static FeeFilterRounder g_filter_rounder{CFeeRate{DEFAULT_MIN_RELAY_TX_FEE}};

    if (m_chainman.ActiveChainstate().IsInitialBlockDownload()) {
        // Received tx-inv messages are discarded when the active
        // chainstate is in IBD, so tell the peer to not send them.
        currentFilter = MAX_MONEY;
    } else {
        static const CAmount MAX_FILTER{g_filter_rounder.round(MAX_MONEY)};
        if (peer.m_fee_filter_sent == MAX_FILTER) {
            // Send the current filter if we sent MAX_FILTER previously
            // and made it out of IBD.
            peer.m_next_send_feefilter = 0us;
        }
    }
    if (current_time > peer.m_next_send_feefilter) {
        CAmount filterToSend = g_filter_rounder.round(currentFilter);
        // We always have a fee filter of at least the min relay fee
        filterToSend = std::max(filterToSend, m_mempool.m_min_relay_feerate.GetFeePerK());
        if (filterToSend != peer.m_fee_filter_sent) {
            m_connman.PushMessage(&pto, CNetMsgMaker(pto.GetCommonVersion()).Make(NetMsgType::FEEFILTER, filterToSend));
            peer.m_fee_filter_sent = filterToSend;
        }
        peer.m_next_send_feefilter = GetExponentialRand(current_time, AVG_FEEFILTER_BROADCAST_INTERVAL);
    }
    // If the fee filter has changed substantially and it's still more than MAX_FEEFILTER_CHANGE_DELAY
    // until scheduled broadcast, then move the broadcast to within MAX_FEEFILTER_CHANGE_DELAY.
    else if (current_time + MAX_FEEFILTER_CHANGE_DELAY < peer.m_next_send_feefilter &&
                (currentFilter < 3 * peer.m_fee_filter_sent / 4 || currentFilter > 4 * peer.m_fee_filter_sent / 3)) {
        peer.m_next_send_feefilter = current_time + GetRandomDuration<std::chrono::microseconds>(MAX_FEEFILTER_CHANGE_DELAY);
    }
}

namespace {
class CompareInvMempoolOrder
{
    CTxMemPool* mp;
    bool m_wtxid_relay;
public:
    explicit CompareInvMempoolOrder(CTxMemPool *_mempool, bool use_wtxid)
    {
        mp = _mempool;
        m_wtxid_relay = use_wtxid;
    }

    bool operator()(std::set<uint256>::iterator a, std::set<uint256>::iterator b)
    {
        /* As std::make_heap produces a max-heap, we want the entries with the
         * fewest ancestors/highest fee to sort later. */
        return mp->CompareDepthAndScore(*b, *a, m_wtxid_relay);
    }
};
} // namespace

bool PeerManagerImpl::RejectIncomingTxs(const CNode& peer) const
{
    // block-relay-only peers may never send txs to us
    if (peer.IsBlockOnlyConn()) return true;
    // In -blocksonly mode, peers need the 'relay' permission to send txs to us
    if (m_ignore_incoming_txs && !peer.HasPermission(NetPermissionFlags::Relay)) return true;
    return false;
}

bool PeerManagerImpl::SetupAddressRelay(const CNode& node, Peer& peer)
{
    // We don't participate in addr relay with outbound block-relay-only
    // connections to prevent providing adversaries with the additional
    // information of addr traffic to infer the link.
    if (node.IsBlockOnlyConn()) return false;

    if (!peer.m_addr_relay_enabled.exchange(true)) {
        // First addr message we have received from the peer, initialize
        // m_addr_known
        peer.m_addr_known = std::make_unique<CRollingBloomFilter>(5000, 0.001);
    }

    return true;
}

bool PeerManagerImpl::SendMessages(CNode* pto)
{
    AssertLockHeld(g_msgproc_mutex);

    PeerRef peer = GetPeerRef(pto->GetId());
    if (!peer) return false;
    const Consensus::Params& consensusParams = m_chainparams.GetConsensus();

    // We must call MaybeDiscourageAndDisconnect first, to ensure that we'll
    // disconnect misbehaving peers even before the version handshake is complete.
    if (MaybeDiscourageAndDisconnect(*pto, *peer)) return true;

    // Don't send anything until the version handshake is complete
    if (!pto->fSuccessfullyConnected || pto->fDisconnect)
        return true;

    // If we get here, the outgoing message serialization version is set and can't change.
    const CNetMsgMaker msgMaker(pto->GetCommonVersion());

    const auto current_time{GetTime<std::chrono::microseconds>()};

    if (pto->IsAddrFetchConn() && current_time - pto->m_connected > 10 * AVG_ADDRESS_BROADCAST_INTERVAL) {
        LogPrint(BCLog::NET, "addrfetch connection timeout; disconnecting peer=%d\n", pto->GetId());
        pto->fDisconnect = true;
        return true;
    }

    MaybeSendPing(*pto, *peer, current_time);

    // MaybeSendPing may have marked peer for disconnection
    if (pto->fDisconnect) return true;

    MaybeSendAddr(*pto, *peer, current_time);

    MaybeSendSendHeaders(*pto, *peer);

    {
        LOCK(cs_main);

        CNodeState &state = *State(pto->GetId());

        // Start block sync
        if (m_chainman.m_best_header == nullptr) {
            m_chainman.m_best_header = m_chainman.ActiveChain().Tip();
        }

        // Determine whether we might try initial headers sync or parallel
        // block download from this peer -- this mostly affects behavior while
        // in IBD (once out of IBD, we sync from all peers).
        bool sync_blocks_and_headers_from_peer = false;
        if (state.fPreferredDownload) {
            sync_blocks_and_headers_from_peer = true;
        } else if (CanServeBlocks(*peer) && !pto->IsAddrFetchConn()) {
            // Typically this is an inbound peer. If we don't have any outbound
            // peers, or if we aren't downloading any blocks from such peers,
            // then allow block downloads from this peer, too.
            // We prefer downloading blocks from outbound peers to avoid
            // putting undue load on (say) some home user who is just making
            // outbound connections to the network, but if our only source of
            // the latest blocks is from an inbound peer, we have to be sure to
            // eventually download it (and not just wait indefinitely for an
            // outbound peer to have it).
            if (m_num_preferred_download_peers == 0 || mapBlocksInFlight.empty()) {
                sync_blocks_and_headers_from_peer = true;
            }
        }

        if (!state.fSyncStarted && CanServeBlocks(*peer) && !fImporting && !fReindex) {
            // Only actively request headers from a single peer, unless we're close to today.
            if ((nSyncStarted == 0 && sync_blocks_and_headers_from_peer) || m_chainman.m_best_header->Time() > GetAdjustedTime() - 24h) {
                const CBlockIndex* pindexStart = m_chainman.m_best_header;
                /* If possible, start at the block preceding the currently
                   best known header.  This ensures that we always get a
                   non-empty list of headers back as long as the peer
                   is up-to-date.  With a non-empty response, we can initialise
                   the peer's known best block.  This wouldn't be possible
                   if we requested starting at m_chainman.m_best_header and
                   got back an empty response.  */
                if (pindexStart->pprev)
                    pindexStart = pindexStart->pprev;
                if (MaybeSendGetHeaders(*pto, GetLocator(pindexStart), *peer)) {
                    LogPrint(BCLog::NET, "initial getheaders (%d) to peer=%d (startheight:%d)\n", pindexStart->nHeight, pto->GetId(), peer->m_starting_height);

                    state.fSyncStarted = true;
                    state.m_headers_sync_timeout = current_time + HEADERS_DOWNLOAD_TIMEOUT_BASE +
                        (
                         // Convert HEADERS_DOWNLOAD_TIMEOUT_PER_HEADER to microseconds before scaling
                         // to maintain precision
                         std::chrono::microseconds{HEADERS_DOWNLOAD_TIMEOUT_PER_HEADER} *
                         Ticks<std::chrono::seconds>(GetAdjustedTime() - m_chainman.m_best_header->Time()) / consensusParams.nPowTargetSpacing
                        );
                    nSyncStarted++;
                }
            }
        }

        //
        // Try sending block announcements via headers
        //
        {
            // If we have no more than MAX_BLOCKS_TO_ANNOUNCE in our
            // list of block hashes we're relaying, and our peer wants
            // headers announcements, then find the first header
            // not yet known to our peer but would connect, and send.
            // If no header would connect, or if we have too many
            // blocks, or if the peer doesn't want headers, just
            // add all to the inv queue.
            LOCK(peer->m_block_inv_mutex);
            std::vector<CBlock> vHeaders;
            bool fRevertToInv = ((!state.fPreferHeaders &&
                                 (!state.m_requested_hb_cmpctblocks || peer->m_blocks_for_headers_relay.size() > 1)) ||
                                 peer->m_blocks_for_headers_relay.size() > MAX_BLOCKS_TO_ANNOUNCE);
            const CBlockIndex *pBestIndex = nullptr; // last header queued for delivery
            ProcessBlockAvailability(pto->GetId()); // ensure pindexBestKnownBlock is up-to-date

            if (!fRevertToInv) {
                bool fFoundStartingHeader = false;
                // Try to find first header that our peer doesn't have, and
                // then send all headers past that one.  If we come across any
                // headers that aren't on m_chainman.ActiveChain(), give up.
                for (const uint256& hash : peer->m_blocks_for_headers_relay) {
                    const CBlockIndex* pindex = m_chainman.m_blockman.LookupBlockIndex(hash);
                    assert(pindex);
                    if (m_chainman.ActiveChain()[pindex->nHeight] != pindex) {
                        // Bail out if we reorged away from this block
                        fRevertToInv = true;
                        break;
                    }
                    if (pBestIndex != nullptr && pindex->pprev != pBestIndex) {
                        // This means that the list of blocks to announce don't
                        // connect to each other.
                        // This shouldn't really be possible to hit during
                        // regular operation (because reorgs should take us to
                        // a chain that has some block not on the prior chain,
                        // which should be caught by the prior check), but one
                        // way this could happen is by using invalidateblock /
                        // reconsiderblock repeatedly on the tip, causing it to
                        // be added multiple times to m_blocks_for_headers_relay.
                        // Robustly deal with this rare situation by reverting
                        // to an inv.
                        fRevertToInv = true;
                        break;
                    }
                    pBestIndex = pindex;
                    if (fFoundStartingHeader) {
                        // add this to the headers message
                        vHeaders.push_back(pindex->GetBlockHeader());
                    } else if (PeerHasHeader(&state, pindex)) {
                        continue; // keep looking for the first new block
                    } else if (pindex->pprev == nullptr || PeerHasHeader(&state, pindex->pprev)) {
                        // Peer doesn't have this header but they do have the prior one.
                        // Start sending headers.
                        fFoundStartingHeader = true;
                        vHeaders.push_back(pindex->GetBlockHeader());
                    } else {
                        // Peer doesn't have this header or the prior one -- nothing will
                        // connect, so bail out.
                        fRevertToInv = true;
                        break;
                    }
                }
            }
            if (!fRevertToInv && !vHeaders.empty()) {
                if (vHeaders.size() == 1 && state.m_requested_hb_cmpctblocks) {
                    // We only send up to 1 block as header-and-ids, as otherwise
                    // probably means we're doing an initial-ish-sync or they're slow
                    LogPrint(BCLog::NET, "%s sending header-and-ids %s to peer=%d\n", __func__,
                            vHeaders.front().GetHash().ToString(), pto->GetId());

                    std::optional<CSerializedNetMsg> cached_cmpctblock_msg;
                    {
                        LOCK(m_most_recent_block_mutex);
                        if (m_most_recent_block_hash == pBestIndex->GetBlockHash()) {
                            cached_cmpctblock_msg = msgMaker.Make(NetMsgType::CMPCTBLOCK, *m_most_recent_compact_block);
                        }
                    }
                    if (cached_cmpctblock_msg.has_value()) {
                        m_connman.PushMessage(pto, std::move(cached_cmpctblock_msg.value()));
                    } else {
                        CBlock block;
                        bool ret = ReadBlockFromDisk(block, pBestIndex, consensusParams);
                        assert(ret);
                        CBlockHeaderAndShortTxIDs cmpctblock{block};
                        m_connman.PushMessage(pto, msgMaker.Make(NetMsgType::CMPCTBLOCK, cmpctblock));
                    }
                    state.pindexBestHeaderSent = pBestIndex;
                } else if (state.fPreferHeaders) {
                    if (vHeaders.size() > 1) {
                        LogPrint(BCLog::NET, "%s: %u headers, range (%s, %s), to peer=%d\n", __func__,
                                vHeaders.size(),
                                vHeaders.front().GetHash().ToString(),
                                vHeaders.back().GetHash().ToString(), pto->GetId());
                    } else {
                        LogPrint(BCLog::NET, "%s: sending header %s to peer=%d\n", __func__,
                                vHeaders.front().GetHash().ToString(), pto->GetId());
                    }
                    m_connman.PushMessage(pto, msgMaker.Make(NetMsgType::HEADERS, vHeaders));
                    state.pindexBestHeaderSent = pBestIndex;
                } else
                    fRevertToInv = true;
            }
            if (fRevertToInv) {
                // If falling back to using an inv, just try to inv the tip.
                // The last entry in m_blocks_for_headers_relay was our tip at some point
                // in the past.
                if (!peer->m_blocks_for_headers_relay.empty()) {
                    const uint256& hashToAnnounce = peer->m_blocks_for_headers_relay.back();
                    const CBlockIndex* pindex = m_chainman.m_blockman.LookupBlockIndex(hashToAnnounce);
                    assert(pindex);

                    // Warn if we're announcing a block that is not on the main chain.
                    // This should be very rare and could be optimized out.
                    // Just log for now.
                    if (m_chainman.ActiveChain()[pindex->nHeight] != pindex) {
                        LogPrint(BCLog::NET, "Announcing block %s not on main chain (tip=%s)\n",
                            hashToAnnounce.ToString(), m_chainman.ActiveChain().Tip()->GetBlockHash().ToString());
                    }

                    // If the peer's chain has this block, don't inv it back.
                    if (!PeerHasHeader(&state, pindex)) {
                        peer->m_blocks_for_inv_relay.push_back(hashToAnnounce);
                        LogPrint(BCLog::NET, "%s: sending inv peer=%d hash=%s\n", __func__,
                            pto->GetId(), hashToAnnounce.ToString());
                    }
                }
            }
            peer->m_blocks_for_headers_relay.clear();
        }

        //
        // Message: inventory
        //
        std::vector<CInv> vInv;
        {
            LOCK(peer->m_block_inv_mutex);
            vInv.reserve(std::max<size_t>(peer->m_blocks_for_inv_relay.size(), INVENTORY_BROADCAST_MAX));

            // Add blocks
            for (const uint256& hash : peer->m_blocks_for_inv_relay) {
                vInv.push_back(CInv(MSG_BLOCK, hash));
                if (vInv.size() == MAX_INV_SZ) {
                    m_connman.PushMessage(pto, msgMaker.Make(NetMsgType::INV, vInv));
                    vInv.clear();
                }
            }
            peer->m_blocks_for_inv_relay.clear();
        }

        if (auto tx_relay = peer->GetTxRelay(); tx_relay != nullptr) {
                LOCK(tx_relay->m_tx_inventory_mutex);
                // Check whether periodic sends should happen
                bool fSendTrickle = pto->HasPermission(NetPermissionFlags::NoBan);
                if (tx_relay->m_next_inv_send_time < current_time) {
                    fSendTrickle = true;
                    if (pto->IsInboundConn()) {
                        tx_relay->m_next_inv_send_time = NextInvToInbounds(current_time, INBOUND_INVENTORY_BROADCAST_INTERVAL);
                    } else {
                        tx_relay->m_next_inv_send_time = GetExponentialRand(current_time, OUTBOUND_INVENTORY_BROADCAST_INTERVAL);
                    }
                }

                // Time to send but the peer has requested we not relay transactions.
                if (fSendTrickle) {
                    LOCK(tx_relay->m_bloom_filter_mutex);
                    if (!tx_relay->m_relay_txs) tx_relay->m_tx_inventory_to_send.clear();
                }

                // Respond to BIP35 mempool requests
                if (fSendTrickle && tx_relay->m_send_mempool) {
                    auto vtxinfo = m_mempool.infoAll();
                    tx_relay->m_send_mempool = false;
                    const CFeeRate filterrate{tx_relay->m_fee_filter_received.load()};

                    LOCK(tx_relay->m_bloom_filter_mutex);

                    for (const auto& txinfo : vtxinfo) {
                        const uint256& hash = peer->m_wtxid_relay ? txinfo.tx->GetWitnessHash() : txinfo.tx->GetHash();
                        CInv inv(peer->m_wtxid_relay ? MSG_WTX : MSG_TX, hash);
                        tx_relay->m_tx_inventory_to_send.erase(hash);
                        // Don't send transactions that peers will not put into their mempool
                        if (txinfo.fee < filterrate.GetFee(txinfo.vsize)) {
                            continue;
                        }
                        if (tx_relay->m_bloom_filter) {
                            if (!tx_relay->m_bloom_filter->IsRelevantAndUpdate(*txinfo.tx)) continue;
                        }
                        tx_relay->m_tx_inventory_known_filter.insert(hash);
                        // Responses to MEMPOOL requests bypass the m_recently_announced_invs filter.
                        vInv.push_back(inv);
                        if (vInv.size() == MAX_INV_SZ) {
                            m_connman.PushMessage(pto, msgMaker.Make(NetMsgType::INV, vInv));
                            vInv.clear();
                        }
                    }
                    tx_relay->m_last_mempool_req = std::chrono::duration_cast<std::chrono::seconds>(current_time);
                }

                // Determine transactions to relay
                if (fSendTrickle) {
                    // Produce a vector with all candidates for sending
                    std::vector<std::set<uint256>::iterator> vInvTx;
                    vInvTx.reserve(tx_relay->m_tx_inventory_to_send.size());
                    for (std::set<uint256>::iterator it = tx_relay->m_tx_inventory_to_send.begin(); it != tx_relay->m_tx_inventory_to_send.end(); it++) {
                        vInvTx.push_back(it);
                    }
                    const CFeeRate filterrate{tx_relay->m_fee_filter_received.load()};
                    // Topologically and fee-rate sort the inventory we send for privacy and priority reasons.
                    // A heap is used so that not all items need sorting if only a few are being sent.
                    CompareInvMempoolOrder compareInvMempoolOrder(&m_mempool, peer->m_wtxid_relay);
                    std::make_heap(vInvTx.begin(), vInvTx.end(), compareInvMempoolOrder);
                    // No reason to drain out at many times the network's capacity,
                    // especially since we have many peers and some will draw much shorter delays.
                    unsigned int nRelayedTransactions = 0;
                    LOCK(tx_relay->m_bloom_filter_mutex);
                    while (!vInvTx.empty() && nRelayedTransactions < INVENTORY_BROADCAST_MAX) {
                        // Fetch the top element from the heap
                        std::pop_heap(vInvTx.begin(), vInvTx.end(), compareInvMempoolOrder);
                        std::set<uint256>::iterator it = vInvTx.back();
                        vInvTx.pop_back();
                        uint256 hash = *it;
                        CInv inv(peer->m_wtxid_relay ? MSG_WTX : MSG_TX, hash);
                        // Remove it from the to-be-sent set
                        tx_relay->m_tx_inventory_to_send.erase(it);
                        // Check if not in the filter already
                        if (tx_relay->m_tx_inventory_known_filter.contains(hash)) {
                            continue;
                        }
                        // Not in the mempool anymore? don't bother sending it.
                        auto txinfo = m_mempool.info(ToGenTxid(inv));
                        if (!txinfo.tx) {
                            continue;
                        }
                        auto txid = txinfo.tx->GetHash();
                        auto wtxid = txinfo.tx->GetWitnessHash();
                        // Peer told you to not send transactions at that feerate? Don't bother sending it.
                        if (txinfo.fee < filterrate.GetFee(txinfo.vsize)) {
                            continue;
                        }
                        if (tx_relay->m_bloom_filter && !tx_relay->m_bloom_filter->IsRelevantAndUpdate(*txinfo.tx)) continue;
                        // Send
                        State(pto->GetId())->m_recently_announced_invs.insert(hash);
                        vInv.push_back(inv);
                        nRelayedTransactions++;
                        {
                            // Expire old relay messages
                            while (!g_relay_expiration.empty() && g_relay_expiration.front().first < current_time)
                            {
                                mapRelay.erase(g_relay_expiration.front().second);
                                g_relay_expiration.pop_front();
                            }

                            auto ret = mapRelay.emplace(txid, std::move(txinfo.tx));
                            if (ret.second) {
                                g_relay_expiration.emplace_back(current_time + RELAY_TX_CACHE_TIME, ret.first);
                            }
                            // Add wtxid-based lookup into mapRelay as well, so that peers can request by wtxid
                            auto ret2 = mapRelay.emplace(wtxid, ret.first->second);
                            if (ret2.second) {
                                g_relay_expiration.emplace_back(current_time + RELAY_TX_CACHE_TIME, ret2.first);
                            }
                        }
                        if (vInv.size() == MAX_INV_SZ) {
                            m_connman.PushMessage(pto, msgMaker.Make(NetMsgType::INV, vInv));
                            vInv.clear();
                        }
                        tx_relay->m_tx_inventory_known_filter.insert(hash);
                        if (hash != txid) {
                            // Insert txid into m_tx_inventory_known_filter, even for
                            // wtxidrelay peers. This prevents re-adding of
                            // unconfirmed parents to the recently_announced
                            // filter, when a child tx is requested. See
                            // ProcessGetData().
                            tx_relay->m_tx_inventory_known_filter.insert(txid);
                        }
                    }
                }
        }
        if (!vInv.empty())
            m_connman.PushMessage(pto, msgMaker.Make(NetMsgType::INV, vInv));

        // Detect whether we're stalling
        if (state.m_stalling_since.count() && state.m_stalling_since < current_time - BLOCK_STALLING_TIMEOUT) {
            // Stalling only triggers when the block download window cannot move. During normal steady state,
            // the download window should be much larger than the to-be-downloaded set of blocks, so disconnection
            // should only happen during initial block download.
            LogPrintf("Peer=%d is stalling block download, disconnecting\n", pto->GetId());
            pto->fDisconnect = true;
            return true;
        }
        // In case there is a block that has been in flight from this peer for block_interval * (1 + 0.5 * N)
        // (with N the number of peers from which we're downloading validated blocks), disconnect due to timeout.
        // We compensate for other peers to prevent killing off peers due to our own downstream link
        // being saturated. We only count validated in-flight blocks so peers can't advertise non-existing block hashes
        // to unreasonably increase our timeout.
        if (state.vBlocksInFlight.size() > 0) {
            QueuedBlock &queuedBlock = state.vBlocksInFlight.front();
            int nOtherPeersWithValidatedDownloads = m_peers_downloading_from - 1;
            if (current_time > state.m_downloading_since + std::chrono::seconds{consensusParams.nPowTargetSpacing} * (BLOCK_DOWNLOAD_TIMEOUT_BASE + BLOCK_DOWNLOAD_TIMEOUT_PER_PEER * nOtherPeersWithValidatedDownloads)) {
                LogPrintf("Timeout downloading block %s from peer=%d, disconnecting\n", queuedBlock.pindex->GetBlockHash().ToString(), pto->GetId());
                pto->fDisconnect = true;
                return true;
            }
        }
        // Check for headers sync timeouts
        if (state.fSyncStarted && state.m_headers_sync_timeout < std::chrono::microseconds::max()) {
            // Detect whether this is a stalling initial-headers-sync peer
            if (m_chainman.m_best_header->Time() <= GetAdjustedTime() - 24h) {
                if (current_time > state.m_headers_sync_timeout && nSyncStarted == 1 && (m_num_preferred_download_peers - state.fPreferredDownload >= 1)) {
                    // Disconnect a peer (without NetPermissionFlags::NoBan permission) if it is our only sync peer,
                    // and we have others we could be using instead.
                    // Note: If all our peers are inbound, then we won't
                    // disconnect our sync peer for stalling; we have bigger
                    // problems if we can't get any outbound peers.
                    if (!pto->HasPermission(NetPermissionFlags::NoBan)) {
                        LogPrintf("Timeout downloading headers from peer=%d, disconnecting\n", pto->GetId());
                        pto->fDisconnect = true;
                        return true;
                    } else {
                        LogPrintf("Timeout downloading headers from noban peer=%d, not disconnecting\n", pto->GetId());
                        // Reset the headers sync state so that we have a
                        // chance to try downloading from a different peer.
                        // Note: this will also result in at least one more
                        // getheaders message to be sent to
                        // this peer (eventually).
                        state.fSyncStarted = false;
                        nSyncStarted--;
                        state.m_headers_sync_timeout = 0us;
                    }
                }
            } else {
                // After we've caught up once, reset the timeout so we can't trigger
                // disconnect later.
                state.m_headers_sync_timeout = std::chrono::microseconds::max();
            }
        }

        // Check that outbound peers have reasonable chains
        // GetTime() is used by this anti-DoS logic so we can test this using mocktime
        ConsiderEviction(*pto, *peer, GetTime<std::chrono::seconds>());

        //
        // Message: getdata (blocks)
        //
        std::vector<CInv> vGetData;
        if (CanServeBlocks(*peer) && ((sync_blocks_and_headers_from_peer && !IsLimitedPeer(*peer)) || !m_chainman.ActiveChainstate().IsInitialBlockDownload()) && state.nBlocksInFlight < MAX_BLOCKS_IN_TRANSIT_PER_PEER) {
            std::vector<const CBlockIndex*> vToDownload;
            NodeId staller = -1;
            FindNextBlocksToDownload(*peer, MAX_BLOCKS_IN_TRANSIT_PER_PEER - state.nBlocksInFlight, vToDownload, staller);
            for (const CBlockIndex *pindex : vToDownload) {
                uint32_t nFetchFlags = GetFetchFlags(*peer);
                vGetData.push_back(CInv(MSG_BLOCK | nFetchFlags, pindex->GetBlockHash()));
                BlockRequested(pto->GetId(), *pindex);
                LogPrint(BCLog::NET, "Requesting block %s (%d) peer=%d\n", pindex->GetBlockHash().ToString(),
                    pindex->nHeight, pto->GetId());
            }
            if (state.nBlocksInFlight == 0 && staller != -1) {
                if (State(staller)->m_stalling_since == 0us) {
                    State(staller)->m_stalling_since = current_time;
                    LogPrint(BCLog::NET, "Stall started peer=%d\n", staller);
                }
            }
        }

        //
        // Message: getdata (transactions)
        //
        std::vector<std::pair<NodeId, GenTxid>> expired;
        auto requestable = m_txrequest.GetRequestable(pto->GetId(), current_time, &expired);
        for (const auto& entry : expired) {
            LogPrint(BCLog::NET, "timeout of inflight %s %s from peer=%d\n", entry.second.IsWtxid() ? "wtx" : "tx",
                entry.second.GetHash().ToString(), entry.first);
        }
        for (const GenTxid& gtxid : requestable) {
            if (!AlreadyHaveTx(gtxid)) {
                LogPrint(BCLog::NET, "Requesting %s %s peer=%d\n", gtxid.IsWtxid() ? "wtx" : "tx",
                    gtxid.GetHash().ToString(), pto->GetId());
                vGetData.emplace_back(gtxid.IsWtxid() ? MSG_WTX : (MSG_TX | GetFetchFlags(*peer)), gtxid.GetHash());
                if (vGetData.size() >= MAX_GETDATA_SZ) {
                    m_connman.PushMessage(pto, msgMaker.Make(NetMsgType::GETDATA, vGetData));
                    vGetData.clear();
                }
                m_txrequest.RequestedTx(pto->GetId(), gtxid.GetHash(), current_time + GETDATA_TX_INTERVAL);
            } else {
                // We have already seen this transaction, no need to download. This is just a belt-and-suspenders, as
                // this should already be called whenever a transaction becomes AlreadyHaveTx().
                m_txrequest.ForgetTxHash(gtxid.GetHash());
            }
        }


        if (!vGetData.empty())
            m_connman.PushMessage(pto, msgMaker.Make(NetMsgType::GETDATA, vGetData));
    } // release cs_main
    MaybeSendFeefilter(*pto, *peer, current_time);
    return true;
}
