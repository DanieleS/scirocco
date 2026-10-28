/**
 * @file src/client_roaming.cpp
 * @brief Host side of client roaming, wire format v1. See client_roaming.h.
 */
// standard includes
#include <algorithm>
#include <cstring>
#include <random>

// lib includes
#include <openssl/evp.h>
#include <openssl/rand.h>

// local includes
#include "client_roaming.h"

using namespace std::literals;

namespace client_roaming {

  namespace {
    constexpr std::size_t tag_offset = 20;
    constexpr std::size_t ciphertext_offset = 36;
    constexpr std::size_t gcm_tag_bytes = 16;
    constexpr std::uint8_t direction_client_to_host = 1;
    constexpr std::uint8_t direction_host_to_client = 2;

    struct cipher_ctx_t {
      EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();

      ~cipher_ctx_t() {
        EVP_CIPHER_CTX_free(ctx);
      }

      cipher_ctx_t(const cipher_ctx_t &) = delete;
      cipher_ctx_t &operator=(const cipher_ctx_t &) = delete;
      cipher_ctx_t() = default;
    };

    void put_le(std::uint8_t *out, std::uint64_t value, int bytes) {
      for (int i = 0; i < bytes; ++i) {
        out[i] = static_cast<std::uint8_t>(value >> (8 * i));
      }
    }

    std::uint64_t get_le(const std::uint8_t *in, int bytes) {
      std::uint64_t value = 0;
      for (int i = 0; i < bytes; ++i) {
        value |= static_cast<std::uint64_t>(in[i]) << (8 * i);
      }
      return value;
    }

    bool is_known_kind(std::uint8_t type) {
      return type >= static_cast<std::uint8_t>(kind_t::probe) && type <= static_cast<std::uint8_t>(kind_t::migrated);
    }

    std::uint8_t direction_of(kind_t kind) {
      return kind == kind_t::probe || kind == kind_t::confirm ? direction_client_to_host : direction_host_to_client;
    }

    /// nonce = type, direction, 0, 0, seq (LE), 0, 0, 0, 0
    std::array<std::uint8_t, 12> nonce_of(const header_t &header) {
      std::array<std::uint8_t, 12> nonce {};
      nonce[0] = static_cast<std::uint8_t>(header.kind);
      nonce[1] = direction_of(header.kind);
      put_le(nonce.data() + 4, header.seq, 4);
      return nonce;
    }

    /// The 20-byte clear header, which is also the AAD.
    std::array<std::uint8_t, header_bytes> encode_header(const header_t &header) {
      std::array<std::uint8_t, header_bytes> out {};
      std::copy(magic.begin(), magic.end(), out.begin());
      out[4] = wire_version;
      out[5] = static_cast<std::uint8_t>(header.kind);
      std::copy(header.session_tag.begin(), header.session_tag.end(), out.begin() + 8);
      put_le(out.data() + 16, header.seq, 4);
      return out;
    }

    std::array<std::uint8_t, payload_bytes> encode_payload(const payload_t &payload) {
      std::array<std::uint8_t, payload_bytes> out {};
      put_le(out.data(), payload.a, 8);
      switch (payload.kind) {
        case kind_t::probe:
          put_le(out.data() + 8, payload.b & 0xFFFFFFFFu, 4);
          break;
        case kind_t::challenge:
          put_le(out.data() + 8, payload.b, 8);
          break;
        case kind_t::confirm:
        case kind_t::migrated:
          break;
      }
      return out;
    }

    /// Nothing when the padding that has to be zero is not.
    std::optional<payload_t> decode_payload(kind_t kind, const std::array<std::uint8_t, payload_bytes> &in) {
      payload_t payload {kind, get_le(in.data(), 8), 0};
      const auto zero = [&](std::size_t from) {
        return std::all_of(in.begin() + from, in.end(), [](std::uint8_t byte) {
          return byte == 0;
        });
      };
      switch (kind) {
        case kind_t::probe:
          if (!zero(12)) {
            return std::nullopt;
          }
          payload.b = get_le(in.data() + 8, 4);
          break;
        case kind_t::challenge:
          payload.b = get_le(in.data() + 8, 8);
          break;
        case kind_t::confirm:
        case kind_t::migrated:
          if (!zero(8)) {
            return std::nullopt;
          }
          break;
      }
      return payload;
    }

