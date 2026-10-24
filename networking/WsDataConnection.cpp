#include "WsDataConnection.h"
#if BUILD_LWS
#include <libwebsockets.h>
#endif
#include <openssl/ssl.h>
#include <openssl/pem.h>
#include <spdlog/spdlog.h>

using namespace convo::ws;

#if BUILD_LWS
namespace convo {
   namespace ws {
      const std::vector<uint32_t> kDefaultDelaysTableMs = { 10, 100, 200, 500, 3000, 10000 };

      int callback(lws* wsi, lws_callback_reasons reason, void* user, void* in, size_t len)
      {
         return DataConnection::callbackHelper(wsi, reason, user, in, len);
      }

      struct lws_protocols kProtocols[] = {
         { kProtocolName, callback, 0, kRxBufferSize, kId, nullptr, kTxPacketSize },
         { nullptr, nullptr, 0, 0, 0, nullptr, 0 },
      };

      struct TimerStruct : lws_sorted_usec_list_t
      {
         DataConnection* owner_;
      };

   }  //namespace ws
} // namespace convo

using namespace convo::ws;

DataConnection::DataConnection(const std::shared_ptr<spdlog::logger> &logger
   , DataConnectionParams params)
   : logger_(logger), params_(std::move(params)), reconnectTimer_(new TimerStruct)
{
   // lws will use default value of 30% for jitter
   retryTable_ = std::make_unique<lws_retry_bo>();
   std::memset(retryTable_.get(), 0, sizeof(lws_retry_bo));
   const auto &retryTable = params_.delaysTableMs.empty() ? kDefaultDelaysTableMs : params_.delaysTableMs;
   retryTable_->retry_ms_table = retryTable.data();
   retryTable_->retry_ms_table_count = static_cast<uint16_t>(retryTable.size());

   std::memset(reconnectTimer_.get(), 0, sizeof(*reconnectTimer_));
   reconnectTimer_->owner_ = this;
}

DataConnection::~DataConnection()
{
   close();
}

bool DataConnection::open(const std::string &host, const std::string &port
   , const std::shared_ptr<DataConnectionListener>& listener)
{
   close();

   listener_ = listener;
   host_ = host;
   port_ = std::stoi(port);
   shuttingDown_ = false;

   lws_context_creation_info info;
   memset(&info, 0, sizeof(info));

   info.port = CONTEXT_PORT_NO_LISTEN;
   info.protocols = kProtocols;
   info.gid = -1;
   info.uid = -1;
   info.retry_and_idle_policy = defaultRetryAndIdlePolicy();
   info.options = params_.useSsl ? LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT : 0;
   info.user = this;
   info.ssl_options_set = params_.useSsl ? ws::sslOptionsSet() : 0;
   info.timeout_secs = params_.timeoutSecs;

   context_ = lws_create_context(&info);
   if (!context_) {
      SPDLOG_LOGGER_ERROR(logger_, "context create failed");
      return false;
   }

   std::lock_guard<std::mutex> lock(mutex_);
   listenThread_ = std::thread(&DataConnection::listenFunction, this);

   return true;
}

bool DataConnection::close()
{
   if (!listenThread_.joinable()) {
      return false;
   }

   shuttingDown_ = true;
   lws_cancel_service(context_);

   listenThread_.join();

   lws_context_destroy(context_);

   context_ = nullptr;
   listener_.reset();
   newPackets_ = {};
   allPackets_ = {};
   currFragment_ = {};
   state_ = {};
   sentCounter_ = {};
   sentAckCounter_ = {};
   queuedCounter_ = {};
   recvCounter_ = {};
   recvAckCounter_ = {};
   cookie_ = {};
   std::memset(reconnectTimer_.get(), 0, sizeof(*reconnectTimer_));
   reconnectTimer_->owner_ = this;
   retryCounter_ = {};
   shuttingDownReceived_ = {};

   return true;
}

bool DataConnection::send(const std::string &data)
{
   if (!context_) {
      return false;
   }
   {
      std::lock_guard<std::mutex> lock(mutex_);
      newPackets_.push(filterRawPacket(Packet::data(data)));
   }
   lws_cancel_service(context_);
   return true;
}

bool DataConnection::isActive() const
{
   return context_ != nullptr;
}

bool DataConnection::timer(std::chrono::milliseconds timeout, DataConnection::TimerCallback callback)
{
   if (!isActive()) {
      SPDLOG_LOGGER_ERROR(logger_, "can't start timer because connection is not active");
      return false;
   }
   {
      // Fix for the TSAN report
      std::lock_guard<std::mutex> lock(mutex_);
      if (listenThread_.get_id() != std::this_thread::get_id()) {
         SPDLOG_LOGGER_ERROR(logger_, "starting timer from non-listening thread is not supported");
         return false;
      }
   }
   timers_.scheduleCallback(context_, timeout, std::move(callback));
   return true;
}

