#include "WsServerConnection.h"
#ifdef BUILD_ARMORY
#include "Utils/BinaryData.h"
#include "Utils/Cryptography.h"
#endif
#include "base/StringUtils.h"
#include "base/ThreadName.h"

#include <random>
#if BUILD_LWS
#include <libwebsockets.h>
#endif
#include <spdlog/spdlog.h>

#if BUILD_LWS
namespace convo {
   namespace ws {

      int callback(lws* wsi, lws_callback_reasons reason, void* /*user*/, void* in, size_t len)
      {
         return ServerConnection::callbackHelper(wsi, reason, in, len);
      }

      struct lws_protocols kProtocols[] = {
         { kProtocolName, callback, 0, kRxBufferSize, kId, nullptr, kTxPacketSize },
         { nullptr, nullptr, 0, 0, 0, nullptr, 0 },
      };

      // Make sure that regular clientId can't clash with that
      const std::string kAllClientsId{};

      // Do not disconnect banned clients immediately so they have chance to receive pending messages
      const int kForcedDisconnectTimeSeconds = 1;

      static uint64_t generateRandomClientId()
      {
         static std::random_device rd;
         static std::mt19937_64 gen(rd());
         static std::uniform_int_distribution<uint64_t> dis;
         return dis(gen);
      }

      static std::string generateNewCookie()
      {
         std::string cookie(32, 0);
         for (int i = 0; i < cookie.size() / sizeof(uint64_t); ++i) {
            auto id = generateRandomClientId();
            std::memcpy(&cookie[i * sizeof(uint64_t)], &id, sizeof(id));
         }
         return cookie;
      }

   }
} // namespace convo

using namespace convo::ws;

ServerConnection::ServerConnection(const std::shared_ptr<spdlog::logger>& logger, ServerConnectionParams params)
   : logger_(logger), params_(std::move(params))
{}

ServerConnection::~ServerConnection()
{
   stopServer();
}

bool ServerConnection::bind(const std::string& /*host*/, const std::string& port
   , const std::shared_ptr<convo::server::ConnectionListener>& listener)
{
   stopServer();

   struct lws_context_creation_info info;
   memset(&info, 0, sizeof(info));
   info.port = std::stoi(port);
   info.protocols = kProtocols;
   info.gid = -1;
   info.uid = -1;
   info.retry_and_idle_policy = defaultRetryAndIdlePolicy();
   info.options = LWS_SERVER_OPTION_VALIDATE_UTF8 | LWS_SERVER_OPTION_DISABLE_IPV6;
   info.user = this;

   // Context creation will return nullptr if port binding failed
   context_ = lws_create_context(&info);
   if (context_ == nullptr) {
      SPDLOG_LOGGER_ERROR(logger_, "context create failed");
      return false;
   }

   shuttingDown_ = false;
   listener_ = listener;
   listenThread_ = std::thread(&ServerConnection::listenFunction, this);
   return true;
}

void ServerConnection::listenFunction()
{
   convo::setCurrentThreadName("WsServer");

   while (!done()) {
      lws_service(context_, 0);
   }
}

void ServerConnection::stopServer()
{
   if (!listenThread_.joinable()) {
      return;
   }

   shuttingDown_ = true;
   lws_cancel_service(context_);

   listenThread_.join();

   lws_context_destroy(context_);
   listener_.reset();
   context_ = nullptr;

   packets_ = {};
   clients_ = {};
   connections_ = {};
   cookieToClientIdMap_ = {};
   shuttingDownReceived_ = {};
   timers_.clear();
}