    /// One AES-128 block, ECB, no padding.
    bool ecb_block(const rikey_t &key, const char (&input)[17], std::array<std::uint8_t, 16> &out) {
      cipher_ctx_t cipher;
      if (!cipher.ctx || EVP_EncryptInit_ex(cipher.ctx, EVP_aes_128_ecb(), nullptr, key.data(), nullptr) != 1) {
        return false;
      }
      EVP_CIPHER_CTX_set_padding(cipher.ctx, 0);
      int length = 0;
      if (EVP_EncryptUpdate(cipher.ctx, out.data(), &length, reinterpret_cast<const unsigned char *>(input), 16) != 1 || length != 16) {
        return false;
      }
      int final_length = 0;
      return EVP_EncryptFinal_ex(cipher.ctx, out.data() + length, &final_length) == 1 && final_length == 0;
    }

    std::uint64_t os_random() {
      std::uint64_t value = 0;
      if (RAND_bytes(reinterpret_cast<unsigned char *>(&value), sizeof(value)) != 1) {
        std::random_device device;
        value = (static_cast<std::uint64_t>(device()) << 32) ^ device();
      }
      return value;
    }
  }  // namespace

  std::optional<keys_t> derive_keys(const rikey_t &rikey) {
    // "RLROAM-KEY-v1" and "RLROAM-TAG-v1": 13 ASCII characters and three zero bytes.
    static constexpr char key_block[17] = {'R', 'L', 'R', 'O', 'A', 'M', '-', 'K', 'E', 'Y', '-', 'v', '1', 0, 0, 0, 0};
    static constexpr char tag_block[17] = {'R', 'L', 'R', 'O', 'A', 'M', '-', 'T', 'A', 'G', '-', 'v', '1', 0, 0, 0, 0};

    keys_t keys {};
    std::array<std::uint8_t, 16> tag_block_out {};
    if (!ecb_block(rikey, key_block, keys.key) || !ecb_block(rikey, tag_block, tag_block_out)) {
      return std::nullopt;
    }
    std::copy_n(tag_block_out.begin(), session_tag_bytes, keys.session_tag.begin());
    return keys;
  }

  std::optional<header_t> decode_header(const std::uint8_t *data, std::size_t size) {
    if (!data || size != datagram_bytes) {
      return std::nullopt;
    }
    if (!std::equal(magic.begin(), magic.end(), data) || data[4] != wire_version || !is_known_kind(data[5])) {
      return std::nullopt;
    }
    if (data[6] != 0 || data[7] != 0) {
      return std::nullopt;
    }
    header_t header {};
    header.kind = static_cast<kind_t>(data[5]);
    std::copy_n(data + 8, session_tag_bytes, header.session_tag.begin());
    header.seq = static_cast<std::uint32_t>(get_le(data + 16, 4));
    if (header.seq == 0) {
      return std::nullopt;
    }
    return header;
  }

  std::optional<datagram_t> seal(const rikey_t &key, const header_t &header, const payload_t &payload) {
    const auto aad = encode_header(header);
    const auto nonce = nonce_of(header);
    const auto plaintext = encode_payload(payload);

    cipher_ctx_t cipher;
    if (!cipher.ctx || EVP_EncryptInit_ex(cipher.ctx, EVP_aes_128_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(cipher.ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()), nullptr) != 1 ||
        EVP_EncryptInit_ex(cipher.ctx, nullptr, nullptr, key.data(), nonce.data()) != 1) {
      return std::nullopt;
    }

    datagram_t out {};
    std::copy(aad.begin(), aad.end(), out.begin());

    int length = 0;
    if (EVP_EncryptUpdate(cipher.ctx, nullptr, &length, aad.data(), static_cast<int>(aad.size())) != 1 ||
        EVP_EncryptUpdate(cipher.ctx, out.data() + ciphertext_offset, &length, plaintext.data(), static_cast<int>(plaintext.size())) != 1 ||
        length != static_cast<int>(payload_bytes)) {
      return std::nullopt;
    }
    int final_length = 0;
    if (EVP_EncryptFinal_ex(cipher.ctx, out.data() + ciphertext_offset + length, &final_length) != 1 ||
        EVP_CIPHER_CTX_ctrl(cipher.ctx, EVP_CTRL_GCM_GET_TAG, static_cast<int>(gcm_tag_bytes), out.data() + tag_offset) != 1) {
      return std::nullopt;
    }
    return out;
  }

