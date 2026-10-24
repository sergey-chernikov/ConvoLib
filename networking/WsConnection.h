#ifndef CONVO_WS_CONNECTION_H
#define CONVO_WS_CONNECTION_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct lws;
struct lws_retry_bo;
struct x509_store_ctx_st;

#if 0
extern "C" {
   extern int openssl_websocket_private_data_index, openssl_SSL_CTX_private_data_index;
}
#endif

namespace convo {

   std::string peerAddressString(int socket);

   namespace ws {

      using PrivateKey = std::vector<uint8_t>;
      constexpr size_t kDefaultMaximumWsPacketSize = 100 * 1024 * 1024;

      const lws_retry_bo* defaultRetryAndIdlePolicy();

      std::string connectedIp(lws* wsi);

      // NOTE: Not available after LWS_CALLBACK_ESTABLISHED
      std::string forwardedIp(lws* wsi);

      std::string certPublicKey(x509_store_ctx_st* ctx);

      long sslOptionsSet();

      // Generates secp256r1 private key (in DER format)
      PrivateKey generatePrivKey();

      // Returns compressed public key (33 bytes)
      std::string publicKey(const PrivateKey& privKey);
#if 0
      // Generate self-signed cerificate for the privKey
      std::string generateSelfSignedCert(const PrivateKey& privKey
         , const std::chrono::seconds& expireTime = std::chrono::hours(20 * 365 * 24));
#endif

      class RawPacket
      {
      private:
         // Actual data padded by LWS_PRE
         std::vector<uint8_t> data_;

      public:
         explicit RawPacket(const std::vector<uint8_t>&);

         uint8_t* getPtr();
         size_t getSize() const;
      };

      extern const char* kProtocolName;

      constexpr size_t kRxBufferSize = 16 * 1024;
      constexpr size_t kTxPacketSize = 16 * 1024;
      constexpr int kId = 0;

      struct Packet
      {
         enum class Type : uint8_t
         {
            Invalid = 0,
            RequestNew = 0x11,
            RequestResumed = 0x12,
            ResponseNew = 0x13,
            ResponseResumed = 0x14,
            ResponseUnknown = 0x15,
            Data = 0x16,
            Ack = 0x17,

            Min = RequestNew,
            Max = Ack,
         };

         Type type{};
         std::string payload;
         size_t recvCounter{};

         static RawPacket requestNew();
         static RawPacket requestResumed(const std::string& cookie, uint64_t recvCounter);
         static RawPacket responseNew(const std::string& cookie);
         static RawPacket responseResumed(uint64_t recvCounter);
         static RawPacket responseUnknown();
         static RawPacket data(const std::string& payload);
         static RawPacket ack(uint64_t recvCounter);

         static Packet parse(const std::string&);
      };
   }
}

#endif // CONVO_WS_CONNECTION_H