int ServerConnection::callback(lws *wsi, int reason, void *in, size_t len)
{
   switch (reason) {
      case LWS_CALLBACK_EVENT_WAIT_CANCELLED: {
         std::queue<DataToSend> packets;
         std::queue<std::string> forceClosingClients;
         {
            std::lock_guard<std::mutex> lock(mutex_);
            std::swap(packets, packets_);
            std::swap(forceClosingClients, forceClosingClients_);
         }
         while (!packets.empty()) {
            auto data = std::move(packets.front());
            packets.pop();

            if (data.clientId == kAllClientsId) {
               for (auto &item : clients_) {
                  auto &client = item.second;
                  client.allPackets.insert(std::make_pair(client.queuedCounter, data.packet));
                  client.queuedCounter += 1;
                  requestWriteIfNeeded(client);
               }
               continue;
            }

            auto clientIt = clients_.find(data.clientId);
            if (clientIt == clients_.end()) {
               continue;
            }
            auto &client = clientIt->second;
            client.allPackets.insert(std::make_pair(client.queuedCounter, data.packet));
            client.queuedCounter += 1;
            requestWriteIfNeeded(client);
         }

         if (shuttingDown_.load() && !shuttingDownReceived_) {
            shuttingDownReceived_ = true;
            for (const auto &connection : connections_) {
               lws_close_reason(connection.first, LWS_CLOSE_STATUS_NORMAL, nullptr, 0);
               lws_set_timeout(connection.first, PENDING_TIMEOUT_USER_OK, kForcedDisconnectTimeSeconds);
            }
         }

         while (!forceClosingClients.empty()) {
            auto clientId = std::move(forceClosingClients.front());
            forceClosingClients.pop();

            auto clientIt = clients_.find(clientId);
            if (clientIt != clients_.end()) {
               SPDLOG_LOGGER_DEBUG(logger_, "force close client {}", convo::toHex(clientId));
               auto clientWsi = clientIt->second.wsi;
               if (clientWsi) {
                  auto &connection = connections_.at(clientWsi);
                  connection.state = State::Closed;
                  lws_close_reason(clientWsi, LWS_CLOSE_STATUS_PROTOCOL_ERR, nullptr, 0);
                  lws_set_timeout(clientWsi, PENDING_TIMEOUT_USER_OK, LWS_TO_KILL_SYNC);
               }
               closeConnectedClient(clientId);
            }
            const auto& listener = listener_.lock();
            if (listener) {
               listener->onDisconnected(clientId);
            }
         }

         break;
      }

      case LWS_CALLBACK_ESTABLISHED: {
         auto &connection = connections_[wsi];
         auto connIp = connectedIp(wsi);
         auto forwIp = forwardedIp(wsi);
         connection.ipAddr = params_.trustForwardedForHeader && !forwIp.empty() ? forwIp : connIp;
         SPDLOG_LOGGER_DEBUG(logger_, "wsi connected: {}, connected ip: {}, forwarded ip: {}"
            , static_cast<void*>(wsi), connIp, forwIp);
         if (shuttingDown_) {
            connection.state = State::Closed;
            return -1;
         }
         if (params_.filterCallback && !params_.filterCallback(connection.ipAddr)) {
            SPDLOG_LOGGER_DEBUG(logger_, "drop connection because filterCallback returns false");
            // NOTE: Simply returning -1 is not enough here for some reasons!
            connection.state = State::Closed;
            return -1;
         }

         timers_.scheduleCallback(context_, params_.handshakeTimeout, [this, wsi] {
            auto it = connections_.find(wsi);
            if (it != connections_.end() && it->second.state != State::Connected) {
               SPDLOG_LOGGER_ERROR(logger_, "close client because handshake is not complete on time");
               lws_close_reason(wsi, LWS_CLOSE_STATUS_PROTOCOL_ERR, nullptr, 0);
               lws_set_timeout(wsi, PENDING_TIMEOUT_USER_OK, LWS_TO_KILL_SYNC);
            }
         });
         break;
      }

      case LWS_CALLBACK_CLOSED: {
         SPDLOG_LOGGER_DEBUG(logger_, "wsi disconnected: {}", static_cast<void*>(wsi));
         auto connectionIt = connections_.find(wsi);
         assert(connectionIt != connections_.end());
         auto &connection = connectionIt->second;
         switch (connection.state) {
            case State::Connected:
            case State::SendingHandshakeResumed: {
               const auto &clientId = connection.clientId;
               auto &client = clients_.at(connection.clientId);
               SPDLOG_LOGGER_DEBUG(logger_, "connection closed unexpectedly, clientId: {}", convo::toHex(connection.clientId));
               client.wsi = nullptr;
               timers_.scheduleCallback(context_, params_.clientTimeout, [this, clientId] {
                  auto clientIt = clients_.find(clientId);
                  if (clientIt == clients_.end()) {
                     return;
                  }
                  auto &client = clientIt->second;
                  if (client.wsi == nullptr) {
                     SPDLOG_LOGGER_ERROR(logger_, "connection removed by timeout");
                     closeConnectedClient(clientId);
                     const auto& listener = listener_.lock();
                     if (listener) {
                        listener->onDisconnected(clientId);
                        listener->onError(clientId, convo::server::ClientError::Timeout);
                     }
                  }
               });
               break;
            }
            case State::SendingHandshakeNew:
            case State::SendingHandshakeNotFound:
            case State::WaitHandshake:
            case State::Closed: {
               // Nothing to do
               break;
            }
         }
         connections_.erase(connectionIt);
         break;
      }

      case LWS_CALLBACK_RECEIVE: {
         auto &connection = connections_.at(wsi);

         auto ptr = static_cast<const char*>(in);
         connection.currFragment.insert(connection.currFragment.end(), ptr, ptr + len);
         if (connection.currFragment.size() > params_.maximumPacketSize) {
            SPDLOG_LOGGER_ERROR(logger_, "maximum packet size reached");
            processError(wsi);
            return -1;
         }
         if (lws_remaining_packet_payload(wsi) > 0) {
            return 0;
         }
         if (!lws_is_final_fragment(wsi)) {
            SPDLOG_LOGGER_ERROR(logger_, "unexpected fragment");
            processError(wsi);
            return -1;
         }

         //TODO: try-catch here and handle parse error
         auto packet = Packet::parse(connection.currFragment);
         connection.currFragment.clear();

         switch (connection.state) {
            case State::Connected: {
               auto &client = clients_.at(connection.clientId);
               switch (packet.type) {
                  case Packet::Type::Data: {
                     client.recvCounter += 1;
                     const auto& listener = listener_.lock();
                     if (listener) {
                        listener->onDataFrom(connection.clientId, packet.payload);
                     }
                     break;
                  }
                  case Packet::Type::Ack: {
                     if (!processSentAck(client, packet.recvCounter)) {
                        SPDLOG_LOGGER_ERROR(logger_, "invalid ack");
                        processError(wsi);
                        return -1;
                     }
                     break;
                  }
                  default: {
                     SPDLOG_LOGGER_ERROR(logger_, "unexpected packet");
                     processError(wsi);
                     return -1;
                  }
               }
               requestWriteIfNeeded(client);
               return 0;
            }
            case State::WaitHandshake: {
               switch (packet.type) {
                  case Packet::Type::RequestNew: {
                     connection.state = State::SendingHandshakeNew;
                     lws_callback_on_writable(wsi);
                     return 0;
                  }
                  case Packet::Type::RequestResumed: {
                     auto cookieIt = cookieToClientIdMap_.find(packet.payload);
                     if (cookieIt == cookieToClientIdMap_.end()) {
                        connection.state = State::SendingHandshakeNotFound;
                        lws_callback_on_writable(wsi);
                        SPDLOG_LOGGER_ERROR(logger_, "resume cookie not found");
                        return 0;
                     }
                     const auto &clientId = cookieIt->second;
                     auto &client = clients_.at(clientId);
                     if (!processSentAck(client, packet.recvCounter)) {
                        SPDLOG_LOGGER_ERROR(logger_, "resuming connection failed");
                        processError(wsi);
                        return -1;
                     }
                     if (client.wsi != nullptr) {
                        auto &oldConnection = connections_.at(client.wsi);
                        oldConnection.state = State::Closed;
                        lws_callback_on_writable(client.wsi);
                     }
                     connection.state = State::SendingHandshakeResumed;
                     connection.clientId = clientId;
                     client.wsi = wsi;
                     client.sentCounter = packet.recvCounter;
                     lws_callback_on_writable(wsi);
                     return 0;
                  }
                  default: {
                     SPDLOG_LOGGER_ERROR(logger_, "unexpected packet");
                     processError(wsi);
                     return -1;
                  }
               }
            }
            case State::SendingHandshakeResumed: {
               auto &client = clients_.at(connection.clientId);
               client.wsi = nullptr;
               SPDLOG_LOGGER_ERROR(logger_, "unexpected packet");
               processError(wsi);
               return -1;
            }
            case State::SendingHandshakeNotFound:
            case State::SendingHandshakeNew: {
               SPDLOG_LOGGER_ERROR(logger_, "unexpected packet");
               processError(wsi);
               return -1;
            }
            case State::Closed: {
               processError(wsi);
               return -1;
            }
         }
         break;
      }

      case LWS_CALLBACK_SERVER_WRITEABLE: {
         auto &connection = connections_.at(wsi);

         switch (connection.state) {
            case State::Connected: {
               auto &client = clients_.at(connection.clientId);

               if (client.recvCounter != client.recvAckCounter) {
                  auto packet = Packet::ack(client.recvCounter);
                  int rc = lws_write(wsi, packet.getPtr(), packet.getSize(), LWS_WRITE_BINARY);
                  if (rc != static_cast<int>(packet.getSize())) {
                      SPDLOG_LOGGER_ERROR(logger_, "write failed");
                      processError(wsi);
                      return -1;
                  }
                  client.recvAckCounter = client.recvCounter;
               } else if (client.sentCounter != client.queuedCounter) {
                  auto &packet = client.allPackets.at(client.sentCounter);
                  int rc = lws_write(wsi, packet.getPtr(), packet.getSize(), LWS_WRITE_BINARY);
                  if (rc == -1) {
                     SPDLOG_LOGGER_ERROR(logger_, "write failed");
                     processError(wsi);
                     return -1;
                  }
                  if (rc != static_cast<int>(packet.getSize())) {
                      SPDLOG_LOGGER_ERROR(logger_, "write truncated");
                      processError(wsi);
                      return -1;
                  }
                  client.sentCounter += 1;
               }
               requestWriteIfNeeded(client);
               return 0;
            }
            case State::WaitHandshake: {
               return 0;
            }
            case State::SendingHandshakeNotFound: {
               auto packet = Packet::responseUnknown();
               lws_write(wsi, packet.getPtr(), packet.getSize(), LWS_WRITE_BINARY);
               connection.state = State::Closed;
               return -1;
            }
            case State::SendingHandshakeResumed: {
               auto &client = clients_.at(connection.clientId);
               auto packet = Packet::responseResumed(client.recvCounter);
               int rc = lws_write(wsi, packet.getPtr(), packet.getSize(), LWS_WRITE_BINARY);
               if (rc == -1) {
                  SPDLOG_LOGGER_ERROR(logger_, "write failed");
                  processError(wsi);
                  return -1;
               }
               connection.state = State::Connected;
               lws_callback_on_writable(wsi);
               SPDLOG_LOGGER_DEBUG(logger_, "session resumed for client {}"
                  , convo::toHex(connection.clientId));
               return 0;
            }
            case State::SendingHandshakeNew: {
               auto cookie = generateNewCookie();
               auto packet = Packet::responseNew(cookie);
               int rc = lws_write(wsi, packet.getPtr(), packet.getSize(), LWS_WRITE_BINARY);
               if (rc == -1) {
                  SPDLOG_LOGGER_ERROR(logger_, "write failed");
                  processError(wsi);
                  return -1;
               }
               auto clientId = nextClientId();
               connection.state = State::Connected;
               cookieToClientIdMap_[cookie] = clientId;
               auto &client = clients_[clientId];
               client.cookie = cookie;
               client.wsi = wsi;
               connection.clientId = clientId;
               server::Details details;
               details[server::Detail::IpAddr] = connection.ipAddr;
               SPDLOG_LOGGER_DEBUG(logger_, "new session started for client {}"
                  , convo::toHex(clientId));
               const auto& listener = listener_.lock();
               if (listener) {
                  listener->onConnected(clientId, details);
               }
               lws_callback_on_writable(wsi);
               return 0;
            }
            case State::Closed: {
               return -1;
            }
         }
         break;
      }

      case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE: {
         uint16_t code;
         std::memcpy(&code, in, sizeof(code));
         code = htons(code);
         auto &connection = connections_.at(wsi);
         SPDLOG_LOGGER_DEBUG(logger_, "closing frame received with status code {}", code);
         switch (connection.state) {
            case State::Connected: {
               if (code == LWS_CLOSE_STATUS_NORMAL) {
                  connection.state = State::Closed;
                  closeConnectedClient(connection.clientId);
                  const auto& listener = listener_.lock();
                  if (listener) {
                     listener->onDisconnected(connection.clientId);
                  }
               }
               return -1;
            }
            default:
               break;
         }
         break;
      }

      default:
         break;
   }

   return 0;
}

