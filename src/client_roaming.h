/**
 * @file src/client_roaming.h
 * @brief Host side of client roaming, wire format v1.
 *
 * When a client's address changes (Wi-Fi to another Wi-Fi, to mobile data, a NAT rebinding), the
 * stream used to freeze: the host kept sending to the old address. A client that knows this
 * extension proves its new address instead, QUIC-style, and the host moves the session only after
 * the proof:
 *
 *   1. PROBE      client -> host, from the new address.
 *   2. CHALLENGE  host -> client, to the PROBE's source: a random number only the key holder can read.
 *   3. CONFIRM    client -> host, from the same address, echoing the challenge.
 *   4. MIGRATED   host -> client: the session has moved.
 *
 * Negotiated and off by default: the host adds `a=x-rl-roaming:1` to DESCRIBE only when
 * `client_roaming` is enabled, and does nothing until an authenticated PROBE arrives. A client that
 * doesn't know the attribute never sends one. The datagrams travel on the control UDP port, outside
 * ENet, and are exactly 52 bytes with little-endian integers (the format is the one of
 * RamazanKara/nightjar `docs/protocol/client-roaming.md`, byte for byte):
 *
 *   0  4  magic 0F FE 52 4D       20 16  AES-128-GCM tag
 *   4  1  version (1)             36 16  ciphertext of the 16-byte payload
 *   5  1  type (1..4)
 *   6  2  reserved (0)            AAD = bytes 0..19, nonce = type, direction, 0, 0, seq LE, 0 0 0 0
 *   8  8  session tag
 *   16 4  seq
 *
 * Both keys come from the launch's `rikey` with one AES-128 block each.
 *
 * Nothing here touches sockets or sessions: the table decides, and the stream code sends the answer
 * and carries out a commit. The clock and the challenge source are injectable for tests.
 */
#pragma once

