// Copyright (c) 2019-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <hash.h>
#include <net.h>
#include <netmessagemaker.h>
#include <protocol.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <test/util/xoroshiro128plusplus.h>
#include <util/chaintype.h>

#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace {

std::vector<std::string> g_all_messages;

void initialize_p2p_transport_serialization()
{
    SelectParams(ChainType::REGTEST);
    g_all_messages = getAllNetMessageTypes();
    std::sort(g_all_messages.begin(), g_all_messages.end());
}

} // namespace

FUZZ_TARGET(p2p_transport_serialization, .init = initialize_p2p_transport_serialization)
{
    // Construct transports for both sides, with dummy NodeIds.
    V1Transport recv_transport{NodeId{0}, SER_NETWORK, INIT_PROTO_VERSION};
    V1Transport send_transport{NodeId{1}, SER_NETWORK, INIT_PROTO_VERSION};

    FuzzedDataProvider fuzzed_data_provider{buffer.data(), buffer.size()};

    auto checksum_assist = fuzzed_data_provider.ConsumeBool();
    auto magic_bytes_assist = fuzzed_data_provider.ConsumeBool();
    std::vector<uint8_t> mutable_msg_bytes;

    auto header_bytes_remaining = CMessageHeader::HEADER_SIZE;
    if (magic_bytes_assist) {
        auto msg_start = Params().MessageStart();
        for (size_t i = 0; i < CMessageHeader::MESSAGE_SIZE_SIZE; ++i) {
            mutable_msg_bytes.push_back(msg_start[i]);
        }
        header_bytes_remaining -= CMessageHeader::MESSAGE_SIZE_SIZE;
    }

    if (checksum_assist) {
        header_bytes_remaining -= CMessageHeader::CHECKSUM_SIZE;
    }

    auto header_random_bytes = fuzzed_data_provider.ConsumeBytes<uint8_t>(header_bytes_remaining);
    mutable_msg_bytes.insert(mutable_msg_bytes.end(), header_random_bytes.begin(), header_random_bytes.end());
    auto payload_bytes = fuzzed_data_provider.ConsumeRemainingBytes<uint8_t>();

    if (checksum_assist && mutable_msg_bytes.size() == CMessageHeader::CHECKSUM_OFFSET) {
        CHash256 hasher;
        unsigned char hsh[32];
        hasher.Write(payload_bytes);
        hasher.Finalize(hsh);
        for (size_t i = 0; i < CMessageHeader::CHECKSUM_SIZE; ++i) {
           mutable_msg_bytes.push_back(hsh[i]);
        }
    }

    mutable_msg_bytes.insert(mutable_msg_bytes.end(), payload_bytes.begin(), payload_bytes.end());
    Span<const uint8_t> msg_bytes{mutable_msg_bytes};
    while (msg_bytes.size() > 0) {
        if (!recv_transport.ReceivedBytes(msg_bytes)) {
            break;
        }
        if (recv_transport.ReceivedMessageComplete()) {
            const std::chrono::microseconds m_time{std::numeric_limits<int64_t>::max()};
            bool reject_message{false};
            CNetMessage msg = recv_transport.GetReceivedMessage(m_time, reject_message);
            assert(msg.m_type.size() <= CMessageHeader::COMMAND_SIZE);
            assert(msg.m_raw_message_size <= mutable_msg_bytes.size());
            assert(msg.m_raw_message_size == CMessageHeader::HEADER_SIZE + msg.m_message_size);
            assert(msg.m_time == m_time);

            std::vector<unsigned char> header;
            auto msg2 = CNetMsgMaker{msg.m_recv.GetVersion()}.Make(msg.m_type, Span{msg.m_recv});
            bool queued = send_transport.SetMessageToSend(msg2);
            assert(queued);
            std::optional<bool> known_more;
            while (true) {
                const auto& [to_send, more, _msg_type] = send_transport.GetBytesToSend();
                if (known_more) assert(!to_send.empty() == *known_more);
                if (to_send.empty()) break;
                send_transport.MarkBytesSent(to_send.size());
                known_more = more;
            }
        }
    }
}

