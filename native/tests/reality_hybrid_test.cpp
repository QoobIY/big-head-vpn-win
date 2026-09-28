#include "reality_hybrid.h"
#include <mbedtls/ssl.h>
extern "C" {
#include "kem.h"
}
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
unsigned read16(const unsigned char* p) { return (unsigned(p[0]) << 8) | p[1]; }
void put16(std::vector<unsigned char>& out, unsigned value) {
    out.push_back(static_cast<unsigned char>(value >> 8)); out.push_back(static_cast<unsigned char>(value));
}
struct Key {
    mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
    std::array<unsigned char, 32> pub{};
    Key() {
        psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
        psa_set_key_bits(&attributes, 255);
        psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DERIVE);
        psa_set_key_algorithm(&attributes, PSA_ALG_ECDH);
        check(psa_generate_key(&attributes, &id) == PSA_SUCCESS, "generate X25519");
        size_t length{};
        check(psa_export_public_key(id, pub.data(), pub.size(), &length) == PSA_SUCCESS && length == 32, "export X25519");
    }
    ~Key() { psa_destroy_key(id); }
};
std::vector<unsigned char> hello(const Key& client) {
    std::vector<unsigned char> data(67, 0);
    data[0] = 3; data[1] = 3; data[34] = 32;
    put16(data, 2); put16(data, 0x1301); data.push_back(1); data.push_back(0);
    put16(data, 50);
    put16(data, 10); put16(data, 4); put16(data, 2); put16(data, 29);
    put16(data, 51); put16(data, 38); put16(data, 36); put16(data, 29); put16(data, 32);
    data.insert(data.end(), client.pub.begin(), client.pub.end());
    return data;
}
int main() {
    try {
        check(psa_crypto_init() == PSA_SUCCESS, "PSA initialization");
        Key client, server;
        const auto original = hello(client);
        RealityHybridExchange exchange;
        std::array<unsigned char, 64> result{};
        std::array<unsigned char, 1120> share{};
        check(exchange.derive(client.id, share.data(), share.size(), result.data()) != 0, "reject unoffered group");
        for (size_t size : {size_t(0), size_t(34), size_t(66), original.size() - 1}) {
            auto truncated = original;
            check(exchange.expandClientHello(truncated.data(), &size, truncated.size()) != 0, "reject truncated ClientHello");
        }
        auto small = original; size_t smallLength = small.size();
        check(exchange.expandClientHello(small.data(), &smallLength, small.size()) != 0, "reject insufficient capacity");
        for (bool corrupt : {false, true}) {
            auto data = original; size_t length = data.size(); data.resize(4096);
            check(exchange.expandClientHello(data.data(), &length, data.size()) == 0, "expand ClientHello");
            check(length == original.size() + 1222, "expanded length");
            check(read16(data.data() + 79) == 4 && read16(data.data() + 81) == 0x11ec && read16(data.data() + 83) == 29, "hybrid first in supported groups");
            size_t pos = 75; const unsigned char* publicKey = nullptr;
            while (pos < length) {
                const auto type = read16(data.data() + pos), size = read16(data.data() + pos + 2);
                if (type == 51) {
                    const auto* entry = data.data() + pos + 6;
                    check(read16(entry) == 0x11ec && read16(entry + 2) == 1216, "hybrid key share");
                    publicKey = entry + 4;
                    check(std::equal(client.pub.begin(), client.pub.end(), publicKey + 1184), "X25519 in hybrid share");
                    check(read16(entry + 1220) == 29 && read16(entry + 1222) == 32, "classical fallback share");
                }
                pos += size + 4;
            }
            check(publicKey != nullptr && pos == length, "extension lengths");
            std::array<unsigned char, 32> kemSecret{}, coins{}, ecdh{};
            check(psa_generate_random(coins.data(), coins.size()) == PSA_SUCCESS, "encapsulation randomness");
            check(PQCLEAN_MLKEM768_CLEAN_crypto_kem_enc_derand(share.data(), kemSecret.data(), publicKey, coins.data()) == 0, "encapsulate");
            std::copy(server.pub.begin(), server.pub.end(), share.begin() + 1088);
            size_t written{};
            check(psa_raw_key_agreement(PSA_ALG_ECDH, server.id, client.pub.data(), 32, ecdh.data(), 32, &written) == PSA_SUCCESS && written == 32, "server X25519 secret");
            check(exchange.derive(client.id, share.data(), share.size() - 1, result.data()) != 0, "reject short ciphertext");
            auto zeroPoint = share; std::fill(zeroPoint.begin() + 1088, zeroPoint.end(), 0);
            check(exchange.derive(client.id, zeroPoint.data(), zeroPoint.size(), result.data()) != 0, "reject zero X25519 point");
            if (corrupt) share[0] ^= 1;
            check(exchange.derive(client.id, share.data(), share.size(), result.data()) == 0, "derive hybrid secret");
            check(std::equal(ecdh.begin(), ecdh.end(), result.begin() + 32), "X25519 secret ordering");
            check(std::equal(kemSecret.begin(), kemSecret.end(), result.begin()) != corrupt, "ML-KEM agreement and implicit rejection");
            check(exchange.derive(client.id, share.data(), share.size(), result.data()) != 0, "reject consumed key");
        }
        std::cout << "hybrid exchange: agreement, fallback offer, bounds, invalid point and ciphertext rejection passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