  std::optional<payload_t> open(const rikey_t &key, const std::uint8_t *data, std::size_t size, const header_t &header) {
    if (!data || size != datagram_bytes) {
      return std::nullopt;
    }
    const auto nonce = nonce_of(header);

    cipher_ctx_t cipher;
    if (!cipher.ctx || EVP_DecryptInit_ex(cipher.ctx, EVP_aes_128_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(cipher.ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()), nullptr) != 1 ||
        EVP_DecryptInit_ex(cipher.ctx, nullptr, nullptr, key.data(), nonce.data()) != 1) {
      return std::nullopt;
    }

    std::array<std::uint8_t, gcm_tag_bytes> gcm_tag {};
    std::copy_n(data + tag_offset, gcm_tag.size(), gcm_tag.begin());

    std::array<std::uint8_t, payload_bytes> plaintext {};
    int length = 0;
    if (EVP_DecryptUpdate(cipher.ctx, nullptr, &length, data, static_cast<int>(header_bytes)) != 1 ||
        EVP_DecryptUpdate(cipher.ctx, plaintext.data(), &length, data + ciphertext_offset, static_cast<int>(payload_bytes)) != 1 ||
        length != static_cast<int>(payload_bytes) ||
        EVP_CIPHER_CTX_ctrl(cipher.ctx, EVP_CTRL_GCM_SET_TAG, static_cast<int>(gcm_tag.size()), gcm_tag.data()) != 1) {
      return std::nullopt;
    }
    int final_length = 0;
    if (EVP_DecryptFinal_ex(cipher.ctx, plaintext.data() + length, &final_length) <= 0) {
      return std::nullopt;
    }
    return decode_payload(header.kind, plaintext);
  }

  std::string_view to_string(refusal_t refusal) {
    switch (refusal) {
      case refusal_t::decode:
        return "malformed"sv;
      case refusal_t::direction:
        return "host-bound type"sv;
      case refusal_t::unknown_session:
        return "unknown session"sv;
      case refusal_t::global_rate:
        return "global rate limit"sv;
      case refusal_t::preauth_rate:
        return "per-session pre-authentication rate limit"sv;
      case refusal_t::stopping:
        return "session stopping"sv;
      case refusal_t::authentication:
        return "authentication failed"sv;
      case refusal_t::replay:
        return "replayed seq"sv;
      case refusal_t::session_rate:
        return "session rate limit"sv;
      case refusal_t::no_pending:
        return "no pending address"sv;
      case refusal_t::address:
        return "confirm from another address"sv;
      case refusal_t::challenge:
        return "wrong challenge"sv;
      case refusal_t::commit_rate:
        return "moved too recently"sv;
      case refusal_t::seq_exhausted:
        return "host seq exhausted"sv;
    }
    return "unknown"sv;
  }

  bool table_t::window_t::admit(clock_t::time_point now, unsigned limit) {
    if (!start || now < *start || now - *start >= std::chrono::seconds {1}) {
      start = now;
      count = 0;
    }
    if (count >= limit) {
      return false;
    }
    ++count;
    return true;
  }

  table_t::table_t():
      _challenges {os_random} {
  }

  table_t::table_t(challenge_source_t challenges):
      _challenges {std::move(challenges)} {
  }

