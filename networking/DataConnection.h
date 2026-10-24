#ifndef __CONVO_DATA_CONNECTION_H__
#define __CONVO_DATA_CONNECTION_H__

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "DataConnectionListener.h"

namespace convo {

   class DataConnection
   {
   public:
      DataConnection(const std::shared_ptr<DataConnectionListener>& listener = nullptr)
         : listener_(listener)  {}
      virtual ~DataConnection() noexcept = default;

      DataConnection(const DataConnection&) = delete;
      DataConnection& operator = (const DataConnection&) = delete;

      DataConnection(DataConnection&&) = delete;
      DataConnection& operator = (DataConnection&&) = delete;

   public:
      virtual bool send(const std::string& data) = 0;

      virtual bool open(const std::string& host, const std::string& port
         , const std::shared_ptr<DataConnectionListener>& listener) = 0;
      virtual bool close() = 0;

      virtual bool isActive() const = 0;

      // Execute callback after timeout on listening thread
      using TimerCallback = std::function<void()>;
      virtual bool timer(std::chrono::milliseconds /*timeout*/, TimerCallback) { return false; }

   protected:
      void setListener(const std::shared_ptr<DataConnectionListener>& listener)
      {
         listener_ = listener;
      }

      bool haveListener() const { return (listener_.lock() != nullptr); }
      void detachFromListener();

      virtual void onRawDataReceived(const std::string& /*rawData*/) {}

      void notifyOnData(const std::string& data);
      virtual void notifyOnConnected();
      void notifyOnDisconnected();
      void notifyOnError(const DataConnectionListener::Error);

   protected:
      std::weak_ptr<DataConnectionListener> listener_;
   };

}

#endif // __CONVO_DATA_CONNECTION_H__