int DataConnection::callbackHelper(lws *wsi, int reason, void *user, void *in, size_t len)
{
   auto context = lws_get_context(wsi);
   auto client = static_cast<DataConnection*>(lws_context_user(context));
   return client->callback(wsi, reason, user, in, len);
}

int DataConnection::callback(lws *wsi, int reason, void *user, void *in, size_t len)
{
   switch (reason) {
      case LWS_CALLBACK_OPENSSL_LOAD_EXTRA_CLIENT_VERIFY_CERTS: {
         if (!params_.useSsl || params_.caBundleSize == 0) {
            return 0;
         }
         auto sslCtx = static_cast<SSL_CTX*>(user);
         auto store = SSL_CTX_get_cert_store(sslCtx);
         auto bio = BIO_new_mem_buf(params_.caBundlePtr, static_cast<int>(params_.caBundleSize));
         while (true) {
            auto x = PEM_read_bio_X509_AUX(bio, nullptr, nullptr, nullptr);
            if (!x) {
               break;
            }
            int rc = X509_STORE_add_cert(store, x);
            assert(rc);
            X509_free(x);
         }
         BIO_free(bio);
         break;
      }

      case LWS_CALLBACK_EVENT_WAIT_CANCELLED: {
         {
            std::lock_guard<std::mutex> lock(mutex_);
            while (!newPackets_.empty()) {
               allPackets_.insert(std::make_pair(queuedCounter_, std::move(newPackets_.front())));
               newPackets_.pop();
               queuedCounter_ += 1;
            }
         }
         if (!allPackets_.empty()) {
            if (state_ == State::Connected) {
               assert(wsi_);
               lws_callback_on_writable(wsi_);
            }
         }
         if (shuttingDown_.load() && !shuttingDownReceived_
             && ((wsi_ == nullptr) || (sentCounter_ == queuedCounter_) || (state_ != State::Connected))) {
            if (state_ == State::Connected && wsi_ != nullptr) {
               lws_close_reason(wsi_, LWS_CLOSE_STATUS_NORMAL, nullptr, 0);
               lws_set_timeout(wsi_, PENDING_TIMEOUT_USER_OK, LWS_TO_KILL_SYNC);
               state_ = State::Closing;
            } else {
               state_ = State::Closed;
            }
            shuttingDownReceived_ = true;
         }
         break;
      }

      case LWS_CALLBACK_CLIENT_RECEIVE: {
         if (wsi != wsi_) {
            return -1;
         }

         auto ptr = static_cast<const char*>(in);
         currFragment_.insert(currFragment_.end(), ptr, ptr + len);
         if (currFragment_.size() > params_.maximumPacketSize) {
            SPDLOG_LOGGER_ERROR(logger_, "maximum packet size reached");
            return -1;
         }
         if (lws_remaining_packet_payload(wsi) > 0) {
            return 0;
         }
         if (!lws_is_final_fragment(wsi)) {
            SPDLOG_LOGGER_ERROR(logger_, "unexpected fragment");
            processError();
            return -1;
         }
         //TODO: handle exceptions from parse
         auto packet = Packet::parse(currFragment_);
         currFragment_.clear();

         switch (state_) {
            case State::Connecting:
            case State::Reconnecting: {
               SPDLOG_LOGGER_CRITICAL(logger_, "unexpected message");
               assert(false);
               return -1;
            }
            case State::WaitingNewResponse: {
               if (packet.type != Packet::Type::ResponseNew || packet.payload.empty()) {
                  SPDLOG_LOGGER_ERROR(logger_, "invalid response");
                  processError();
                  return -1;
               }
               SPDLOG_LOGGER_DEBUG(logger_, "connected");
               //TODO: this blindly takes in the first packet as the session cookie, redo this.
               cookie_ = packet.payload; 
               state_ = State::Connected;
               const auto& listener = listener_.lock();
               if (listener) {
                  listener->onConnected();
               }
               requestWriteIfNeeded();
               break;
            }
            case State::WaitingResumedResponse: {
               if (packet.type == Packet::Type::ResponseUnknown) {
                  SPDLOG_LOGGER_ERROR(logger_, "server responds that connection is not known or invalid");
                  processFatalError();
                  return -1;
               }
               if (packet.type != Packet::Type::ResponseResumed) {
                  SPDLOG_LOGGER_ERROR(logger_, "invalid response");
                  processError();
                  return -1;
               }
               if (!processSentAck(packet.recvCounter)) {
                  SPDLOG_LOGGER_DEBUG(logger_, "connection resuming failed");
                  return -1;
               }
               SPDLOG_LOGGER_DEBUG(logger_, "connection resumed succesfully");
               state_ = State::Connected;
               retryCounter_ = 0;
               sentCounter_ = packet.recvCounter;
               requestWriteIfNeeded();
               break;
            }
            case State::Closing:
            case State::Connected: {
               switch (packet.type) {
                  case Packet::Type::Ack: {
                     if (!processSentAck(packet.recvCounter)) {
                        return -1;
                     }
                     break;
                  }
                  case Packet::Type::Data: {
                     const auto& listener = listener_.lock();
                     if (listener) {
                        listener->onDataReceived(packet.payload);
                     }
                     recvCounter_++;
                     break;
                  }
                  default: {
                     SPDLOG_LOGGER_ERROR(logger_, "unexpected packet");
                     processError();
                     return -1;
                  }
               }
               requestWriteIfNeeded();
               break;
            }
            case State::Closed: {
               return -1;
            }
         }

         break;
      }

      case LWS_CALLBACK_CLIENT_WRITEABLE: {
         if (wsi != wsi_) {
            return -1;
         }

         switch (state_) {
            case State::Reconnecting: {
               auto packet = filterRawPacket(Packet::requestResumed(cookie_, recvCounter_));
               int rc = lws_write(wsi, packet.getPtr(), packet.getSize(), LWS_WRITE_BINARY);
               if (rc == -1) {
                  SPDLOG_LOGGER_ERROR(logger_, "write failed");
                  processError();
                  return -1;
               }
               state_ = State::WaitingResumedResponse;
               break;
            }
            case State::Connecting: {
               auto packet = filterRawPacket(Packet::requestNew());
               int rc = lws_write(wsi, packet.getPtr(), packet.getSize(), LWS_WRITE_BINARY);
               if (rc == -1) {
                  SPDLOG_LOGGER_ERROR(logger_, "write failed");
                  processError();
                  return -1;
               }
               state_ = State::WaitingNewResponse;
               break;
            }
            case State::WaitingResumedResponse:
            case State::WaitingNewResponse: {
               // Nothing to do
               break;
            }
            case State::Closing:
            case State::Connected: {
               if (recvCounter_ != recvAckCounter_) {
                  auto packet = filterRawPacket(Packet::ack(recvCounter_));
                  int rc = lws_write(wsi, packet.getPtr(), packet.getSize(), LWS_WRITE_BINARY);
                  if (rc != static_cast<int>(packet.getSize())) {
                      SPDLOG_LOGGER_ERROR(logger_, "write failed");
                      processError();
                      return -1;
                  }
                  recvAckCounter_ = recvCounter_;
               } else if (sentCounter_ != queuedCounter_) {
                  // NOTE: Making packet copy here!
                  // LWS will mangle packet for WS masking purpose and thus packet won't be usable for retransmits after session resume.
                  auto packet = allPackets_.at(sentCounter_);
                  int rc = lws_write(wsi, packet.getPtr(), packet.getSize(), LWS_WRITE_BINARY);
                  if (rc == -1) {
                     SPDLOG_LOGGER_ERROR(logger_, "write failed");
                     processError();
                     return -1;
                  }
                  if (rc != static_cast<int>(packet.getSize())) {
                      SPDLOG_LOGGER_ERROR(logger_, "write truncated");
                      processError();
                      return -1;
                  }
                  sentCounter_ += 1;
               }
               requestWriteIfNeeded();
               break;
            }
            case State::Closed: {
               // Nothing to do
               break;
            }
         }
         break;
      }

      case LWS_CALLBACK_CLIENT_ESTABLISHED: {
         lws_callback_on_writable(wsi);
         break;
      }

      case LWS_CALLBACK_CLIENT_CLOSED: {
         if (wsi == wsi_) {
            processError();
         }
         return -1;
      }

      // LWS_CALLBACK_WSI_DESTROY is added to fix TestWebSocket.DISABLED_StressTest
      case LWS_CALLBACK_WSI_DESTROY:
      case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
         if (wsi == wsi_) {
            processError();
         }
         return -1;
      }

      case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE: {
         uint16_t code;
         std::memcpy(&code, in, sizeof(code));
         code = htons(code);
         SPDLOG_LOGGER_DEBUG(logger_, "closing frame received with status code {}", code);
         if (code == LWS_CLOSE_STATUS_NORMAL) {
            switch (state_) {
               case State::Connected:
               case State::WaitingResumedResponse:
               case State::Reconnecting:
               case State::Closing: {
                  const auto& listener = listener_.lock();
                  if (listener) {
                     listener->onDisconnected();
                  }
               }
                  break;
               case State::Connecting:
               case State::WaitingNewResponse:
               case State::Closed: {
                  const auto& listener = listener_.lock();
                  if (listener) {
                     listener->onError(DataConnectionListener::Error::UndefinedSocket);
                  }
               }
                  break;
            }
            state_ = State::Closed;
         }
         return -1;
      }
   }
   return 0;
}

