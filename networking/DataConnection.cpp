#include "DataConnection.h"
#include <cassert>

using namespace convo;

void DataConnection::detachFromListener()
{
   listener_.reset();
}

void DataConnection::notifyOnData(const std::string& data)
{
   const auto listener = listener_.lock();
   if (listener) {
      listener->onDataReceived(data);
   }
}

void DataConnection::notifyOnConnected()
{
   const auto listener = listener_.lock();
   if (listener) {
      listener->onConnected();
   }
}

void DataConnection::notifyOnDisconnected()
{
   const auto listener = listener_.lock();
   if (listener) {
      listener->onDisconnected();
   }
}

void DataConnection::notifyOnError(DataConnectionListener::Error errorCode)
{
   const auto listener = listener_.lock();
   if (listener) {
      listener->onError(errorCode);
   }
}
