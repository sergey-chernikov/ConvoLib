#include "MessageBus/Adapter.h"
#include "MessageBus/Bus.h"
#ifdef CONVO_BUILD_PROTO
#include "ProtoUtils.h"
#endif

#define TRACK_MSG_ORIGIN

using namespace convo::msg;

bool Adapter::pushFill(Envelope &env)
{
   if (!env.processingQueue && !queue_) {
      return false;
   }
   if (env.origin.empty()) {
      env.origin = name();
   }
#ifdef TRACK_MSG_ORIGIN
   if (!isSupervisor()) {
      const auto& supportedUsers = supportedReceivers(); 
      const bool isOwnSender = env.sender->isSystem() ? true
         : (supportedUsers.find(env.sender) != supportedUsers.end());
      if (!isOwnSender) {
         reportInvalidSender(env.sender, env.foreignId());
         return false;
      }
   }
#endif
   const auto& q = env.processingQueue ? env.processingQueue : queue_.get();
   return q->pushFill(env);
}

SeqId Adapter::pushRequest(const std::shared_ptr<User>& sender
   , const std::shared_ptr<User>& receiver, const std::string& msg, ContentType ct
   , const TimeStamp& execAt, unsigned rptCount, const bus_clock::duration& rptInterval)
{
   auto env = Envelope::makeRequest(sender, receiver, msg, ct, execAt, rptCount, rptInterval);
   if (pushFill(env)) {
      return env.foreignId();
   }
   return 0;
}

SeqId Adapter::pushTimeoutRequest(const std::shared_ptr<User>& sender
   , const std::shared_ptr<User>& receiver, const std::string& msg
   , const bus_clock::duration& rptInterval, ContentType ct
   , unsigned rptCount)
{
   auto env = Envelope::makeRequest(sender, receiver, msg, ct, {}, rptCount, rptInterval);
   if (pushFill(env)) {
      return env.foreignId();
   }
   return 0;
}

SeqId Adapter::pushResponse(const std::shared_ptr<User>& sender
   , const std::shared_ptr<User>& receiver, const std::string& msg
   , ContentType ct, SeqId respId)
{
   auto env = Envelope::makeResponse(sender, receiver, msg, respId, ct);
   if (pushFill(env)) {
      return env.foreignId();
   }
   return 0;
}

SeqId Adapter::pushResponse(const std::shared_ptr<User>& sender
   , const Envelope& envReq, const std::string& msg
   , ContentType ct, unsigned rptCount, const bus_clock::duration& rptInterval)
{
   auto env = Envelope::makeResponse(sender, envReq.sender, msg, envReq.foreignId()
      , ct, rptCount, rptInterval);
   env.processingQueue = envReq.processingQueue;
   env.routingKey = envReq.routingKey;
   if (envReq.replyTo) {
      env.sender = envReq.sender;
      env.receiver = envReq.replyTo;
      env.replyTo = sender;
   }
   if (pushFill(env)) {
      return env.foreignId();
   }
   return 0;
}

SeqId Adapter::pushUpdate(const std::shared_ptr<User>& sender
   , const std::shared_ptr<User>& receiver, const std::string& msg, ContentType ct
   , SeqId respId)
{
   return pushResponse(sender, receiver, msg, ct, respId);
}

SeqId Adapter::pushBroadcast(const std::shared_ptr<User>& sender
   , const std::string& msg, bool global, ContentType ct)
{
   auto env = Envelope::makeBroadcast(sender, msg, ct, global);
   if (pushFill(env)) {
      return env.foreignId();
   }
   return 0;
}

#ifdef BUILD_CAPNP
SeqId Adapter::pushRequest(const std::shared_ptr<User>& sender
   , const std::shared_ptr<User>& receiver, capnp::MessageBuilder& msg
   , const TimeStamp& execAt, unsigned rptCount, const bus_clock::duration& rptInterval)
{
   return pushRequest(sender, receiver, capn::serialize()(msg), ContentType::capnp
      , execAt, rptCount, rptInterval);
}

SeqId Adapter::pushUpdate(const std::shared_ptr<User>& sender
   , const std::shared_ptr<User>& receiver, capnp::MessageBuilder& msg, SeqId respId)
{
   return pushResponse(sender, receiver, capn::serialize()(msg), ContentType::capnp, respId);
}

SeqId Adapter::pushResponse(const std::shared_ptr<User>& sender, const Envelope& envReq
   , capnp::MessageBuilder& msg, unsigned rptCount, const bus_clock::duration& rptInterval)
{
   return pushResponse(sender, envReq, capn::serialize()(msg), ContentType::capnp
      , rptCount, rptInterval);
}

SeqId Adapter::pushBroadcast(const std::shared_ptr<User>& sender
   , capnp::MessageBuilder& msg, bool global)
{
   return pushBroadcast(sender, capn::serialize()(msg), global, ContentType::capnp);
}
#endif   //DISABLE_CAPNP