void DataConnection::reconnectCallback(lws_sorted_usec_list *list)
{
   auto data = static_cast<TimerStruct*>(list);
   data->owner_->reconnect();
}

void DataConnection::listenFunction()
{
   reconnect();

   while (state_ != State::Closed) {
      lws_service(context_, 0);
   }

   wsi_ = nullptr;
}

void DataConnection::scheduleReconnect()
{
   auto nextDelayMs = lws_retry_get_delay_ms(context_, retryTable_.get(), &retryCounter_, nullptr);
   SPDLOG_LOGGER_DEBUG(logger_, "schedule reconnect in {} ms, retry counter: {}", nextDelayMs, retryCounter_);
   lws_sul_schedule(context_, 0, reconnectTimer_.get(), reconnectCallback, static_cast<lws_usec_t>(nextDelayMs) * 1000);
}

void DataConnection::reconnect()
{
   SPDLOG_LOGGER_DEBUG(logger_, "try connect");
   assert(wsi_ == nullptr);

   struct lws_client_connect_info i;
   memset(&i, 0, sizeof(i));
   i.address = host_.c_str();
   i.host = i.address;
   i.port = port_;
   i.origin = i.address;
   i.path = "/";
   i.context = context_;
   i.protocol = kProtocolName;
   i.userdata = this;
   i.ssl_connection = params_.useSsl ? LCCSCF_USE_SSL : 0;

   wsi_ = lws_client_connect_via_info(&i);
}

