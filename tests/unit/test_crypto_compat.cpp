#ifdef NDEBUG
  #undef NDEBUG
#endif

// Exercise the actual crypto.cpp implementation with injected failures at the
// new X509 name/signing boundaries. This fixture does not use a network host.
#include "../../src/crypto.h"
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

namespace {
  int fail_step = 0;
  X509_NAME *test_name_new() { return fail_step == 1 ? nullptr : X509_NAME_new(); }
  int test_add_entry(X509_NAME *name, const char *field, int type, const unsigned char *bytes, int len, int loc, int set) {
    return fail_step == 2 ? 0 : X509_NAME_add_entry_by_txt(name, field, type, bytes, len, loc, set);
  }
  int test_subject(X509 *cert, const X509_NAME *name) { return fail_step == 3 ? 0 : X509_set_subject_name(cert, name); }
  int test_issuer(X509 *cert, const X509_NAME *name) { return fail_step == 4 ? 0 : X509_set_issuer_name(cert, name); }
  int test_sign(X509 *cert, EVP_PKEY *key, const EVP_MD *md) { return fail_step == 5 ? 0 : X509_sign(cert, key, md); }
}
#define X509_NAME_new test_name_new
#define X509_NAME_add_entry_by_txt test_add_entry
#define X509_set_subject_name test_subject
#define X509_set_issuer_name test_issuer
#define X509_sign test_sign
#include "../../src/crypto.cpp"
#undef X509_NAME_new
#undef X509_NAME_add_entry_by_txt
#undef X509_set_subject_name
#undef X509_set_issuer_name
#undef X509_sign

int main() {
  auto creds = crypto::gen_creds("Sunshine Gamestream Host", 1024);
  assert(!creds.x509.empty() && !creds.pkey.empty());
  auto cert = crypto::x509(creds.x509);
  auto key = crypto::pkey(creds.pkey);
  assert(cert && key);
  auto *subject = X509_get_subject_name(cert.get());
  auto *issuer = X509_get_issuer_name(cert.get());
  assert(subject && issuer && X509_NAME_cmp(subject, issuer) == 0);
  const int cn_index = X509_NAME_get_index_by_NID(subject, NID_commonName, -1);
  assert(cn_index >= 0);
  const auto *entry = X509_NAME_get_entry(subject, cn_index);
  assert(entry);
  const auto *cn = X509_NAME_ENTRY_get_data(entry);
  assert(cn);
#if OPENSSL_VERSION_NUMBER >= 0x40100000L
  auto cn_length = ASN1_STRING_get_length(cn);
#else
  auto cn_length = static_cast<size_t>(ASN1_STRING_length(cn));
#endif
  constexpr std::string_view expected_cn = "Sunshine Gamestream Host";
  assert(cn_length == expected_cn.size());
  assert(std::memcmp(ASN1_STRING_get0_data(cn), expected_cn.data(), cn_length) == 0);
  assert(X509_check_private_key(cert.get(), key.get()) == 1);
  assert(X509_verify(cert.get(), key.get()) == 1);
  auto sig = crypto::signature(cert);
  const ASN1_BIT_STRING *raw = nullptr;
  X509_get0_signature(&raw, nullptr, cert.get());
  assert(raw && sig.data() == reinterpret_cast<const char *>(ASN1_STRING_get0_data(raw)));
#if OPENSSL_VERSION_NUMBER >= 0x40100000L
  assert(sig.size() == static_cast<size_t>(ASN1_STRING_get_length(raw)));
#else
  assert(sig.size() == static_cast<size_t>(ASN1_STRING_length(raw)));
#endif
  assert(std::memcmp(sig.data(), ASN1_STRING_get0_data(raw), sig.size()) == 0);

  // Alter one byte of the DER-encoded signature without changing its length.
  // The resulting certificate need not verify: it tests binary extraction.
  int len = i2d_X509(cert.get(), nullptr);
  assert(len > 0);
  std::vector<unsigned char> der(static_cast<size_t>(len));
  unsigned char *out = der.data();
  assert(i2d_X509(cert.get(), &out) == len);
  assert(der.size() >= sig.size());
  auto *encoded_signature = der.data() + der.size() - sig.size();
  assert(std::memcmp(encoded_signature, sig.data(), sig.size()) == 0);
  encoded_signature[0] = 0;
  const unsigned char *in = der.data();
  crypto::x509_t binary_cert {d2i_X509(nullptr, &in, len)};
  assert(binary_cert);
  auto binary_sig = crypto::signature(binary_cert);
  assert(binary_sig.size() == sig.size() && binary_sig[0] == '\0');
  assert(std::memcmp(binary_sig.data() + 1, sig.data() + 1, sig.size() - 1) == 0);

  // A paired client's own certificate is accepted; one signed by it (made with its key) is not.
  {
    auto paired = std::make_shared<crypto::named_cert_t>();
    paired->cert = creds.x509;
    crypto::cert_chain_t chain;
    chain.add(paired);

    crypto::p_named_cert_t matched;
    assert(chain.verify(cert.get(), matched) == nullptr);
    assert(matched == paired);

    crypto::x509_t derived {X509_new()};
    assert(derived);
    assert(X509_set_version(derived.get(), 2) == 1);
    assert(ASN1_INTEGER_set(X509_get_serialNumber(derived.get()), 2) == 1);
    assert(X509_gmtime_adj(X509_getm_notBefore(derived.get()), 0));
    assert(X509_gmtime_adj(X509_getm_notAfter(derived.get()), 3600));
    crypto::x509_name_t derived_name {X509_NAME_new()};
    assert(derived_name);
    constexpr unsigned char derived_cn[] = "Derived Client";
    assert(X509_NAME_add_entry_by_txt(derived_name.get(), "CN", MBSTRING_ASC, derived_cn, sizeof(derived_cn) - 1, -1, 0) == 1);
    assert(X509_set_subject_name(derived.get(), derived_name.get()) == 1);
    assert(X509_set_issuer_name(derived.get(), X509_get_subject_name(cert.get())) == 1);
    assert(X509_set_pubkey(derived.get(), key.get()) == 1);
    assert(X509_sign(derived.get(), key.get(), EVP_sha256()) > 0);

    crypto::p_named_cert_t derived_match;
    assert(chain.verify(derived.get(), derived_match) != nullptr);
    assert(!derived_match);

    // Concurrent handshakes verify against one chain: no shared X509_STORE_CTX.
    std::atomic<int> failures {0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t) {
      workers.emplace_back([&]() {
        for (int i = 0; i < 300; ++i) {
          crypto::p_named_cert_t m;
          if (chain.verify(cert.get(), m) != nullptr || m != paired) {
            ++failures;
          }
        }
      });
    }
    for (auto &w : workers) {
      w.join();
    }
    assert(failures == 0);
  }

  for (fail_step = 1; fail_step <= 5; ++fail_step) {
    auto failed = crypto::gen_creds("Sunshine Gamestream Host", 1024);
    assert(failed.x509.empty() && failed.pkey.empty());
  }
  std::cout << "OpenSSL certificate compatibility fixture passed\n";
}