convo::server::ClientId ServerConnection::nextClientId()
{
   return std::to_string(generateRandomClientId());
}

bool ServerConnection::done() const
{
   return shuttingDown_ && connections_.empty();
}

bool ServerConnection::writeNeeded(const ClientData &client) const
{
   if (client.wsi == nullptr) {
      return false;
   }
   return client.sentCounter != client.queuedCounter || client.recvCounter != client.recvAckCounter;
}

void ServerConnection::requestWriteIfNeeded(const ClientData &client)
{
   if (writeNeeded(client)) {
      lws_callback_on_writable(client.wsi);
   }
}

bool ServerConnection::processSentAck(ServerConnection::ClientData &client
   , uint64_t sentAckCounter)
{
   if (sentAckCounter < client.sentAckCounter || sentAckCounter > client.sentCounter) {
      SPDLOG_LOGGER_ERROR(logger_, "invalid ack value from client");
      return false;
   }

   while (client.sentAckCounter < sentAckCounter) {
      size_t count = client.allPackets.erase(client.sentAckCounter);
      assert(count == 1);
      client.sentAckCounter += 1;
   }

   return true;
}

void ServerConnection::processError(lws *wsi)
{
   auto &connection = connections_.at(wsi);
   switch (connection.state) {
      case State::SendingHandshakeResumed:
      case State::Connected: {
         auto &client = clients_.at(connection.clientId);
         assert(client.wsi == wsi);
         client.wsi = nullptr;
         break;
      }
      default: {
         assert(connection.clientId.empty());
         break;
      }
   }
   connection.state = State::Closed;
}