namespace {

template<typename R>
void SimulationTest(Transport& initiator, Transport& responder, R& rng, FuzzedDataProvider& provider)
{
    // Simulation test with two Transport objects, which send messages to each other, with
    // sending and receiving fragmented into multiple pieces that may be interleaved. It primarily
    // verifies that the sending and receiving side are compatible with each other, plus a few
    // sanity checks. It does not attempt to introduce errors in the communicated data.

    // Put the transports in an array for by-index access.
    const std::array<Transport*, 2> transports = {&initiator, &responder};

    // Two vectors representing in-flight bytes. inflight[i] is from transport[i] to transport[!i].
    std::array<std::vector<uint8_t>, 2> in_flight;

    // Two queues with expected messages. expected[i] is expected to arrive in transport[!i].
    std::array<std::deque<CSerializedNetMsg>, 2> expected;

    // Vectors with bytes last returned by GetBytesToSend() on transport[i].
    std::array<std::vector<uint8_t>, 2> to_send;

    // Last returned 'more' values (if still relevant) by transport[i]->GetBytesToSend().
    std::array<std::optional<bool>, 2> last_more;

    // Whether more bytes to be sent are expected on transport[i].
    std::array<std::optional<bool>, 2> expect_more;

    // Function to consume a message type.
    auto msg_type_fn = [&]() {
        uint8_t v = provider.ConsumeIntegral<uint8_t>();
        if (v == 0xFF) {
            // If v is 0xFF, construct a valid (but possibly unknown) message type from the fuzz
            // data.
            std::string ret;
            while (ret.size() < CMessageHeader::COMMAND_SIZE) {
                char c = provider.ConsumeIntegral<char>();
                // Match the allowed characters in CMessageHeader::IsCommandValid(). Any other
                // character is interpreted as end.
                if (c < ' ' || c > 0x7E) break;
                ret += c;
            }
            return ret;
        } else {
            // Otherwise, use it as index into the list of known messages.
            return g_all_messages[v % g_all_messages.size()];
        }
    };

    // Function to construct a CSerializedNetMsg to send.
    auto make_msg_fn = [&](bool first) {
        CSerializedNetMsg msg;
        if (first) {
            // Always send a "version" message as first one.
            msg.m_type = "version";
        } else {
            msg.m_type = msg_type_fn();
        }
        // Determine size of message to send (limited to 75 kB for performance reasons).
        size_t size = provider.ConsumeIntegralInRange<uint32_t>(0, 75000);
        // Get payload of message from RNG.
        msg.data.resize(size);
        for (auto& v : msg.data) v = uint8_t(rng());
        // Return.
        return msg;
    };

    // The next message to be sent (initially version messages, but will be replaced once sent).
    std::array<CSerializedNetMsg, 2> next_msg = {
        make_msg_fn(/*first=*/true),
        make_msg_fn(/*first=*/true)
    };

    // Wrapper around transport[i]->GetBytesToSend() that performs sanity checks.
    auto bytes_to_send_fn = [&](int side) -> Transport::BytesToSend {
        const auto& [bytes, more, msg_type] = transports[side]->GetBytesToSend();
        // Compare with expected more.
        if (expect_more[side].has_value()) assert(!bytes.empty() == *expect_more[side]);
        // Compare with previously reported output.
        assert(to_send[side].size() <= bytes.size());
        assert(to_send[side] == Span{bytes}.first(to_send[side].size()));
        to_send[side].resize(bytes.size());
        std::copy(bytes.begin(), bytes.end(), to_send[side].begin());
        // Remember 'more' result.
        last_more[side] = {more};
        // Return.
        return {bytes, more, msg_type};
    };

    // Function to make side send a new message.
    auto new_msg_fn = [&](int side) {
        // Don't do anything if there are too many unreceived messages already.
        if (expected[side].size() >= 16) return;
        // Try to send (a copy of) the message in next_msg[side].
        CSerializedNetMsg msg = next_msg[side].Copy();
        bool queued = transports[side]->SetMessageToSend(msg);
        // Update expected more data.
        expect_more[side] = std::nullopt;
        // Verify consistency of GetBytesToSend after SetMessageToSend
        bytes_to_send_fn(/*side=*/side);
        if (queued) {
            // Remember that this message is now expected by the receiver.
            expected[side].emplace_back(std::move(next_msg[side]));
            // Construct a new next message to send.
            next_msg[side] = make_msg_fn(/*first=*/false);
        }
    };

    // Function to make side send out bytes (if any).
    auto send_fn = [&](int side, bool everything = false) {
        const auto& [bytes, more, msg_type] = bytes_to_send_fn(/*side=*/side);
        // Don't do anything if no bytes to send.
        if (bytes.empty()) return false;
        size_t send_now = everything ? bytes.size() : provider.ConsumeIntegralInRange<size_t>(0, bytes.size());
        if (send_now == 0) return false;
        // Add bytes to the in-flight queue, and mark those bytes as consumed.
        in_flight[side].insert(in_flight[side].end(), bytes.begin(), bytes.begin() + send_now);
        transports[side]->MarkBytesSent(send_now);
        // If all to-be-sent bytes were sent, move last_more data to expect_more data.
        if (send_now == bytes.size()) {
            expect_more[side] = last_more[side];
        }
        // Remove the bytes from the last reported to-be-sent vector.
        assert(to_send[side].size() >= send_now);
        to_send[side].erase(to_send[side].begin(), to_send[side].begin() + send_now);
        // Verify that GetBytesToSend gives a result consistent with earlier.
        bytes_to_send_fn(/*side=*/side);
        // Return whether anything was sent.
        return send_now > 0;
    };

    // Function to make !side receive bytes (if any).
    auto recv_fn = [&](int side, bool everything = false) {
        // Don't do anything if no bytes in flight.
        if (in_flight[side].empty()) return false;
        // Decide span to receive
        size_t to_recv_len = in_flight[side].size();
        if (!everything) to_recv_len = provider.ConsumeIntegralInRange<size_t>(0, to_recv_len);
        Span<const uint8_t> to_recv = Span{in_flight[side]}.first(to_recv_len);
        // Process those bytes
        while (!to_recv.empty()) {
            size_t old_len = to_recv.size();
            bool ret = transports[!side]->ReceivedBytes(to_recv);
            // Bytes must always be accepted, as this test does not introduce any errors in
            // communication.
            assert(ret);
            // Clear cached expected 'more' information: if certainly no more data was to be sent
            // before, receiving bytes makes this uncertain.
            if (expect_more[!side] == false) expect_more[!side] = std::nullopt;
            // Verify consistency of GetBytesToSend after ReceivedBytes
            bytes_to_send_fn(/*side=*/!side);
            bool progress = to_recv.size() < old_len;
            if (transports[!side]->ReceivedMessageComplete()) {
                bool reject{false};
                auto received = transports[!side]->GetReceivedMessage({}, reject);
                // Receiving must succeed.
                assert(!reject);
                // There must be a corresponding expected message.
                assert(!expected[side].empty());
                // The m_message_size field must be correct.
                assert(received.m_message_size == received.m_recv.size());
                // The m_type must match what is expected.
                assert(received.m_type == expected[side].front().m_type);
                // The data must match what is expected.
                assert(MakeByteSpan(received.m_recv) == MakeByteSpan(expected[side].front().data));
                expected[side].pop_front();
                progress = true;
            }
            // Progress must be made (by processing incoming bytes and/or returning complete
            // messages) until all received bytes are processed.
            assert(progress);
        }
        // Remove the processed bytes from the in_flight buffer.
        in_flight[side].erase(in_flight[side].begin(), in_flight[side].begin() + to_recv_len);
        // Return whether anything was received.
        return to_recv_len > 0;
    };

    // Main loop, interleaving new messages, sends, and receives.
    LIMITED_WHILE(provider.remaining_bytes(), 1000) {
        CallOneOf(provider,
            // (Try to) give the next message to the transport.
            [&] { new_msg_fn(/*side=*/0); },
            [&] { new_msg_fn(/*side=*/1); },
            // (Try to) send some bytes from the transport to the network.
            [&] { send_fn(/*side=*/0); },
            [&] { send_fn(/*side=*/1); },
            // (Try to) receive bytes from the network, converting to messages.
            [&] { recv_fn(/*side=*/0); },
            [&] { recv_fn(/*side=*/1); }
        );
    }

    // When we're done, perform sends and receives of existing messages to flush anything already
    // in flight.
    while (true) {
        bool any = false;
        if (send_fn(/*side=*/0, /*everything=*/true)) any = true;
        if (send_fn(/*side=*/1, /*everything=*/true)) any = true;
        if (recv_fn(/*side=*/0, /*everything=*/true)) any = true;
        if (recv_fn(/*side=*/1, /*everything=*/true)) any = true;
        if (!any) break;
    }

    // Make sure nothing is left in flight.
    assert(in_flight[0].empty());
    assert(in_flight[1].empty());

    // Make sure all expected messages were received.
    assert(expected[0].empty());
    assert(expected[1].empty());
}

std::unique_ptr<Transport> MakeV1Transport(NodeId nodeid) noexcept
{
    return std::make_unique<V1Transport>(nodeid, SER_NETWORK, INIT_PROTO_VERSION);
}

} // namespace

FUZZ_TARGET(p2p_transport_bidirectional, .init = initialize_p2p_transport_serialization)
{
    // Test with two V1 transports talking to each other.
    FuzzedDataProvider provider{buffer.data(), buffer.size()};
    XoRoShiRo128PlusPlus rng(provider.ConsumeIntegral<uint64_t>());
    auto t1 = MakeV1Transport(NodeId{0});
    auto t2 = MakeV1Transport(NodeId{1});
    if (!t1 || !t2) return;
    SimulationTest(*t1, *t2, rng, provider);
}
