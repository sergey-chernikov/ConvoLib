#include "WsConnection.h"
#if BUILD_LWS
#ifdef WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif
#include <libwebsockets.h>
#endif
#include <spdlog/spdlog.h>
#include <openssl/ec.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#ifdef BUILD_ARMORY
#include "Utils/Cryptography.h"
#endif
#include "base/StringUtils.h"

using namespace convo::ws;

#if BUILD_LWS
const char *convo::ws::kProtocolName = "convo-ws-proto";

namespace convo {

   std::string peerAddressString(int socket)
   {
#ifdef WIN32
      SOCKET sock = socket;
      SOCKADDR_IN peerInfo = { };
      int peerLen = sizeof(peerInfo);
#else
      int sock = socket;
      sockaddr_in peerInfo = { };
      socklen_t peerLen = sizeof(peerInfo);
#endif

      const auto rc = getpeername(sock, (sockaddr*)&peerInfo, &peerLen);
      if (rc == 0) {
         return inet_ntoa(peerInfo.sin_addr);
      } else {
         return "";
      }
   }

   namespace ws {

      constexpr size_t kLwsPrePaddingSize = LWS_PRE;

      constexpr auto kDefaultCurve = NID_X9_62_prime256v1;
      const auto kDefaultHash = "SHA-256";

      class RawPacketBuilder
      {
      private:
         std::vector<uint8_t> data_;

      public:
         RawPacketBuilder(Packet::Type type)
         {
            data_.push_back(static_cast<uint8_t>(type));
         }

         RawPacketBuilder& putString(const std::string& data)
         {
            const uint32_t size = data.size();
            std::array<uint8_t, sizeof(size)> buf;
            std::memcpy(buf.data(), &size, buf.size());
            data_.insert(data_.end(), buf.begin(), buf.end());
            data_.insert(data_.end(), data.begin(), data.end());
            return *this;
         }

         RawPacketBuilder& putNumber(uint64_t n)
         {
            std::array<uint8_t, sizeof(n)> buf;
            std::memcpy(buf.data(), &n, buf.size());
            data_.insert(data_.end(), buf.begin(), buf.end());
            return *this;
         }

         RawPacket build()
         {
            return RawPacket(data_);
         }
      };

      constexpr auto kPingPongInterval = std::chrono::seconds(60);
      constexpr auto kHangupInterval = std::chrono::seconds(90);
      const lws_retry_bo kDefaultRetryAndIdlePolicy = { nullptr, 0, 0
         , uint16_t(kPingPongInterval / std::chrono::seconds(1)), uint16_t(kHangupInterval / std::chrono::seconds(1)), 0 };

      std::once_flag g_sslInitFlag;

      const lws_retry_bo* defaultRetryAndIdlePolicy()
      {
         return &kDefaultRetryAndIdlePolicy;
      }

      std::string connectedIp(lws* wsi)
      {
         const auto socket = (int)lws_get_socket_fd(wsi);
         auto ipAddr = peerAddressString(socket);
         return ipAddr;
      }

      std::string forwardedIp(lws* wsi)
      {
         int n = lws_hdr_total_length(wsi, WSI_TOKEN_X_FORWARDED_FOR);
         if (n < 0) {
            return "";
         }
         std::string value;
         value.resize(static_cast<size_t>(n + 1));
         n = lws_hdr_copy(wsi, &value[0], static_cast<int>(value.size()), WSI_TOKEN_X_FORWARDED_FOR);
         if (n < 0) {
            assert(false);
            return "";
         }
         value.resize(static_cast<size_t>(n));
         auto ipAddr = convo::trim(convo::split(value, ',').back());
         return ipAddr;
      }

      std::string certPublicKey(x509_store_ctx_st* ctx)
      {
         auto currCert = X509_STORE_CTX_get_current_cert(ctx);
         if (!currCert) {
            throw std::runtime_error("X509_STORE_CTX_get_current_cert failed");
         }
         auto pubKey = X509_get0_pubkey(currCert);
         if (!pubKey) {
            throw std::runtime_error("X509_get0_pubkey failed");
         }
         auto ecKey = EVP_PKEY_get0_EC_KEY(pubKey);
         if (!ecKey) {
            throw std::runtime_error("EVP_PKEY_get0_EC_KEY failed");
         }
         auto point = EC_KEY_get0_public_key(ecKey);
         if (!point) {
            throw std::runtime_error("EC_KEY_get0_public_key failed");
         }
         auto group = EC_KEY_get0_group(ecKey);
         if (!group) {
            throw std::runtime_error("EC_KEY_get0_group failed");
         }
         int curveName = EC_GROUP_get_curve_name(group);
         if (curveName != kDefaultCurve) {
            throw std::runtime_error(std::format("unexpected curve name: {}", curveName));
         }
         auto size = EC_POINT_point2oct(group, point, POINT_CONVERSION_COMPRESSED, nullptr, 0, nullptr);
         if (size == 0) {
            throw std::runtime_error("EC_POINT_point2oct failed");
         }
         std::string result(size, 0);
         size = EC_POINT_point2oct(group, point, POINT_CONVERSION_COMPRESSED
            , reinterpret_cast<uint8_t*>(&result[0]), result.size(), nullptr);
         if (size != result.size()) {
            throw std::runtime_error("EC_POINT_point2oct failed");
         }
         return result;
      }

      ws::PrivateKey generatePrivKey()
      {
#ifdef BUILD_ARMORY
         return Cryptography::PRNG::generateRandomStrong(32).getDataVector();
#else
         //TODO
         return {};
#endif
      }

#if 0 //the code below fails to compile on Win32 with Botan 2.19.5, luckily it's not used in any project atm
#include <botan/x509self.h>

