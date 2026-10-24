#ifndef __CONVO_DATA_CONNECTION_LISTENER_H__
#define __CONVO_DATA_CONNECTION_LISTENER_H__

#include <stdexcept>
#include <string>
#include <vector>

namespace convo {

   class DataConnectionListener
   {
   public:
      enum class Error : uint8_t
      {
         NoError,
         UndefinedSocket,
         HostNotFound,
         HandshakeFailed,
         SerializationFailed,
         HeartbeatWaitFailed,
         ConnectionTimeout,
         ProtocolViolation,
      };

   public:
      DataConnectionListener() = default;
      virtual ~DataConnectionListener() noexcept = default;

      DataConnectionListener(const DataConnectionListener&) = delete;
      DataConnectionListener& operator = (const DataConnectionListener&) = delete;

      DataConnectionListener(DataConnectionListener&&) = delete;
      DataConnectionListener& operator = (DataConnectionListener&&) = delete;

   public:
      virtual void onDataReceived(const std::string& data) = 0;
      virtual void onConnected() = 0;
      virtual void onDisconnected() = 0;
      virtual void onError(const Error) = 0;
   };


   class DataTopicListener : public DataConnectionListener
   {
   public:
      DataTopicListener() = default;

      void onDataReceived(const std::string&) override final
      {
         throw std::runtime_error("not supported");
      }
      virtual void onDataReceived(const std::string& topic, const std::vector<std::string>& data) = 0;
   };

} // namespace convo
#endif // __CONVO_DATA_CONNECTION_LISTENER_H__
