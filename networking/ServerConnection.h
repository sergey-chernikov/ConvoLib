#ifndef __CONVO_SERVER_CONNECTION_H__
#define __CONVO_SERVER_CONNECTION_H__

#include "ServerConnectionListener.h"
#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace convo {
   namespace server {

      class Connection
      {
      public:
         Connection() = default;
         virtual ~Connection() noexcept = default;

         Connection(const Connection&) = delete;
         Connection& operator = (const Connection&) = delete;
         Connection(Connection&&) = delete;
         Connection& operator = (Connection&&) = delete;

         virtual bool bind(const std::string& host, const std::string& port
            , const std::shared_ptr<ConnectionListener>&) = 0;

         virtual bool sendTo(const ClientId&, const std::string& data) = 0;
         virtual bool sendToAll(const std::string&) { return false; }

         // Execute callback after timeout on listening thread
         using TimerCallback = std::function<void()>;
         virtual bool timer(std::chrono::milliseconds /*timeout*/, TimerCallback /*callback*/) { return false; }

         // Close client connection
         virtual bool close(const ClientId&) { return false; }
      };

   }
}

#endif // __CONVO_SERVER_CONNECTION_H__