void DataConnection::processError()
{
   wsi_ = nullptr;

   if (retryCounter_ >= retryTable_->retry_ms_table_count) {
      SPDLOG_LOGGER_ERROR(logger_, "too many reconnect retries failed");
      processFatalError();
      return;
   }

   switch (state_) {
      case State::WaitingNewResponse:
      case State::Connecting: {
         state_ = State::Connecting;
         scheduleReconnect();
         break;
      }

      case State::Reconnecting:
      case State::Connected:
      case State::WaitingResumedResponse: {
         state_ = State::Reconnecting;
         scheduleReconnect();
         break;
      }
      case State::Closing: {
         const auto& listener = listener_.lock();
         if (listener) {
            listener->onDisconnected();
         }
         state_ = State::Closed;
         break;
      }
      case State::Closed: {
         break;
      }
   }
}

void DataConnection::processFatalError()
{
   const auto& listener = listener_.lock();
   if (listener) {
      switch (state_) {
      case State::Connecting:
      case State::WaitingNewResponse:
         listener->onError(DataConnectionListener::Error::UndefinedSocket);
         break;

      case State::Closing:
         listener->onDisconnected();
         break;

      case State::Reconnecting:
      case State::Connected:
      case State::WaitingResumedResponse:
         listener->onDisconnected();
         listener->onError(DataConnectionListener::Error::UndefinedSocket);
         break;

      case State::Closed:
         break;
      }
   }
   state_ = State::Closed;
   wsi_ = nullptr;
}

bool DataConnection::writeNeeded() const
{
   switch (state_) {
      case State::Connected:
         return sentCounter_ != queuedCounter_ || recvCounter_ != recvAckCounter_;
      case State::Connecting:
      case State::Reconnecting:
      case State::WaitingNewResponse:
      case State::WaitingResumedResponse:
      case State::Closed:
      case State::Closing:
         return false;
   }
   assert(false);
   return false;
}

void DataConnection::requestWriteIfNeeded()
{
   if (writeNeeded() && wsi_ != nullptr) {
      lws_callback_on_writable(wsi_);
   }
   if (shuttingDown_) {
      lws_cancel_service(context_);
   }
}

bool DataConnection::processSentAck(uint64_t sentAckCounter)
{
   if (sentAckCounter < sentAckCounter_ || sentAckCounter > sentCounter_) {
      SPDLOG_LOGGER_ERROR(logger_, "invalid ack value from server");
      processFatalError();
      return false;
   }

   while (sentAckCounter_ < sentAckCounter) {
      size_t count = allPackets_.erase(sentAckCounter_);
      assert(count == 1);
      sentAckCounter_ += 1;
   }

   return true;
}

RawPacket DataConnection::filterRawPacket(const RawPacket& packet) const
{
   return packet;
}
#endif   //BUILD_LWS