// standard includes
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace client_roaming {

  /// DESCRIBE attribute line (without the line end): the highest wire version the host speaks.
  inline constexpr std::string_view sdp_attribute = "a=x-rl-roaming:1";

  inline constexpr std::size_t datagram_bytes = 52;
  inline constexpr std::size_t payload_bytes = 16;
  inline constexpr std::size_t header_bytes = 20;
  inline constexpr std::size_t session_tag_bytes = 8;
  inline constexpr std::uint8_t wire_version = 1;
  inline constexpr std::array<std::uint8_t, 4> magic {0x0F, 0xFE, 0x52, 0x4D};

  /// PROBE flag bit 0: the client rebound its media sockets, so pings come from new ports.
  inline constexpr std::uint32_t probe_flag_media_rebound = 0x1;

  /// Datagrams (pending or current) per session and second that are handled after authentication.
  inline constexpr unsigned session_per_second = 10;
  /// Datagrams per session tag and second that are even opened, counted before authentication.
  inline constexpr unsigned preauth_per_second = 50;
  /// Datagrams with a known session tag per second over all sessions.
  inline constexpr unsigned global_per_second = 5000;
  /// How long a probing address may take to confirm.
  inline constexpr std::chrono::seconds pending_lifetime {3};
  /// The shortest time between two moves of one session.
  inline constexpr std::chrono::seconds commit_interval {1};
  /// Sessions whose seqs are remembered after they ended (least recently used first out).
  inline constexpr std::size_t seq_memory_entries = 32;

  using datagram_t = std::array<std::uint8_t, datagram_bytes>;
  using rikey_t = std::array<std::uint8_t, 16>;
  using tag_t = std::array<std::uint8_t, session_tag_bytes>;

  enum class kind_t : std::uint8_t {
    probe = 1,  ///< client -> host
    challenge = 2,  ///< host -> client
    confirm = 3,  ///< client -> host
    migrated = 4,  ///< host -> client
  };

  /// What a datagram says. `a` and `b` are the two payload halves (see `payload_t`).
  struct payload_t {
    kind_t kind;
    std::uint64_t a;  ///< PROBE: nonce_c. CHALLENGE: nonce_c. CONFIRM, MIGRATED: challenge.
    std::uint64_t b;  ///< PROBE: flags (u32). CHALLENGE: challenge. CONFIRM, MIGRATED: 0.
  };

  struct header_t {
    kind_t kind;
    tag_t session_tag;
    std::uint32_t seq;
  };

  struct keys_t {
    rikey_t key;  ///< AES-128-ECB(rikey, "RLROAM-KEY-v1\0\0\0"): seals and opens the datagrams.
    tag_t session_tag;  ///< First 8 bytes of AES-128-ECB(rikey, "RLROAM-TAG-v1\0\0\0"): finds the session.

    bool operator==(const keys_t &) const = default;
  };

  /// The keys one launch's `rikey` derives. Nothing only if OpenSSL fails.
  std::optional<keys_t> derive_keys(const rikey_t &rikey);

  /**
   * @brief Strictly decode the clear header.
   * @return Nothing for a datagram that isn't exactly 52 bytes, lacks the magic, has another
   *         version, an unknown type or nonzero reserved bytes, or seq 0.
   */
  std::optional<header_t> decode_header(const std::uint8_t *data, std::size_t size);

  /// Seals `payload` under `header` (whose kind must match) into wire form. The direction in the nonce
  /// follows the kind: PROBE and CONFIRM are client -> host (1), CHALLENGE and MIGRATED host -> client (2).
  std::optional<datagram_t> seal(const rikey_t &key, const header_t &header, const payload_t &payload);

  /// Opens a datagram whose header was decoded. Nothing when the GCM tag fails or the padding isn't zero.
  std::optional<payload_t> open(const rikey_t &key, const std::uint8_t *data, std::size_t size, const header_t &header);

  /// An address in canonical form: IPv4 as an IPv4-mapped IPv6 address, so the two spellings compare equal.
  struct endpoint_t {
    std::array<std::uint8_t, 16> ip {};
    std::uint16_t port = 0;

    bool operator==(const endpoint_t &) const = default;
    bool same_ip(const endpoint_t &other) const {
      return ip == other.ip;
    }
  };

  /// Why a datagram was dropped. A dropped datagram is never answered.
  enum class refusal_t {
    decode,  ///< The clear header is malformed.
    direction,  ///< CHALLENGE or MIGRATED, which only a host sends.
    unknown_session,  ///< No session has this tag.
    global_rate,  ///< Over `global_per_second`.
    preauth_rate,  ///< Over `preauth_per_second` for this tag.
    stopping,  ///< The session is stopping.
    authentication,  ///< The GCM tag doesn't verify or the padding isn't zero.
    replay,  ///< Seq not above the last one seen from the client.
    session_rate,  ///< Over `session_per_second`.
    no_pending,  ///< CONFIRM without a pending address, or after it expired.
    address,  ///< CONFIRM from another address than the pending one.
    challenge,  ///< CONFIRM with another challenge than the pending one.
    commit_rate,  ///< The session moved less than `commit_interval` ago.
    seq_exhausted,  ///< The host's seq would wrap; it never reuses a nonce.
  };

  std::string_view to_string(refusal_t refusal);

  /// What the caller does with a datagram.
  struct result_t {
    enum class type_t {
      refused,  ///< Dropped, `refusal` says why. Nothing is sent.
      reply,  ///< Send `datagram` to `to`. Nothing moves.
      commit,  ///< Move session `id` from `old_address` to `new_address`, then send `datagram` (MIGRATED) to `new_address`.
    } type = type_t::refused;
    refusal_t refusal = refusal_t::decode;
    endpoint_t to;
    datagram_t datagram {};
    std::uint64_t id = 0;
    endpoint_t old_address;
    endpoint_t new_address;
    bool media_rebound = false;  ///< The PROBE's flag: expect pings from new ports.
  };

  /**
   * @brief The roaming state of every session with an ENet peer, keyed by the caller's session id.
   * @details Used from one thread (the control thread); it has no lock.
   */
  class table_t {
  public:
    using clock_t = std::chrono::steady_clock;
    using challenge_source_t = std::function<std::uint64_t()>;

    /// Challenges come from the OS's random numbers.
    table_t();
    /// Challenges come from `challenges`, for tests.
    explicit table_t(challenge_source_t challenges);

    /**
     * @brief Accepts roaming for session `id`, whose ENet peer is at `current`.
     * @details Inserting an id again (an ENet reconnect under the same key) updates its address and keeps
     * both seqs, and the seqs of a session that ended are remembered by its tag, so no side ever
     * reuses a GCM nonce under a launch's key and old datagrams stay replays. A stopping session
     * stays stopping.
     */
    void insert(std::uint64_t id, const rikey_t &rikey, const endpoint_t &current);

    /// From now on the session refuses every datagram.
    void set_stopping(std::uint64_t id);

    void remove(std::uint64_t id);

    bool contains(std::uint64_t id) const;

    bool empty() const {
      return _entries.empty();
    }

    /// The address the session's client was last validated at.
    std::optional<endpoint_t> current(std::uint64_t id) const;

    /// Handles one datagram that starts with the roaming magic, received from `from`.
    result_t on_datagram(clock_t::time_point now, const endpoint_t &from, const std::uint8_t *data, std::size_t size);

  private:
    struct window_t {
      std::optional<clock_t::time_point> start;
      unsigned count = 0;

      bool admit(clock_t::time_point now, unsigned limit);
    };

    struct pending_t {
      endpoint_t address;
      std::uint64_t nonce_c;
      std::uint64_t challenge;
      bool media_rebound;
      clock_t::time_point expires;
    };

    struct here_t {
      std::uint64_t challenge;
      clock_t::time_point expires;
    };

    struct entry_t {
      std::uint64_t id;
      keys_t keys;
      endpoint_t current;
      bool stopping = false;
      std::uint32_t client_seq = 0;
      std::uint32_t host_seq = 0;
      std::optional<pending_t> pending;
      std::optional<here_t> here;
      std::optional<std::uint64_t> committed;
      std::optional<clock_t::time_point> last_commit;
      window_t preauth;
      window_t window;
    };

    struct memory_t {
      tag_t session_tag;
      std::uint32_t client_seq;
      std::uint32_t host_seq;
    };

    bool answer(entry_t &entry, const payload_t &payload, result_t &result);
    result_t on_probe(entry_t &entry, clock_t::time_point now, const endpoint_t &from, const payload_t &payload);
    result_t on_confirm(entry_t &entry, clock_t::time_point now, const endpoint_t &from, std::uint64_t challenge);
    void remember(const entry_t &entry);

    std::vector<entry_t> _entries;
    std::deque<memory_t> _memory;  ///< Most recently ended first.
    window_t _window;
    challenge_source_t _challenges;
  };

}  // namespace client_roaming
