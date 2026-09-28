#pragma once

#include <array>
#include <cstddef>
#include <psa/crypto.h>

// TLS X25519MLKEM768: ML-KEM material precedes X25519 material on the wire
// and in the combined shared secret. The X25519 key also authenticates REALITY.
class RealityHybridExchange {
public:
    ~RealityHybridExchange();
    RealityHybridExchange() = default;
    RealityHybridExchange(const RealityHybridExchange&) = delete;
    RealityHybridExchange& operator=(const RealityHybridExchange&) = delete;
    int expandClientHello(unsigned char* body, size_t* length, size_t capacity);
    int derive(mbedtls_svc_key_id_t privateKey, const unsigned char* share,
               size_t length, unsigned char secret[64]);
    bool negotiated() const { return negotiated_; }
private:
    std::array<unsigned char, 1184> publicKey_{};
    std::array<unsigned char, 2400> secretKey_{};
    bool offered_{};
    bool negotiated_{};
};
