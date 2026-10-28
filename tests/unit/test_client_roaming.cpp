/**
 * @file tests/unit/test_client_roaming.cpp
 * @brief Test src/client_roaming.*.
 */
#include "../tests_common.h"
#include "src/client_roaming.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using namespace client_roaming;

namespace {
  using clock = table_t::clock_t;

  constexpr rikey_t rikey {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
  constexpr std::uint64_t nonce_c = 0x0123456789abcdefull;
  constexpr std::uint64_t known_challenge = 0xfedcba9876543210ull;

  template<std::size_t N>
  std::array<std::uint8_t, N> from_hex(const std::string &text) {
    std::array<std::uint8_t, N> out {};
    if (text.size() != N * 2) {
      throw std::logic_error("bad known-answer hex");
    }
    for (std::size_t i = 0; i < N; ++i) {
      out[i] = static_cast<std::uint8_t>(std::stoi(text.substr(i * 2, 2), nullptr, 16));
    }
    return out;
  }

  const auto known_key = from_hex<16>("6d553d3fa2e47b32ebb1609b11c82df2");
  const auto known_tag = from_hex<8>("52d89ae18eeb0232");
  const auto probe_vector = from_hex<datagram_bytes>("0ffe524d0101000052d89ae18eeb023201000000275f7b4c17bd8fa9b0467587697c1a022c64e82c54fdbd5b9645a4373c755ab8");
  const auto challenge_vector = from_hex<datagram_bytes>("0ffe524d0102000052d89ae18eeb0232010000002954b77944af32c4e11d538346b6a81ec5035975a9ad5739a4f02740e81f59e9");
  const auto confirm_vector = from_hex<datagram_bytes>("0ffe524d0103000052d89ae18eeb02320200000049a23e8d3d0ab0db81e9b7c106334d238d38b827e46b1abf7a143a1063a29d9c");
  const auto migrated_vector = from_hex<datagram_bytes>("0ffe524d0104000052d89ae18eeb0232020000007e1ab6560eb57e87dd9d21d46c85fb7430202c7441ef5e9e4cbe141ac2ee8d80");

  endpoint_t at(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d, std::uint16_t port) {
    endpoint_t endpoint;
    endpoint.ip[10] = 0xff;
    endpoint.ip[11] = 0xff;
    endpoint.ip[12] = a;
    endpoint.ip[13] = b;
    endpoint.ip[14] = c;
    endpoint.ip[15] = d;
    endpoint.port = port;
    return endpoint;
  }

  const auto old_address = at(192, 168, 1, 20, 50000);
  const auto new_address = at(10, 0, 0, 7, 41000);
  const auto third_address = at(203, 0, 113, 9, 41000);

  // The table asks for the known challenge unless a test says otherwise.
  table_t make_table() {
    table_t table {[] {
      return known_challenge;
    }};
    table.insert(1, rikey, old_address);
    return table;
  }

  datagram_t client(std::uint32_t seq, const payload_t &payload, const rikey_t &for_rikey = rikey) {
    const auto keys = *derive_keys(for_rikey);
    return *seal(keys.key, {payload.kind, keys.session_tag, seq}, payload);
  }

  datagram_t probe(std::uint32_t seq, std::uint32_t flags = 0, std::uint64_t nonce = nonce_c) {
    return client(seq, {kind_t::probe, nonce, flags});
  }

  datagram_t confirm(std::uint32_t seq, std::uint64_t challenge = known_challenge) {
    return client(seq, {kind_t::confirm, challenge, 0});
  }

  result_t feed(table_t &table, clock::time_point now, const endpoint_t &from, const datagram_t &datagram) {
    return table.on_datagram(now, from, datagram.data(), datagram.size());
  }

  payload_t host_payload(const datagram_t &datagram) {
    const auto header = decode_header(datagram.data(), datagram.size());
    EXPECT_TRUE(header.has_value());
    const auto payload = open(known_key, datagram.data(), datagram.size(), *header);
    EXPECT_TRUE(payload.has_value());
    return payload.value_or(payload_t {});
  }

  std::uint32_t host_seq(const datagram_t &datagram) {
    return decode_header(datagram.data(), datagram.size())->seq;
  }
}  // namespace

TEST(ClientRoamingWire, KeysReproduceTheKnownAnswers) {
  const auto keys = derive_keys(rikey);
  ASSERT_TRUE(keys.has_value());
  EXPECT_EQ(keys->key, known_key);
  EXPECT_EQ(keys->session_tag, known_tag);
}

TEST(ClientRoamingWire, DatagramsReproduceTheKnownAnswers) {
  const struct {
    header_t header;
    payload_t payload;
    datagram_t expected;
  } cases[] = {
    {{kind_t::probe, known_tag, 1}, {kind_t::probe, nonce_c, probe_flag_media_rebound}, probe_vector},
    {{kind_t::challenge, known_tag, 1}, {kind_t::challenge, nonce_c, known_challenge}, challenge_vector},
    {{kind_t::confirm, known_tag, 2}, {kind_t::confirm, known_challenge, 0}, confirm_vector},
    {{kind_t::migrated, known_tag, 2}, {kind_t::migrated, known_challenge, 0}, migrated_vector},
  };
  for (const auto &item : cases) {
    const auto sealed = seal(known_key, item.header, item.payload);
    ASSERT_TRUE(sealed.has_value());
    EXPECT_EQ(*sealed, item.expected) << static_cast<int>(item.payload.kind);

    const auto header = decode_header(item.expected.data(), item.expected.size());
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(header->kind, item.header.kind);
    EXPECT_EQ(header->seq, item.header.seq);
    const auto opened = open(known_key, item.expected.data(), item.expected.size(), *header);
    ASSERT_TRUE(opened.has_value());
    EXPECT_EQ(opened->kind, item.payload.kind);
    EXPECT_EQ(opened->a, item.payload.a);
    EXPECT_EQ(opened->b, item.payload.b);
  }
}

TEST(ClientRoamingWire, MagicAndSdpLineAreTheContract) {
  EXPECT_EQ(magic, (std::array<std::uint8_t, 4> {0x0F, 0xFE, 0x52, 0x4D}));
  EXPECT_EQ(sdp_attribute, "a=x-rl-roaming:1");
  EXPECT_EQ(datagram_bytes, 52u);
}

TEST(ClientRoamingWire, StrictDecodeRefusesAMalformedHeader) {
  const auto decode = [](const datagram_t &data, std::size_t size = datagram_bytes) {
    return decode_header(data.data(), size);
  };
  EXPECT_TRUE(decode(probe_vector).has_value());

  EXPECT_FALSE(decode(probe_vector, datagram_bytes - 1));
  EXPECT_FALSE(decode_header(nullptr, datagram_bytes));
  const auto longer = [&] {
    std::array<std::uint8_t, datagram_bytes + 1> data {};
    std::copy(probe_vector.begin(), probe_vector.end(), data.begin());
    return decode_header(data.data(), data.size());
  };
  EXPECT_FALSE(longer());

  auto altered = probe_vector;
  altered[0] ^= 1;
  EXPECT_FALSE(decode(altered)) << "magic";
  altered = probe_vector;
  altered[4] = 2;
  EXPECT_FALSE(decode(altered)) << "version";
  altered = probe_vector;
  altered[5] = 0;
  EXPECT_FALSE(decode(altered)) << "type 0";
  altered[5] = 5;
  EXPECT_FALSE(decode(altered)) << "type 5";
  altered = probe_vector;
  altered[6] = 1;
  EXPECT_FALSE(decode(altered)) << "reserved";
  altered = probe_vector;
  altered[7] = 1;
  EXPECT_FALSE(decode(altered)) << "reserved";
  altered = probe_vector;
  altered[16] = altered[17] = altered[18] = altered[19] = 0;
  EXPECT_FALSE(decode(altered)) << "seq 0";
}

TEST(ClientRoamingWire, ProbeFlagsAreAU32AndTheRestIsPadding) {
  const auto keys = *derive_keys(rikey);
  // The flags are 32 bits wide; whatever is above is not encoded, so a sealed PROBE always has clean padding.
  const auto sealed = *seal(keys.key, {kind_t::probe, keys.session_tag, 1}, {kind_t::probe, nonce_c, 0x1ull | (0x1ull << 32)});
  const auto header = *decode_header(sealed.data(), sealed.size());
  const auto opened = open(keys.key, sealed.data(), sealed.size(), header);
  ASSERT_TRUE(opened.has_value());
  EXPECT_EQ(opened->b, 1u);
}

TEST(ClientRoamingTable, TheKnownExchangeMovesTheSession) {
  auto table = make_table();
  const auto now = clock::now();

  const auto challenge = feed(table, now, new_address, probe_vector);
  ASSERT_EQ(challenge.type, result_t::type_t::reply);
  EXPECT_EQ(challenge.to, new_address);
  EXPECT_EQ(challenge.datagram, challenge_vector);
  EXPECT_EQ(table.current(1), old_address) << "a probe moves nothing";

  const auto moved = feed(table, now, new_address, confirm_vector);
  ASSERT_EQ(moved.type, result_t::type_t::commit);
  EXPECT_EQ(moved.id, 1u);
  EXPECT_EQ(moved.old_address, old_address);
  EXPECT_EQ(moved.new_address, new_address);
  EXPECT_TRUE(moved.media_rebound);
  EXPECT_EQ(moved.datagram, migrated_vector);
  EXPECT_EQ(table.current(1), new_address);
}

TEST(ClientRoamingTable, AnyChangedBitIsDropped) {
  const auto now = clock::now();
  for (std::size_t bit = 0; bit < datagram_bytes * 8; ++bit) {
    auto altered = probe_vector;
    altered[bit / 8] ^= static_cast<std::uint8_t>(1 << (bit % 8));
    auto table = make_table();
    EXPECT_EQ(feed(table, now, new_address, altered).type, result_t::type_t::refused) << "bit " << bit;
  }

  auto table = make_table();
  auto forged = probe_vector;
  forged[20] ^= 0x80;
  EXPECT_EQ(feed(table, now, new_address, forged).refusal, refusal_t::authentication);
  EXPECT_EQ(feed(table, now, new_address, probe_vector).type, result_t::type_t::reply) << "a forgery doesn't consume the seq";

  // Sealed under another launch's key but carrying this session's tag.
  const rikey_t other_key {7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7};
  const auto other = *derive_keys(other_key);
  const auto foreign = *seal(other.key, {kind_t::probe, known_tag, 9}, {kind_t::probe, 1, 0});
  EXPECT_EQ(feed(table, now, new_address, foreign).refusal, refusal_t::authentication);
}

TEST(ClientRoamingTable, HostBoundTypesAndUnknownSessionsAreDropped) {
  auto table = make_table();
  const auto now = clock::now();
  EXPECT_EQ(feed(table, now, new_address, challenge_vector).refusal, refusal_t::direction);
  EXPECT_EQ(feed(table, now, new_address, migrated_vector).refusal, refusal_t::direction);

  table_t empty;
  EXPECT_EQ(feed(empty, now, new_address, probe_vector).refusal, refusal_t::unknown_session);
  EXPECT_EQ(table.on_datagram(now, new_address, probe_vector.data(), 40).refusal, refusal_t::decode);
}

TEST(ClientRoamingTable, AReplayedSeqIsDropped) {
  auto table = make_table();
  const auto now = clock::now();
  EXPECT_EQ(feed(table, now, new_address, probe(5)).type, result_t::type_t::reply);
  EXPECT_EQ(feed(table, now, new_address, probe(5)).refusal, refusal_t::replay);
  EXPECT_EQ(feed(table, now, third_address, probe(4)).refusal, refusal_t::replay) << "an older probe replayed from elsewhere";
  EXPECT_EQ(feed(table, now, new_address, confirm(5)).refusal, refusal_t::replay) << "seq is shared by PROBE and CONFIRM";
  EXPECT_EQ(feed(table, now, new_address, confirm(6)).type, result_t::type_t::commit);
  EXPECT_EQ(feed(table, now, new_address, confirm(6)).refusal, refusal_t::replay);
  EXPECT_EQ(table.current(1), new_address);
}

TEST(ClientRoamingTable, AWrongChallengeOrAddressMovesNothing) {
  auto table = make_table();
  const auto now = clock::now();
  EXPECT_EQ(feed(table, now, new_address, confirm(1)).refusal, refusal_t::no_pending);
  EXPECT_EQ(feed(table, now, new_address, probe(2)).type, result_t::type_t::reply);
  EXPECT_EQ(feed(table, now, new_address, confirm(3, known_challenge ^ 1)).refusal, refusal_t::challenge);
  EXPECT_EQ(feed(table, now, third_address, confirm(4)).refusal, refusal_t::address);
  auto other_port = new_address;
  other_port.port = 41001;
  EXPECT_EQ(feed(table, now, other_port, confirm(5)).refusal, refusal_t::address) << "same IP, another port is another address";
  EXPECT_EQ(table.current(1), old_address);
  EXPECT_EQ(feed(table, now, new_address, confirm(6)).type, result_t::type_t::commit);
}

TEST(ClientRoamingTable, ANewerProbeReplacesThePendingAddressAndPendingExpires) {
  std::uint64_t next = 0;
  table_t table {[&next] {
    return ++next;
  }};
  table.insert(1, rikey, old_address);
  const auto now = clock::now();

  EXPECT_EQ(feed(table, now, third_address, probe(1, 0, 1)).type, result_t::type_t::reply);
  EXPECT_EQ(feed(table, now, new_address, probe(2, 0, 2)).type, result_t::type_t::reply);
  EXPECT_EQ(feed(table, now, third_address, confirm(3, 1)).refusal, refusal_t::address);
  EXPECT_EQ(feed(table, now, new_address, confirm(4, 1)).refusal, refusal_t::challenge) << "the replaced probe's challenge";

  EXPECT_EQ(feed(table, now, new_address, probe(5, 0, 3)).type, result_t::type_t::reply);
  EXPECT_EQ(feed(table, now + pending_lifetime, new_address, confirm(6, 3)).refusal, refusal_t::no_pending);
  EXPECT_EQ(table.current(1), old_address);
}

TEST(ClientRoamingTable, ARepeatedProbeHearsTheSameChallengeWithoutExtendingTheExpiry) {
  std::uint64_t next = 100;
  table_t table {[&next] {
    return ++next;
  }};
  table.insert(1, rikey, old_address);
  const auto now = clock::now();

  const auto first = feed(table, now, new_address, probe(1));
  ASSERT_EQ(first.type, result_t::type_t::reply);
  const auto again = feed(table, now + 2s, new_address, probe(2, probe_flag_media_rebound));
  ASSERT_EQ(again.type, result_t::type_t::reply);

  const auto payload_of = [](const result_t &result) {
    const auto keys = *derive_keys(rikey);
    const auto header = *decode_header(result.datagram.data(), result.datagram.size());
    return *open(keys.key, result.datagram.data(), result.datagram.size(), header);
  };
  EXPECT_EQ(payload_of(first).b, payload_of(again).b);
  EXPECT_EQ(payload_of(first).a, nonce_c);
  EXPECT_EQ(host_seq(again.datagram), host_seq(first.datagram) + 1) << "the host's own seq goes on";

  // The expiry still counts from the first probe: 3 s after it nothing is pending, even though the repeat was at 2 s.
  EXPECT_EQ(feed(table, now + pending_lifetime, new_address, confirm(3, payload_of(first).b)).refusal, refusal_t::no_pending);
}

TEST(ClientRoamingTable, TheMediaFlagOfARepeatedProbeIsKept) {
  auto table = make_table();
  const auto now = clock::now();
  EXPECT_EQ(feed(table, now, new_address, probe(1)).type, result_t::type_t::reply);
  EXPECT_EQ(feed(table, now, new_address, probe(2, probe_flag_media_rebound)).type, result_t::type_t::reply);
  const auto moved = feed(table, now, new_address, confirm(3));
  ASSERT_EQ(moved.type, result_t::type_t::commit);
  EXPECT_TRUE(moved.media_rebound);
}

TEST(ClientRoamingTable, AStoppingSessionRefusesEverything) {
  auto table = make_table();
  const auto now = clock::now();
  EXPECT_EQ(feed(table, now, new_address, probe(1)).type, result_t::type_t::reply);
  table.set_stopping(1);
  EXPECT_EQ(feed(table, now, new_address, confirm(2)).refusal, refusal_t::stopping);
  EXPECT_EQ(feed(table, now, new_address, probe(3)).refusal, refusal_t::stopping);
  EXPECT_EQ(table.current(1), old_address);

  table.insert(1, rikey, old_address);
  EXPECT_EQ(feed(table, now, new_address, probe(4)).refusal, refusal_t::stopping) << "a late reconnect doesn't revive it";

  table.remove(1);
  EXPECT_FALSE(table.contains(1));
  EXPECT_EQ(feed(table, now, new_address, probe(5)).refusal, refusal_t::unknown_session);
}

TEST(ClientRoamingTable, RequestsAreRateLimitedPerSession) {
  auto table = make_table();
  const auto start = clock::now();
  for (unsigned seq = 1; seq <= session_per_second; ++seq) {
    EXPECT_EQ(feed(table, start, new_address, probe(seq)).type, result_t::type_t::reply) << seq;
  }
  EXPECT_EQ(feed(table, start, new_address, probe(session_per_second + 1)).refusal, refusal_t::session_rate);
  EXPECT_EQ(feed(table, start + 1s, new_address, probe(session_per_second + 2)).type, result_t::type_t::reply);
}

TEST(ClientRoamingTable, DatagramsAreCountedPerTagBeforeAuthentication) {
  auto table = make_table();
  const auto now = clock::now();
  auto forged = probe_vector;
  forged[40] ^= 1;
  for (unsigned i = 0; i < preauth_per_second; ++i) {
    EXPECT_EQ(feed(table, now, third_address, forged).refusal, refusal_t::authentication) << i;
  }
  EXPECT_EQ(feed(table, now, third_address, forged).refusal, refusal_t::preauth_rate);
  EXPECT_EQ(feed(table, now, new_address, probe_vector).refusal, refusal_t::preauth_rate) << "the real client shares the tag's window";
  EXPECT_EQ(feed(table, now + 1s, new_address, probe_vector).type, result_t::type_t::reply);
}

TEST(ClientRoamingTable, SessionsEachKeepTheirOwnPreauthShareButMeetTheGlobalCap) {
  table_t table {[] {
    return known_challenge;
  }};
  const auto now = clock::now();
  const unsigned sessions = global_per_second / preauth_per_second + 1;
  std::vector<rikey_t> keys;
  for (unsigned n = 0; n < sessions; ++n) {
    rikey_t key {};
    key.fill(static_cast<std::uint8_t>(n + 1));
    keys.push_back(key);
    table.insert(n, key, old_address);
  }

  // Every session sends its whole pre-authentication share (the datagrams repeat seq 1, so all but the
  // first are replays, which still cost an open); only the global cap refuses any of them.
  unsigned handled = 0;
  unsigned capped = 0;
  for (unsigned round = 0; round < preauth_per_second; ++round) {
    for (const auto &key : keys) {
      const auto result = feed(table, now, new_address, client(1, {kind_t::probe, 1, 0}, key));
      if (result.type == result_t::type_t::refused && result.refusal == refusal_t::global_rate) {
        ++capped;
      } else {
        ++handled;
      }
    }
  }
  EXPECT_EQ(handled, global_per_second);
  EXPECT_EQ(capped, sessions * preauth_per_second - global_per_second);
}

TEST(ClientRoamingTable, ASessionMovesAtMostOnceASecond) {
  auto table = make_table();
  const auto now = clock::now();
  EXPECT_EQ(feed(table, now, new_address, probe(1)).type, result_t::type_t::reply);
  EXPECT_EQ(feed(table, now, new_address, confirm(2)).type, result_t::type_t::commit);

  EXPECT_EQ(feed(table, now, third_address, probe(3)).type, result_t::type_t::reply);
  EXPECT_EQ(feed(table, now, third_address, confirm(4)).refusal, refusal_t::commit_rate);

  const auto later = now + commit_interval;
  const auto moved = feed(table, later, third_address, confirm(5));
  ASSERT_EQ(moved.type, result_t::type_t::commit);
  EXPECT_EQ(moved.old_address, new_address);
  EXPECT_EQ(moved.new_address, third_address);
}

TEST(ClientRoamingTable, ARepeatedConfirmHearsMigratedAgainWithoutMoving) {
  auto table = make_table();
  const auto now = clock::now();
  EXPECT_EQ(feed(table, now, new_address, probe(1)).type, result_t::type_t::reply);
  ASSERT_EQ(feed(table, now, new_address, confirm(2)).type, result_t::type_t::commit);

  const auto again = feed(table, now, new_address, confirm(3));
  ASSERT_EQ(again.type, result_t::type_t::reply);
  EXPECT_EQ(again.to, new_address);
  EXPECT_EQ(host_payload(again.datagram).kind, kind_t::migrated);
  EXPECT_EQ(host_payload(again.datagram).a, known_challenge);
  EXPECT_EQ(host_seq(again.datagram), 3u) << "the host's own seq goes on";

  // From the old address it is a confirm without a probe.
  EXPECT_EQ(feed(table, now, old_address, confirm(4)).refusal, refusal_t::no_pending);
}

TEST(ClientRoamingTable, AProbeFromTheCurrentAddressIsAnsweredButMovesNothing) {
  auto table = make_table();
  const auto now = clock::now();
  const auto answer = feed(table, now, old_address, probe(1));
  ASSERT_EQ(answer.type, result_t::type_t::reply);
  EXPECT_EQ(answer.to, old_address);
  EXPECT_EQ(host_payload(answer.datagram).kind, kind_t::challenge);
  EXPECT_EQ(host_payload(answer.datagram).a, nonce_c);
  EXPECT_EQ(host_payload(answer.datagram).b, known_challenge);

  const auto migrated = feed(table, now, old_address, confirm(2));
  ASSERT_EQ(migrated.type, result_t::type_t::reply) << "its confirm hears MIGRATED and commits nothing";
  EXPECT_EQ(host_payload(migrated.datagram).kind, kind_t::migrated);
  EXPECT_EQ(table.current(1), old_address);

  // Nothing moved, so a real move straight after isn't held back a second.
  EXPECT_EQ(feed(table, now, new_address, probe(3)).type, result_t::type_t::reply);
  EXPECT_EQ(feed(table, now, new_address, confirm(4)).type, result_t::type_t::commit);
}

TEST(ClientRoamingTable, NoAnswerIsLargerThanItsRequest) {
  auto table = make_table();
  const auto now = clock::now();
  unsigned answered = 0;
  const std::pair<endpoint_t, datagram_t> requests[] = {
    {new_address, probe(1)},
    {new_address, confirm(2)},
    {new_address, confirm(3)},
    {old_address, probe(4)},
    {third_address, probe(5)},
  };
  for (const auto &[from, request] : requests) {
    const auto result = feed(table, now, from, request);
    if (result.type != result_t::type_t::refused) {
      ++answered;
      EXPECT_EQ(result.datagram.size(), request.size());
    }
  }
  EXPECT_EQ(answered, 5u);

  // Anything that isn't exactly 52 bytes is never answered.
  std::array<std::uint8_t, 152> longer {};
  const auto padded = probe(6);
  std::copy(padded.begin(), padded.end(), longer.begin());
  EXPECT_EQ(table.on_datagram(now, third_address, longer.data(), longer.size()).type, result_t::type_t::refused);
  EXPECT_EQ(table.on_datagram(now, third_address, magic.data(), magic.size()).type, result_t::type_t::refused);
  EXPECT_EQ(table.on_datagram(now, third_address, nullptr, 0).type, result_t::type_t::refused);
}

TEST(ClientRoamingTable, AReconnectKeepsBothSeqsSoNoNonceRepeats) {
  auto table = make_table();
  const auto now = clock::now();
  const auto first = feed(table, now, new_address, probe_vector);
  ASSERT_EQ(first.type, result_t::type_t::reply);
  EXPECT_EQ(first.datagram, challenge_vector);

  // The ENet peer reconnects from another address under the same launch.
  table.insert(1, rikey, third_address);
  EXPECT_EQ(table.current(1), third_address);
  EXPECT_EQ(feed(table, now, new_address, probe_vector).refusal, refusal_t::replay);
  EXPECT_EQ(feed(table, now, new_address, confirm_vector).refusal, refusal_t::no_pending) << "a reconnect drops the pending address";
  const auto fresh = feed(table, now, new_address, probe(3));
  ASSERT_EQ(fresh.type, result_t::type_t::reply);
  EXPECT_EQ(host_seq(fresh.datagram), 2u);

  // A new launch under the same id starts over, and the old launch's key is gone.
  const rikey_t other_key {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
  table.insert(1, other_key, old_address);
  EXPECT_EQ(feed(table, now, new_address, client(1, {kind_t::probe, 1, 0}, other_key)).type, result_t::type_t::reply);
  EXPECT_EQ(feed(table, now, new_address, probe(4)).refusal, refusal_t::unknown_session);
}

TEST(ClientRoamingTable, SeqsAreRememberedAcrossARemovedSession) {
  auto table = make_table();
  const auto now = clock::now();
  EXPECT_EQ(feed(table, now, new_address, probe(1)).type, result_t::type_t::reply);
  EXPECT_EQ(feed(table, now, new_address, probe(2)).type, result_t::type_t::reply);
  table.remove(1);

  // The same launch key comes back (another session object): nothing is reused.
  table.insert(2, rikey, old_address);
  EXPECT_EQ(feed(table, now + 1s, new_address, probe(2)).refusal, refusal_t::replay);
  const auto fresh = feed(table, now + 1s, new_address, probe(3));
  ASSERT_EQ(fresh.type, result_t::type_t::reply);
  EXPECT_EQ(host_seq(fresh.datagram), 3u);
}

TEST(ClientRoamingTable, TheSeqMemoryIsBounded) {
  table_t table {[] {
    return known_challenge;
  }};
  const auto now = clock::now();
  for (unsigned n = 0; n < seq_memory_entries + 4; ++n) {
    rikey_t key {};
    key.fill(static_cast<std::uint8_t>(n + 1));
    table.insert(n, key, old_address);
    EXPECT_EQ(feed(table, now, new_address, client(1, {kind_t::probe, 1, 0}, key)).type, result_t::type_t::reply);
    table.remove(n);
  }

  // The oldest key was forgotten, so its seq 1 is taken again; a recent key is still a replay.
  rikey_t oldest {};
  oldest.fill(1);
  table.insert(100, oldest, old_address);
  EXPECT_EQ(feed(table, now + 1s, new_address, client(1, {kind_t::probe, 1, 0}, oldest)).type, result_t::type_t::reply);

  rikey_t recent {};
  recent.fill(static_cast<std::uint8_t>(seq_memory_entries + 4));
  table.insert(101, recent, old_address);
  EXPECT_EQ(feed(table, now + 1s, new_address, client(1, {kind_t::probe, 1, 0}, recent)).refusal, refusal_t::replay);
}
