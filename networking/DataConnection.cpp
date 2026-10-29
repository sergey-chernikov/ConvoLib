#include "DataConnection.h"
#include <cassert>

using namespace convo;

void DataConnection::detachFromListener()
{
   listener_ = nullptr;
}

void DataConnection::notifyOnData(const std::string& data)
{
   if (listener_) {
      listener_->onDataReceived(data);
   }
}

void DataConnection::notifyOnConnected()
{
   if (listener_) {
      listener_->onConnected();
   }
}

void DataConnection::notifyOnDisconnected()
{
   if (listener_) {
      listener_->onDisconnected();
   }
}

void DataConnection::notifyOnError(DataConnectionListener::Error errorCode)
{
   if (listener_) {
      listener_->onError(errorCode);
   }
}