      std::string ws::generateSelfSignedCert(const PrivateKey& privKey, const std::chrono::seconds& expireTime)
      {
         std::vector<uint8_t> output;
#ifdef BUILD_BOTAN_
         Botan::AutoSeeded_RNG rng;
         Botan::DataSource_Memory privKeySrc(privKey);
         auto privKeyLoaded = Botan::PKCS8::load_key(privKeySrc);
         auto expireTimeSeconds = expireTime / std::chrono::seconds(1);
         Botan::X509_Cert_Options options("", static_cast<uint32_t>(expireTimeSeconds));
         auto cert = Botan::X509::create_self_signed_cert(options, *privKeyLoaded, kDefaultHash, rng);
         Botan::DER_Encoder encoder(output);
         cert.encode_into(encoder);
#else
         //TODO
#endif
         return std::string(output.begin(), output.end());
      }
#endif

      std::string publicKey(const PrivateKey& privKey)
      {
         auto priv = BN_bin2bn(privKey.data(), privKey.size(), NULL);
         auto key = EC_KEY_new_by_curve_name(kDefaultCurve);
         EC_KEY_set_private_key(key, priv);
         auto group = EC_KEY_get0_group(key);
         auto pubKey = (EC_POINT*)EC_KEY_get0_public_key(key);
         EC_POINT_mul(group, pubKey, priv, NULL, NULL, NULL);
         EC_KEY_set_public_key(key, pubKey);
         const auto len = EC_POINT_point2oct(group, pubKey, POINT_CONVERSION_COMPRESSED
            , NULL, 0, NULL);
         if (!len) { return {}; }
         std::string compressedPubKey(len, 0);
         if (EC_POINT_point2oct(group, pubKey, POINT_CONVERSION_COMPRESSED
            , (unsigned char*)compressedPubKey.data(), len, NULL) != len) {
            return {};
         }
         return compressedPubKey;
      }

      long sslOptionsSet()
      {
         // Allow TLSv1_3 only
         return
            SSL_OP_NO_SSLv2 |
            SSL_OP_NO_SSLv3 |
            SSL_OP_NO_TLSv1 |
            SSL_OP_NO_TLSv1_1 |
            SSL_OP_NO_TLSv1_2;
      }

   }
}

using namespace convo::ws;

RawPacket::RawPacket(const std::vector<uint8_t> &data)
{
   data_.resize(kLwsPrePaddingSize);
   data_.insert(data_.end(), data.begin(), data.end());
}

uint8_t *RawPacket::getPtr()
{
   return data_.data() + kLwsPrePaddingSize;
}

size_t RawPacket::getSize() const
{
   return data_.size() - kLwsPrePaddingSize;
}

RawPacket Packet::requestNew()
{
   return RawPacketBuilder(Type::RequestNew).build();
}

RawPacket Packet::requestResumed(const std::string &cookie, uint64_t recvCounter)
{
   return RawPacketBuilder(Type::RequestResumed).putNumber(recvCounter)
         .putString(cookie).build();
}

RawPacket Packet::responseNew(const std::string &cookie)
{
   return RawPacketBuilder(Type::ResponseNew).putString(cookie).build();
}

RawPacket Packet::responseResumed(uint64_t recvCounter)
{
   return RawPacketBuilder(Type::ResponseResumed).putNumber(recvCounter).build();
}

RawPacket Packet::responseUnknown()
{
   return RawPacketBuilder(Type::ResponseUnknown).build();
}

RawPacket Packet::data(const std::string &payload)
{
   return RawPacketBuilder(Type::Data).putString(payload).build();
}

RawPacket Packet::ack(uint64_t recvCounter)
{
   return RawPacketBuilder(Type::Ack).putNumber(recvCounter).build();
}

Packet Packet::parse(const std::string &data)
{
   if (data.empty()) { return {}; }
   Packet result{};
   unsigned pos = 0;
   const auto& readData = [data, &pos](void* dest, unsigned size)
   {
      if (!dest) {
         throw std::invalid_argument("can't read to nullptr");
      }
      if (data.size() < (pos + size)) {
         throw std::invalid_argument(std::format("can't read {} bytes at pos {} (size = {})"
            , size, pos, data.size()));
      }
      std::memcpy(dest, data.data() + pos, size);
      pos += size;
   };

   result.type = static_cast<Type>(data.at(pos++));
   if (result.type < Type::Min || result.type > Type::Max) {
      throw std::runtime_error("invalid packet type");
   }

   switch (result.type) {
      case Type::RequestResumed:
      case Type::ResponseResumed:
      case Type::Ack: 
         readData(&result.recvCounter, sizeof(result.recvCounter));
         break;
      default:
         break;
   }

   switch (result.type) {
      case Type::RequestResumed:
      case Type::ResponseNew:
      case Type::Data: {
         uint32_t payloadSize = 0;
         readData(&payloadSize, sizeof(payloadSize));
         if (int(data.size() - pos) < (int)payloadSize) {
            throw std::invalid_argument(std::format("data has less bytes ({}) than payload size {}"
               , data.size() - pos, payloadSize));
         }
         std::string payload(payloadSize, 0);
         readData(payload.data(), payloadSize);
         result.payload = std::move(payload);
         break;
      }
      default:
         break;
   }

   if (pos < data.size()) {
      throw std::invalid_argument(std::format("there're some bytes ({}) left unparsed", data.size() - pos));
   }
   return result;
}

#endif   //BUILD_LWS
