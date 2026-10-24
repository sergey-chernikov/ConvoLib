#ifndef __CONVO_SERVER_CONNECTION_LISTENER_H__
#define __CONVO_SERVER_CONNECTION_LISTENER_H__

#include <map>
#include <string>

namespace convo {
   namespace server {

      using ClientId = std::string;

      enum class ClientError : uint8_t
      {
         NoError = 0,
         // Reported when client do not have valid credentials (unknown public key)
         HandshakeFailed = 1,
         Timeout = 2,
      };

      enum class Detail : uint8_t
      {
         Unknown = 0,
         IpAddr = 1,
         PublicKey = 2,
      };
      using Details = std::map<Detail, std::string>;

      class ConnectionListener
      {
      public:
         ConnectionListener() = default;
         virtual ~ConnectionListener() noexcept = default;

         ConnectionListener(const ConnectionListener&) = delete;
         ConnectionListener& operator = (const ConnectionListener&) = delete;
         ConnectionListener(ConnectionListener&&) = delete;
         ConnectionListener& operator = (ConnectionListener&&) = delete;

         virtual void onDataFrom(const ClientId&, const std::string& data) = 0;
         virtual void onConnected(const ClientId&, const Details& details) = 0;
         virtual void onDisconnected(const ClientId&) = 0;
         virtual void onError(const ClientId&, ClientError, const Details & = {}) {}
      };
   }
}

#endif // __CONVO_SERVER_CONNECTION_LISTENER_H__