  void table_t::remember(const entry_t &entry) {
    std::erase_if(_memory, [&](const memory_t &known) {
      return known.session_tag == entry.keys.session_tag;
    });
    _memory.push_front({entry.keys.session_tag, entry.client_seq, entry.host_seq});
    while (_memory.size() > seq_memory_entries) {
      _memory.pop_back();
    }
  }

  void table_t::insert(std::uint64_t id, const rikey_t &rikey, const endpoint_t &current) {
    const auto keys = derive_keys(rikey);
    if (!keys) {
      return;
    }
    for (auto &entry : _entries) {
      if (entry.id == id && entry.keys == *keys) {
        entry.current = current;
        entry.pending.reset();
        entry.here.reset();
        return;
      }
    }
    remove(id);

    entry_t entry {};
    entry.id = id;
    entry.keys = *keys;
    entry.current = current;
    if (const auto known = std::find_if(_memory.begin(), _memory.end(), [&](const memory_t &memory) {
          return memory.session_tag == keys->session_tag;
        });
        known != _memory.end()) {
      entry.client_seq = known->client_seq;
      entry.host_seq = known->host_seq;
      _memory.erase(known);
    }
    _entries.push_back(std::move(entry));
  }

  void table_t::set_stopping(std::uint64_t id) {
    for (auto &entry : _entries) {
      if (entry.id == id) {
        entry.stopping = true;
        entry.pending.reset();
        entry.here.reset();
      }
    }
  }

  void table_t::remove(std::uint64_t id) {
    for (auto it = _entries.begin(); it != _entries.end();) {
      if (it->id == id) {
        remember(*it);
        it = _entries.erase(it);
      } else {
        ++it;
      }
    }
  }

  bool table_t::contains(std::uint64_t id) const {
    return std::any_of(_entries.begin(), _entries.end(), [&](const entry_t &entry) {
      return entry.id == id;
    });
  }

  std::optional<endpoint_t> table_t::current(std::uint64_t id) const {
    for (const auto &entry : _entries) {
      if (entry.id == id) {
        return entry.current;
      }
    }
    return std::nullopt;
  }

  /// Seals the host's next answer. False (and nothing consumed) when the seq would wrap.
  bool table_t::answer(entry_t &entry, const payload_t &payload, result_t &result) {
    if (entry.host_seq == UINT32_MAX) {
      return false;
    }
    const header_t header {payload.kind, entry.keys.session_tag, entry.host_seq + 1};
    const auto sealed = seal(entry.keys.key, header, payload);
    if (!sealed) {
      return false;
    }
    entry.host_seq = header.seq;
    result.datagram = *sealed;
    return true;
  }

  result_t table_t::on_datagram(clock_t::time_point now, const endpoint_t &from, const std::uint8_t *data, std::size_t size) {
    const auto refuse = [](refusal_t why) {
      result_t result;
      result.type = result_t::type_t::refused;
      result.refusal = why;
      return result;
    };

    const auto header = decode_header(data, size);
    if (!header) {
      return refuse(refusal_t::decode);
    }
    if (header->kind != kind_t::probe && header->kind != kind_t::confirm) {
      return refuse(refusal_t::direction);
    }
    const auto found = std::find_if(_entries.begin(), _entries.end(), [&](const entry_t &entry) {
      return entry.keys.session_tag == header->session_tag;
    });
    if (found == _entries.end()) {
      return refuse(refusal_t::unknown_session);
    }
    auto &entry = *found;

    // These bound the GCM opens before anything is authenticated: one tag can't starve the others.
    if (!_window.admit(now, global_per_second)) {
      return refuse(refusal_t::global_rate);
    }
    if (!entry.preauth.admit(now, preauth_per_second)) {
      return refuse(refusal_t::preauth_rate);
    }
    if (entry.stopping) {
      return refuse(refusal_t::stopping);
    }
    const auto payload = open(entry.keys.key, data, size, *header);
    if (!payload) {
      return refuse(refusal_t::authentication);
    }
    if (header->seq <= entry.client_seq) {
      return refuse(refusal_t::replay);
    }
    // Recorded only once the datagram is authentic and fresh.
    entry.client_seq = header->seq;
    // Counted after that too, so a replay can't use up the client's share.
    if (!entry.window.admit(now, session_per_second)) {
      return refuse(refusal_t::session_rate);
    }

    auto result = payload->kind == kind_t::probe ? on_probe(entry, now, from, *payload) : on_confirm(entry, now, from, payload->a);
    result.id = entry.id;
    return result;
  }

