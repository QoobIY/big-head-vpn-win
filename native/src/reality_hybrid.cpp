#include "reality_hybrid.h"

#include <mbedtls/platform_util.h>
#include <mbedtls/ssl.h>
extern "C" {
#include "kem.h"
}
#include <algorithm>
#include <vector>

namespace {
constexpr unsigned hybridGroup = 0x11ec;
constexpr unsigned x25519Group = 0x001d;
unsigned read16(const unsigned char* p) { return (unsigned(p[0]) << 8) | p[1]; }
void append16(std::vector<unsigned char>& out, size_t value) {
    out.push_back(static_cast<unsigned char>(value >> 8));
    out.push_back(static_cast<unsigned char>(value));
}
}

RealityHybridExchange::~RealityHybridExchange() {
    mbedtls_platform_zeroize(secretKey_.data(), secretKey_.size());
}

int RealityHybridExchange::expandClientHello(unsigned char* body, size_t* length, size_t capacity) {
    offered_ = false;
    negotiated_ = false;
    if (!body || !length || *length > capacity || *length < 35) return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    const size_t size = *length;
    size_t pos = 35 + body[34];
    if (pos + 2 > size) return MBEDTLS_ERR_SSL_DECODE_ERROR;
    pos += 2 + read16(body + pos); // cipher suites
    if (pos >= size) return MBEDTLS_ERR_SSL_DECODE_ERROR;
    pos += 1 + body[pos]; // compression methods
    if (pos + 2 > size || read16(body + pos) != size - pos - 2) return MBEDTLS_ERR_SSL_DECODE_ERROR;
    const size_t extensionsOffset = pos;
    pos += 2;
    std::vector<unsigned char> extensions;
    bool groupsFound = false, shareFound = false;
    std::array<unsigned char, 64> coins{};
    if (psa_generate_random(coins.data(), coins.size()) != PSA_SUCCESS) {
        mbedtls_platform_zeroize(coins.data(), coins.size());
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    int result = PQCLEAN_MLKEM768_CLEAN_crypto_kem_keypair_derand(publicKey_.data(), secretKey_.data(), coins.data());
    mbedtls_platform_zeroize(coins.data(), coins.size());
    if (result) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    while (pos < size) {
        if (size - pos < 4) return MBEDTLS_ERR_SSL_DECODE_ERROR;
        const unsigned type = read16(body + pos), len = read16(body + pos + 2);
        pos += 4;
        if (len > size - pos) return MBEDTLS_ERR_SSL_DECODE_ERROR;
        const auto* data = body + pos;
        if (type == 10) { // supported_groups
            if (groupsFound || len < 4 || read16(data) != len - 2 || (len % 2)) return MBEDTLS_ERR_SSL_DECODE_ERROR;
            append16(extensions, type); append16(extensions, len + 2);
            append16(extensions, len); append16(extensions, hybridGroup);
            extensions.insert(extensions.end(), data + 2, data + len);
            groupsFound = true;
        } else if (type == 51) { // key_share; Mbed TLS is configured with X25519 only.
            if (shareFound || len != 38 || read16(data) != 36 ||
                read16(data + 2) != x25519Group || read16(data + 4) != 32)
                return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
            append16(extensions, type); append16(extensions, len + 1220);
            append16(extensions, len - 2 + 1220);
            append16(extensions, hybridGroup); append16(extensions, 1216);
            extensions.insert(extensions.end(), publicKey_.begin(), publicKey_.end());
            extensions.insert(extensions.end(), data + 6, data + 38);
            // Offer classical X25519 second, reusing its ephemeral share.
            extensions.insert(extensions.end(), data + 2, data + len);
            shareFound = true;
        } else {
            append16(extensions, type); append16(extensions, len);
            extensions.insert(extensions.end(), data, data + len);
        }
        pos += len;
    }
    if (!groupsFound || !shareFound) return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    const size_t total = extensionsOffset + 2 + extensions.size();
    if (total > capacity || extensions.size() > 65535) return MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL;
    body[extensionsOffset] = static_cast<unsigned char>(extensions.size() >> 8);
    body[extensionsOffset + 1] = static_cast<unsigned char>(extensions.size());
    std::copy(extensions.begin(), extensions.end(), body + extensionsOffset + 2);
    *length = total;
    offered_ = true;
    return 0;
}

int RealityHybridExchange::derive(mbedtls_svc_key_id_t privateKey, const unsigned char* share,
                                 size_t length, unsigned char secret[64]) {
    if (!offered_ || !share || !secret || length != 1120) return MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER;
    size_t written{};
    // Validate the X25519 point (including all-zero rejection) through PSA.
    if (psa_raw_key_agreement(PSA_ALG_ECDH, privateKey, share + 1088, 32,
                             secret + 32, 32, &written) != PSA_SUCCESS || written != 32) {
        mbedtls_platform_zeroize(secret, 64);
        return MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER;
    }
    if (PQCLEAN_MLKEM768_CLEAN_crypto_kem_dec(secret, share, secretKey_.data())) {
        mbedtls_platform_zeroize(secret, 64);
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    // ML-KEM performs implicit rejection for malformed ciphertexts; the TLS
    // Finished verification then rejects a mismatched combined secret.
    mbedtls_platform_zeroize(secretKey_.data(), secretKey_.size());
    offered_ = false;
    negotiated_ = true;
    return 0;
}