void ServerConnection::closeConnectedClient(const std::string &clientId)
{
   auto &client = clients_.at(clientId);
   auto count = cookieToClientIdMap_.erase(client.cookie);
   assert(count == 1);
   clients_.erase(clientId);
}

bool ServerConnection::sendTo(const server::ClientId &clientId, const std::string &data)
{
   DataToSend toSend{clientId, Packet::data(data)};
   {
      std::lock_guard<std::mutex> lock(mutex_);
      packets_.push(std::move(toSend));
   }
   lws_cancel_service(context_);
   return true;
}

bool ServerConnection::sendToAll(const std::string &data)
{
   return sendTo(kAllClientsId, data);
}

bool ServerConnection::timer(std::chrono::milliseconds timeout, ServerConnection::TimerCallback callback)
{
   if (!isActive()) {
      SPDLOG_LOGGER_ERROR(logger_, "can't start timer because server is not active");
      return false;
   }
   if (listenThread_.get_id() != std::this_thread::get_id()) {
      SPDLOG_LOGGER_ERROR(logger_, "starting timer from non-listening thread is not supported");
      return false;
   }
   timers_.scheduleCallback(context_, timeout, std::move(callback));
   return true;
}

bool ServerConnection::close(const server::ClientId &clientId)
{
   if (!isActive()) {
      return false;
   }
   std::lock_guard<std::mutex> lock(mutex_);
   forceClosingClients_.push(clientId);
   lws_cancel_service(context_);
   return true;
}

int ServerConnection::callbackHelper(lws *wsi, int reason, void *in, size_t len)
{
   auto context = lws_get_context(wsi);
   auto server = static_cast<ServerConnection*>(lws_context_user(context));
   return server->callback(wsi, reason, in, len);
}
#endif