void SharedAdapter::setQueue(const std::shared_ptr<QueueInterface>& queue)
{
   if (!queue_) {
      queue_ = queue;
   }
   queues_.push_back(queue);
}

bool SharedAdapter::pushFill(Envelope& env)
{
   if (env.origin.empty()) {
      env.origin = name();
   }
#ifdef TRACK_MSG_ORIGIN
   if (!isSupervisor()) {
      const auto& supportedUsers = supportedReceivers();
      const bool isOwnSender = env.sender->isSystem() ? true
         : (supportedUsers.find(env.sender) != supportedUsers.end());
      if (!isOwnSender) {
         reportInvalidSender(env.sender, env.foreignId());
         return false;
      }
   }
#endif

   bool rc = true;
   for (const auto& queue : queues_) {
      env.setId(0);
      rc &= queue->pushFill(env);
   }
   return rc;
}


convo::ProcessingResult PipeAdapter::process(const Envelope &env)
{
   if (!endpoint_) {
      return ProcessingResult::Error;
   }
   auto envCopy = env;
   return endpoint_->pushFill(envCopy) ? ProcessingResult::Success : ProcessingResult::Retry;
}

bool PipeAdapter::processBroadcast(const Envelope& env)
{
   if (!endpoint_) {
      return false;
   }
   auto envCopy = env;
   return endpoint_->pushFill(envCopy);
}


bool RelayAdapter::isInitialized() const
{
   return (fallbackUser_ != nullptr);
}

Adapter::Users RelayAdapter::supportedReceivers() const
{
   if (!isInitialized()) {
      throw std::runtime_error("invalid initialization");
   }
   return { fallbackUser_ };
}

void RelayAdapter::setQueue(const std::shared_ptr<QueueInterface>& queue)
{
   Adapter::setQueue(queue);
   queues_.insert(queue);
   const auto& ownReceivers = supportedReceivers();
   for (const auto& user : queue->supportedReceivers()) {
      if (std::find_if(ownReceivers.cbegin(), ownReceivers.cend()
         , [user](const std::shared_ptr<User>& u) { return (u->value() == user); }) != ownReceivers.end()) {
         continue;
      }
      queueByUser_[user] = queue;
   }
}

convo::ProcessingResult RelayAdapter::process(const Envelope& env)
{
   if (!isInitialized()) {
      throw std::runtime_error("invalid initialization");
   }
   return relay(env);
}

bool RelayAdapter::processBroadcast(const Envelope& env)
{
   if (env.relayed) {
      return false;
   }
   if (!isInitialized()) {
      throw std::runtime_error("invalid initialization");
   }
   if ((env.id() != env.foreignId()) && (env.envelopeType() == EnvelopeType::GlobalBroadcast)) {
      return false;  // global broadcasts are processed elsewhere (external relayer like AMQP)
   }

   auto envCopy = env;
   envCopy.relayed = true;

   for (const auto& queue : queues_) {
      if (currentQueue_ == queue->id()) {
         continue;
      }
      envCopy.setId(0);
      queue->pushFill(envCopy);
   }
   return true;
}

convo::ProcessingResult RelayAdapter::relay(const Envelope& env)
{
   if (env.isBC() || env.relayed) {
      return ProcessingResult::Ignored;
   }
   auto envCopy = env;
   envCopy.relayed = true;

   if (env.receiver->isSystem()) {  // push ->system message to all queues
      for (const auto& queue : queues_) {
         if (currentQueue_ == queue->id()) {
            continue;
         }
         envCopy.setId(0);
         queue->pushFill(envCopy);
      }
      return ProcessingResult::Success;
   }

   const auto& itQueue = queueByUser_.find(env.receiver->value());
   if (itQueue == queueByUser_.end()) {
      if (--envCopy.repeatCount == 0) {
         return ProcessingResult::TimedOut;
      }
      return ProcessingResult::Retry;
   }
   envCopy.setId(0);
   return itQueue->second->pushFill(envCopy) ? ProcessingResult::Success
      : ProcessingResult::Error;
}

bool RelayAdapter::pushFill(Envelope& env)
{
   if (!queue_) {
      return false;
   }
   return queue_->pushFill(env);
}


bool operator==(const std::shared_ptr<User>& a, const std::shared_ptr<User>& b)
{
   if (!a && !b) {
      return true;
   }
   if (!a || !b) {
      return false;
   }
   return (a->value() == b->value());
}

bool operator<(const std::shared_ptr<User>& a, const std::shared_ptr<User>& b)
{
   if (a && b) {
      return (a->value() < b->value());
   }
   return false;
}

namespace convo {
   namespace msg {
      ContentType fromString(const std::string& s)
      {
         if (s.find("application/capn") == 0) {
            return ContentType::capnp;
         }
         if (s == "application/protobuf") {
            return ContentType::protobuf;
         }
         if (s == "text/plain") {
            return ContentType::text;
         }
         if (s == "application/json") {
            return ContentType::json;
         }
         return ContentType::unknown;
      }
   }
}
