#ifndef CONVO_WS_SERVER_CONNECTION_H
#define CONVO_WS_SERVER_CONNECTION_H

#include "ServerConnection.h"
#include "WsCommonPrivate.h"
#include "WsConnection.h"

#include <atomic>
#include <map>
#include <mutex>
#include <queue>
#include <set>
#include <thread>

namespace spdlog {
   class logger;
}

struct lws;
struct lws_context;
struct lws_sorted_usec_list;

namespace convo {
   namespace ws {

      struct Packet;

      struct ServerConnectionParams
      {
         size_t maximumPacketSize{ kDefaultMaximumWsPacketSize };

         // If set, client's IP address will be read from x-forwarded-for header value if possible.
         // Last IP address in a list will be used - https://en.wikipedia.org/wiki/X-Forwarded-For#Format
         bool trustForwardedForHeader{};

         // If set, filterCallback must return true if connection is allowed and false if connection should be dropped
         using FilterCallback = std::function<bool(const std::string& ip)>;
         FilterCallback filterCallback;

         std::chrono::milliseconds handshakeTimeout{ std::chrono::seconds(5) };
         std::chrono::milliseconds clientTimeout{ std::chrono::seconds(30) };
      };

      class ServerConnection : public convo::server::Connection
      {
      public:
         ServerConnection(const std::shared_ptr<spdlog::logger>&, ServerConnectionParams);
         ~ServerConnection() override;

         ServerConnection(const ServerConnection&) = delete;
         ServerConnection& operator = (const ServerConnection&) = delete;
         ServerConnection(ServerConnection&&) = delete;
         ServerConnection& operator = (ServerConnection&&) = delete;

         bool bind(const std::string& host, const std::string& port
            , const std::shared_ptr<server::ConnectionListener>&) override;

         bool sendTo(const server::ClientId&, const std::string& data) override;
         bool sendToAll(const std::string& data) override;

         bool timer(std::chrono::milliseconds timeout, TimerCallback callback) override;
         bool close(const server::ClientId&) override;

         static int callbackHelper(struct lws* wsi, int reason, void* in, size_t len);

      private:
         enum class State
         {
            WaitHandshake,
            SendingHandshakeNew,
            SendingHandshakeResumed,
            SendingHandshakeNotFound,
            Connected,
            Closed,
         };

         struct DataToSend
         {
            server::ClientId clientId;
            RawPacket packet;
         };

         struct ConnectionData
         {
            std::string currFragment;
            State state{ State::WaitHandshake };
            server::ClientId clientId; // only for State::Connected and State::SendingHandshakeResumed
            std::string ipAddr;
         };

         struct ClientData
         {
            std::map<uint64_t, RawPacket> allPackets;
            std::string cookie;
            lws* wsi{};
            uint64_t sentCounter{};
            uint64_t sentAckCounter{};
            uint64_t queuedCounter{};
            uint64_t recvCounter{};
            uint64_t recvAckCounter{};
         };

         void listenFunction();

         void stopServer();
         bool isActive() { return listenThread_.joinable(); }

         int callback(lws* wsi, int reason, void* in, size_t len);

         // Methods accessible from listener thread only
         server::ClientId nextClientId();
         bool done() const;
         bool writeNeeded(const ClientData& client) const;
         void requestWriteIfNeeded(const ClientData& client);
         bool processSentAck(ClientData& client, uint64_t sentAckCounter);
         void processError(lws* wsi);
         void closeConnectedClient(const server::ClientId&);

         std::shared_ptr<spdlog::logger>  logger_;
         const ServerConnectionParams params_;

         std::thread listenThread_;
         std::weak_ptr<server::ConnectionListener> listener_;
         std::atomic_bool shuttingDown_{};
         lws_context* context_{};

         mutable std::mutex mutex_;
         std::queue<DataToSend> packets_;
         std::queue<std::string> forceClosingClients_;

         // Fields accessible from listener thread only
         std::map<lws*, ConnectionData> connections_;
         std::map<std::string, ClientData> clients_;
         std::map<std::string, server::ClientId> cookieToClientIdMap_;
         bool shuttingDownReceived_{};
         TimerHelper timers_;

      };

   }
}

#endif // CONVO_WS_SERVER_CONNECTION_H
