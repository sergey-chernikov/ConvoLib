#ifndef CONVO_WS_DATA_CONNECTION_H
#define CONVO_WS_DATA_CONNECTION_H

#include "DataConnection.h"
#include "WsCommonPrivate.h"
#include "WsConnection.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

namespace spdlog {
   class logger;
}
struct lws_context;
struct lws_retry_bo;
struct lws_sorted_usec_list;

namespace convo {
   namespace ws {
      struct TimerStruct;

      struct DataConnectionParams
      {
         const void* caBundlePtr{};
         size_t caBundleSize{};
         bool useSsl{ false };
         size_t maximumPacketSize{ kDefaultMaximumWsPacketSize };
         std::vector<uint32_t> delaysTableMs;
         std::uint32_t timeoutSecs{};
      };

      class DataConnection : public convo::DataConnection
      {
      public:
         DataConnection(const std::shared_ptr<spdlog::logger>& logger, DataConnectionParams params);
         ~DataConnection() override;

         DataConnection(const DataConnection&) = delete;
         DataConnection& operator = (const DataConnection&) = delete;
         DataConnection(DataConnection&&) = delete;
         DataConnection& operator = (DataConnection&&) = delete;

         bool open(const std::string& host, const std::string& port
            , const std::shared_ptr<DataConnectionListener>&) override;
         bool close() override;

         bool send(const std::string& data) override;
         bool isActive() const override;

         bool timer(std::chrono::milliseconds timeout, TimerCallback callback) override;

         static int callbackHelper(struct lws* wsi, int reason, void* user, void* in, size_t len);

      private:
         enum class State
         {
            Connecting,
            Reconnecting,
            WaitingNewResponse,
            WaitingResumedResponse,
            Connected,
            Closing,
            Closed,
         };

         int callback(struct lws* wsi, int reason, void* user, void* in, size_t len);

         static void reconnectCallback(lws_sorted_usec_list* list);

         void listenFunction();

         void scheduleReconnect();
         void reconnect();
         void processError();
         void processFatalError();
         bool writeNeeded() const;
         void requestWriteIfNeeded();
         bool processSentAck(uint64_t sentAckCounter);

         // For tests, default is noop
         virtual RawPacket filterRawPacket(const RawPacket& packet) const;

         std::shared_ptr<spdlog::logger> logger_;
         const DataConnectionParams params_;

         lws_context* context_{};
         std::string host_;
         int port_{};
         std::atomic_bool shuttingDown_{};

         std::thread listenThread_;

         std::mutex mutex_;
         std::queue<RawPacket> newPackets_;

         // Fields accessible from listener thread only!
         std::map<uint64_t, RawPacket> allPackets_;
         std::string currFragment_;
         lws* wsi_{};
         State state_{ State::Connecting };
         uint64_t sentCounter_{};
         uint64_t sentAckCounter_{};
         uint64_t queuedCounter_{};
         uint64_t recvCounter_{};
         uint64_t recvAckCounter_{};
         std::string cookie_;
         std::unique_ptr<TimerStruct> reconnectTimer_;
         uint16_t retryCounter_{};
         bool shuttingDownReceived_{};
         std::unique_ptr<lws_retry_bo> retryTable_;
         TimerHelper timers_;
      };

   }
}

#endif // CONVO_WS_DATA_CONNECTION_H