  result_t table_t::on_probe(entry_t &entry, clock_t::time_point now, const endpoint_t &from, const payload_t &payload) {
    result_t result;
    result.to = from;
    const auto nonce_c = payload.a;
    const bool media = (payload.b & probe_flag_media_rebound) != 0;

    if (from == entry.current) {
      // A client that only suspected a move hears back, but its CONFIRM moves nothing.
      const auto challenge = _challenges();
      if (!answer(entry, {kind_t::challenge, nonce_c, challenge}, result)) {
        result.refusal = refusal_t::seq_exhausted;
        return result;
      }
      entry.here = here_t {challenge, now + pending_lifetime};
      result.type = result_t::type_t::reply;
      return result;
    }

    // The same PROBE again (a lost CHALLENGE) hears the same challenge, and doesn't extend the expiry.
    if (entry.pending && entry.pending->address == from && entry.pending->nonce_c == nonce_c && now < entry.pending->expires) {
      if (!answer(entry, {kind_t::challenge, nonce_c, entry.pending->challenge}, result)) {
        result.refusal = refusal_t::seq_exhausted;
        return result;
      }
      entry.pending->media_rebound = entry.pending->media_rebound || media;
      result.type = result_t::type_t::reply;
      return result;
    }

    // A newer candidate replaces the pending one.
    const auto challenge = _challenges();
    if (!answer(entry, {kind_t::challenge, nonce_c, challenge}, result)) {
      result.refusal = refusal_t::seq_exhausted;
      return result;
    }
    entry.pending = pending_t {from, nonce_c, challenge, media, now + pending_lifetime};
    result.type = result_t::type_t::reply;
    return result;
  }

  result_t table_t::on_confirm(entry_t &entry, clock_t::time_point now, const endpoint_t &from, std::uint64_t challenge) {
    result_t result;
    result.to = from;
    const auto refuse = [&](refusal_t why) {
      result.type = result_t::type_t::refused;
      result.refusal = why;
      return result;
    };
    const auto migrated = [&]() {
      if (!answer(entry, {kind_t::migrated, challenge, 0}, result)) {
        return refuse(refusal_t::seq_exhausted);
      }
      result.type = result_t::type_t::reply;
      return result;
    };

    if (from == entry.current) {
      // A CONFIRM repeated after the move (its MIGRATED was lost) hears MIGRATED again, and so does one
      // answering a challenge that a PROBE from the current address got. Nothing moves.
      if (entry.committed == challenge) {
        return migrated();
      }
      if (entry.here && now < entry.here->expires && entry.here->challenge == challenge) {
        auto answered = migrated();
        if (answered.type == result_t::type_t::reply) {
          entry.here.reset();
        }
        return answered;
      }
    }

    if (!entry.pending || now >= entry.pending->expires) {
      return refuse(refusal_t::no_pending);
    }
    const auto pending = *entry.pending;
    if (from != pending.address) {
      return refuse(refusal_t::address);
    }
    if (challenge != pending.challenge) {
      return refuse(refusal_t::challenge);
    }
    if (entry.last_commit && now - *entry.last_commit < commit_interval) {
      return refuse(refusal_t::commit_rate);
    }
    if (!answer(entry, {kind_t::migrated, challenge, 0}, result)) {
      return refuse(refusal_t::seq_exhausted);
    }

    result.type = result_t::type_t::commit;
    result.id = entry.id;
    result.old_address = entry.current;
    result.new_address = from;
    result.media_rebound = pending.media_rebound;

    entry.current = from;
    entry.pending.reset();
    entry.here.reset();
    entry.committed = challenge;
    entry.last_commit = now;
    return result;
  }

}  // namespace client_roaming
