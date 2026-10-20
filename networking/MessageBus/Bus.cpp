#include <chrono>
#include <random>
#include "MessageBus/Bus.h"
#include <spdlog/spdlog.h>
#include "MessageBus/Adapter.h"
#include "base/StringUtils.h"

//#define MSG_DEBUGGING   // comment this out to disable queue message logging
using namespace convo::msg;

static const std::string kQuitMessage("QUIT");
static const std::string kStopMessage("STOP");
static const std::string kAccResetMessage("ACC_RESET");
static std::mt19937 RNG{ (unsigned)std::chrono::duration_cast<std::chrono::nanoseconds>
      (std::chrono::steady_clock::now().time_since_epoch()).count() };

Router::Router(const std::shared_ptr<spdlog::logger> &logger)
   : logger_(logger)
{}

void Router::bindAdapter(const std::shared_ptr<Adapter> &adapter)
{
   if (!adapter) {
      throw std::runtime_error("invalid null adapter");
   }
   const auto &supportedReceivers = adapter->supportedReceivers();
   if (supportedReceivers.empty() && !adapter->isSupervisor()) {
      return logger_->error("[Router::bindAdapter] {} has no supported receivers", adapter->name());
   }
   const auto it = std::find(allAdapters_.cbegin(), allAdapters_.cend(), adapter);
   if (it == allAdapters_.cend()) {
      allAdapters_.push_back(adapter);
   }
   else {
      return logger_->warn("[Router::bindAdapter] {} was already added - skipping"
         , adapter->name());
   }
   if (adapter->isSupervisor()) {
      supervisor_ = adapter;
      return;
   }

   for (const auto &receiver : supportedReceivers) {
      if (!receiver) {
         logger_->error("[Router::bindAdapter] {} has null receiver", adapter->name());
         continue;
      }
      std::lock_guard<std::mutex> lock(mutex_);
      if (receiver->isFallback()) {
         defaultRoute_ = adapter;
         continue;
      }
      adapters_[receiver->value()] = adapter;
   }
}

std::set<UserValue> Router::supportedReceivers() const
{
   std::set<UserValue> result;
   for (const auto &adapter : adapters_) {
      result.insert(adapter.first);
   }
   return result;
}

bool Router::isDefaultRouted(const Envelope &env) const
{
   if (env.receiver->isFallback()) {
      return true;
   }
   auto itAdapter = adapters_.find(env.receiver->value());
   if (itAdapter == adapters_.end()) {
      if (env.sender->isFallback()) {
         logger_->warn("[Router::process] failed to find route for {} "
            "(from {}) - dropping message #{}", env.receiver->name()
            , env.sender->name(), env.foreignId());
         return false;
      }
   } else {
      return false;
   }
   return true;
}

std::vector<std::shared_ptr<Adapter>> Router::process(const Envelope &env) const
{
   if (supervisor_ && (supervisor_->process(env) == ProcessingResult::Captured)) {
      logger_->debug("[Router::process] msg #{} seized by supervisor", env.id());
      return {};
   }
   if (env.sender->isSystem() && env.receiver && env.receiver->isSystem()) {
      return {};  // skip system messages - they're only passed to supervisor
   }
   std::set<std::shared_ptr<Adapter>> result;
   if (env.isBC()) {
      std::lock_guard<std::mutex> lock(mutex_);
      for (const auto &adapter : adapters_) {
         if (!env.sender->isSystem() && (adapter.first == env.sender->value())) {
#ifdef MSG_DEBUGGING
            logger_->debug("[{}] {}/{} skipped", __func__, adapter.second->name()
               , env.sender->name());
#endif
            continue;
         }
         result.insert(adapter.second);
      }
      if (defaultRoute_ && !env.sender->isFallback()) {
         result.insert(defaultRoute_);
#ifdef MSG_DEBUGGING
         logger_->debug("[{}] defRoute {} added", __func__, defaultRoute_->name());
#endif
      }
   } else {
      if (isDefaultRouted(env)) {
         if (defaultRoute_) {
            return { defaultRoute_ };
         }
         else {
            //throw std::runtime_error("no default route");
            return {};
         }
      }
      else {
         return { obtainAdapterBy(env.receiver) };
      }
   }
   if (result.empty()) {
      if (env.isBC()) {
         return {};
      }
      throw std::runtime_error("no destination found");
   }
#ifdef MSG_DEBUGGING
   else {
      logger_->debug("[{}] #{}: {} adapter[s]", __func__, env.foreignId(), result.size());
   }
#endif
   return { result.cbegin(), result.cend() };
}

std::shared_ptr<Adapter> Router::obtainAdapterBy(const std::shared_ptr<User>& user) const
{
   if (!user) {
      throw std::runtime_error("null user");
   }
   try {
      return adapters_.at(user->value());
   }
   catch (const std::exception &) {
      throw std::runtime_error(user->name() + " not found");
   }
   return nullptr;
}

void Router::reset()
{
   supervisor_.reset();
   adapters_.clear();
}

void Router::reportQueueStats(const std::string& queueName
   , const PerfAccounting& acc)
{
   if (defaultRoute_) {
      defaultRoute_->reportProcessingStats(queueName, acc);
   }
}

bool Router::isDefaultRoute(const std::shared_ptr<Adapter>& a) const
{
   return (a == defaultRoute_);
}


QueueInterface::QueueInterface(const std::shared_ptr<RouterInterface>& router, const std::string& name)
   : router_(router), id_(RNG()), name_(name)
{}

SeqId QueueInterface::resetId(SeqId newId)
{
   if (seqNo_ < newId) {
      seqNo_ = newId;
   }
   return seqNo_;
}

bool QueueInterface::accept(const Envelope& env)
{
   if (!env.sender) {
      return false;
   }
   const auto& itDeferred = deferredIds_.find(env.id());
   if (itDeferred != deferredIds_.end()) {
      deferredIds_.erase(itDeferred);
      return true;
   }
   return (env.id() >= lastProcessedSeqNo_);
}

void QueueInterface::stop()
{
   static const auto userSys = std::make_shared<UserSystem>();
   auto envQuit = Envelope::makeRequest(userSys, userSys, kStopMessage
      , ContentType::text);
   pushFill(envQuit);
}


Queue_Locking::Queue_Locking(const std::shared_ptr<RouterInterface> &router
   , const std::shared_ptr<spdlog::logger> &logger, const std::string &name
   , const size_t batchLimit, const std::chrono::seconds& accInterval)
   : QueueInterface(router, name), logger_(logger)
   , batchLimit_(batchLimit), accountingInterval_(accInterval)
{
   thread_ = std::jthread(&Queue::process, this);
}

Queue_Locking::~Queue_Locking()
{
   terminate();
}

void Queue_Locking::terminate()
{
   stop();
   joinThread();
   router_->reset();
}

bool Queue_Locking::pushFill(Envelope &env)
{
   if (env.posted.time_since_epoch().count() == 0) {
      env.posted = bus_clock::now();
   }
   {
      std::unique_lock<std::mutex> lock(dataMutex_);
      env.setIdIfUnset(nextId());
      queue_.push_back(env);
   }

#ifdef MSG_DEBUGGING
   std::string msgBody;
   for (const char c : env.message) {
      if ((c < 32) || (c > 126)) {
         break;
      }
      msgBody += c;
      if (msgBody.length() >= 8) {
         break;
      }
   }
   if (msgBody.empty() && !env.message.empty()) {
      msgBody = bs::toHex(env.message.substr(0, 8));
      if (env.message.size() > 8) {
         msgBody += "...";
      }
   }
   logger_->debug("[Queue::push] {}: #{}/{} {}({}) -> {}({}) r#{} retries {}\n"
      "[{}] {}", name_, idOf(env), env.foreignId()
      , env.sender->name(), env.sender->value()
      , env.receiver ? env.receiver->name() : "null"
      , env.receiver ? env.receiver->value() : 0, env.rawResponseId(), env.repeatCount
      , env.message.size(), msgBody.empty() ? msgBody : "'" + msgBody + "'");
#endif   //MSG_DEBUGGING

   if (env.executeAt <= env.posted) {
      std::unique_lock<std::mutex> lock(cvMutex_);
      cvQueue_.notify_one();
   }
   return true;
}

void Queue_Locking::joinThread()
{
   if (thread_.joinable()) {
      thread_.join();
   }
}

void Queue_Locking::process()
{
   std::srand((unsigned int)std::time(nullptr));    // requred for per-thread randomness
   logger_->debug("[Queue::process] {} #{} started", name_, id());
   decltype(queue_) deferredQueue;
   auto dqTime = bus_clock::now();
   auto accTime = bus_clock::now();
   PerfAccounting acc;

   const auto &processPortion = [this, &deferredQueue, &dqTime, &accTime, &acc]
      (const decltype(queue_) &tempQueue, const bus_clock::time_point &timeNow) -> size_t
   {
      size_t nbProcessed = 0;
      for (auto env : tempQueue) {
         if (!accept(env)) {
            logger_->warn("[Queue::process] {}: envelope #{} failed to pass "
               "validity checks (< {}) - skipping", name_, idOf(env), lastProcessedSeqNo_);
            continue;
         }
         std::vector<std::shared_ptr<Adapter>> adapters;
         try {
            adapters = router_->process(env);
         }
         catch (const std::exception& e) {
            logger_->error("[Queue::process] {}: #{} routing failure for {}: {}"
               , name_, idOf(env), env.receiver ? env.receiver->name() : "null", e.what());
            continue;
         }

         if (env.executeAt.time_since_epoch().count() != 0) {
            if ((adapters.size() == 1) && router_->isDefaultRoute(adapters.at(0))) {
               //relay immediately
            }
            else if (env.executeAt > timeNow) {
               defer(env);
               deferredQueue.push_back(env);
               //acc.addQueueTime(std::chrono::duration_cast<std::chrono::microseconds>(timeNow - env.posted), -2);
               continue;
            }
         }

         env.processingQueue = this;
         if (env.receiver && env.sender->isSystem() && env.receiver->isSystem()) {
            if (env.contentType == ContentType::text) {
               if ((env.message == kQuitMessage) || (env.message == kStopMessage)) {
                  logger_->info("[Queue::process::{}] detected {} system message #{}"
                     , name_, env.message, env.foreignId());
                  unsigned nbAdapters = 0;
                  for (const auto& adapter : router_->allAdapters()) {
                     if (router_->isDefaultRoute(adapter)) { continue; }   //maybe this is not needed
                     if (adapter->processBroadcast(env)) { nbAdapters++; }
                  }
                  logger_->debug("[Queue::process::{}] {} processed in {} adapter[s]"
                     , name_, env.message, nbAdapters);
                  running_ = false;
                  break;
               }
               else if (env.message == kAccResetMessage) {
                  acc.reset();
                  continue;
               }
               else {
                  logger_->warn("[Queue::process] {} unknown system message {} - skipping"
                     , name_, env.message);
               }
            }
         } else {
            ++nbProcessed;
            const bool deferred = (deferredIds_.find(idOf(env)) != deferredIds_.end());
            const auto& process = [this, env, timeNow, &acc, &deferredQueue, deferred]
               (const std::shared_ptr<Adapter>&adapter)
            {
               const auto& supportedContentTypes = adapter->supportedContent();
               const bool ctSupported = supportedContentTypes.empty() ? true
                  : (supportedContentTypes.find(env.contentType) != supportedContentTypes.end());
               if (!ctSupported) {
                  if (!env.isBC()) {
                     logger_->error("[Queue::process::{}] content type {} from {}"
                        " is not supported by {} (#{}) [{}] {}", name_, (int)env.contentType
                        , env.sender->name(), adapter->name(), env.foreignId()
                        , env.message.size(), convo::toHex(env.message));
                  }
                  return;
               }
               adapter->setCurrentQueue(id());
               const auto& procStart = bus_clock::now();
               if (env.isBC()) {
                  if (adapter->processBroadcast(env)) {
                     acc.add(static_cast<int>((*adapter->supportedReceivers().cbegin())->value() + 0x1000)
                        , std::chrono::duration_cast<std::chrono::microseconds>(bus_clock::now() - procStart));
                  }
                  acc.addQueueTime(std::chrono::duration_cast<std::chrono::microseconds>(timeNow - env.posted));
               }
               else {
                  if (!env.executeAt.time_since_epoch().count()) {
                     const auto queueDiff = std::chrono::duration_cast<std::chrono::milliseconds>
                        (timeNow - env.posted).count();
                     if ((queueDiff > 0) && (queueDiff > long(batchLimit_ * 2))) {
                        const auto& logMsg = fmt::format("[Queue::process] {}/{}:"
                           " #{} from {} to {} residing for {}s\n{}"
                           , name_, id(), env.foreignId(), env.sender->name()
                           , env.receiver ? env.receiver->name() : "null"
                           , queueDiff / 1000.0, convo::toHex(env.message));
                        if (queueDiff > long(batchLimit_ * 6)) {
                           logger_->critical(logMsg);
                        }
                        else {
                           logger_->debug(logMsg);
                        }
                     }
                  }
                  const auto processResult = adapter->process(env);
                  const auto& busTimeAfterProc = bus_clock::now();
                  switch (processResult) {
                  case ProcessingResult::Success:
                     if (env.isRequest()) {
                        if (env.repeatInterval.count()) {   // watch only requests with repeat interval set
                           auto envCopy = env;
                           envCopy.repeatCount = 0;
                           if (env.repeatCount > 1) {
                              envCopy.repeatInterval *= env.repeatCount;
                           }
                           watchResponseTimeout(envCopy);
                        }
                     }
                     else if (env.responseId() < (SeqId)EnvelopeType::MaxValue) {
                        unwatchResponseTimeout(env.responseId());
                     }
                     acc.addQueueTime(std::chrono::duration_cast<std::chrono::microseconds>(timeNow - env.posted));
                     acc.add(static_cast<int>(env.receiver ? env.receiver->value() : 0)
                        , std::chrono::duration_cast<std::chrono::microseconds>(busTimeAfterProc - procStart));
                     break;

                  case ProcessingResult::Retry:
                     if (env.repeatCount > 0) {
                        const auto& result = defer(env);
                        if (result.second) { // avoid duplicates
                           auto envCopy = env;
                           envCopy.repeatCount--;
                           if (env.isRequest() && !envCopy.repeatCount && env.repeatInterval.count()) {
                              watchResponseTimeout(envCopy);
                           }
                           if (env.repeatInterval.count()) {
                              envCopy.executeAt = busTimeAfterProc + env.repeatInterval;
                           }
                           deferredQueue.emplace_back(std::move(envCopy));
                           //acc.addQueueTime(std::chrono::duration_cast<std::chrono::microseconds>(timeNow - env.posted), -3);
                        }
                     }
                     else if (deferred) {
                        logger_->warn("[Queue::process::{}] request #{} from {} timed out on retry"
                           , name_, env.foreignId(), env.sender->name());
                        adapter->processTimeout(env);
                        unwatchResponseTimeout(env.foreignId());
                     }
                     break;

                  case ProcessingResult::TimedOut:
                     adapter->processTimeout(env);
                     acc.addQueueTime(std::chrono::duration_cast<std::chrono::microseconds>(timeNow - env.posted));
                     acc.add(static_cast<int>(env.receiver ? env.receiver->value() : 0)
                        , std::chrono::duration_cast<std::chrono::microseconds>(busTimeAfterProc - procStart));
                     break;

                  case ProcessingResult::Error:    //TODO: increment error counter if needed
                     if (!env.isRequest() && (env.responseId() < (SeqId)EnvelopeType::MaxValue)) {
                        unwatchResponseTimeout(env.responseId());  // failed responses should send normal reply to requester
                     }
#ifdef MSG_DEBUGGING
                     logger_->info("[Queue::process] {} error processing #{} "
                        "from {} to {} in {} - skipping", name_, idOf(env)
                        , env.sender->name(), env.receiver ? env.receiver->name() : "null"
                        , adapter->name());
#endif
                     acc.addQueueTime(std::chrono::duration_cast<std::chrono::microseconds>(timeNow - env.posted));
                     acc.add(static_cast<int>(env.receiver ? env.receiver->value() : 0)
                        , std::chrono::duration_cast<std::chrono::microseconds>(busTimeAfterProc - procStart));
                     break;

                  case ProcessingResult::Ignored:
                     if (env.isRequest()) {
                        if (env.repeatInterval.count()) {   // watch only requests with repeat interval set
                           watchResponseTimeout(env);
                        }
                     }
                     acc.addQueueTime(std::chrono::duration_cast<std::chrono::microseconds>(timeNow - env.posted));
                     break;

                  default:
                     logger_->error("[Queue::process::{}] unknown processing result {} by {} for #{}"
                        , name_, (int)processResult, adapter->name(), env.foreignId());
                     break;
                  }
               }
            };
            if (running_.load()) {
               if (adapters.empty()) {
                  continue;   // empty result is intended for skipping a message silently (e.g. by supervisor)
               }
               std::shared_ptr<Adapter> curAdapter;
               try {
                  for (const auto& adapter : adapters) {
                     curAdapter = adapter;
#ifdef MSG_DEBUGGING
                     logger_->debug("[Queue::process] {}: #{}/{} r#{} {}->{} by {}"
                        , name_, idOf(env), env.foreignId(), env.rawResponseId()
                        , env.sender->name(), env.receiver ? env.receiver->name() : "*"
                        , adapter->name());
#endif
                     process(adapter);
                  }
               } catch (const std::exception& e) {
                  logger_->error("[Queue::process::{}] exception: {} for #{}/{} [{}] "
                     "from {} ({}) to {} ({}) in {} - skipping", name_, e.what(), idOf(env)
                     , env.responseId(), env.message.size(), env.sender->value()
                     , env.sender->name(), env.receiver ? env.receiver->value() : 0
                     , env.receiver ? env.receiver->name() : "null"
                     , curAdapter ? curAdapter->name() : "no adapter");
                  continue;
               }
            }
         }
         const auto lastId = idOf(env);
         if (lastId > lastProcessedSeqNo_) {
            lastProcessedSeqNo_ = lastId;
         }
         env.processingQueue = nullptr;
      }
      return nbProcessed;
   };

   while (running_) {
      bool queueEmpty = true;
      if (false) {
         std::unique_lock<std::mutex> dataLock(dataMutex_);
         queueEmpty = queue_.empty();
      }
      if (queueEmpty) {
         std::unique_lock<std::mutex> lock(cvMutex_);
         cvQueue_.wait_for(lock, std::chrono::milliseconds{ 5 });
      }
      if (!running_) {
         break;
      }
      size_t nbProcessed = 0;
      const auto startProcTime = bus_clock::now();
      if (!deferredQueue.empty()) {
         decltype(queue_) tempQueue;
         deferredQueue.swap(tempQueue);
         nbProcessed += processPortion(tempQueue, startProcTime);
      }
      if (!queue_.empty()) {
         decltype(queue_) tempQueue;
         {
            std::unique_lock<std::mutex> lock(dataMutex_);
            tempQueue.swap(queue_);
         }
         if (!tempQueue.empty()) {
            nbProcessed += processPortion(tempQueue, startProcTime);
         }
      }
      auto timeNow = bus_clock::now();
      if (nbProcessed) {
         const auto usProcTime = std::chrono::duration_cast<std::chrono::microseconds>
            (timeNow - startProcTime).count();
         const int sProcTime = usProcTime / 1000000;
         if (sProcTime > 300) {
            logger_->critical("[{}::{}] batch processing time exceeds 5min ({}s for {} msgs)"
               , __func__, name(), sProcTime, nbProcessed);
            //TODO: clean-up queue probably
         } else if (nbProcessed > batchLimit_) {
            logger_->warn("[{}::{}] batch count exceeds {} ({}, processed in {}s)"
               , __func__, name(), batchLimit_, nbProcessed, (usProcTime / 1000) / 1000.0);
         } else if ((nbProcessed >= (batchLimit_ / 5)) || (sProcTime > 2)) {
            logger_->info("[{}::{}] processed {} messages in {}ms", __func__, name()
               , nbProcessed, usProcTime / 1000.0);
         }
      }
      if (watchRequests_.size() > 100) {
         logger_->warn("[{}::{}] too many watch requests ({})", __func__, name()
            , watchRequests_.size());
      }
      auto itWatchReq = watchRequests_.begin();
      while (itWatchReq != watchRequests_.end()) {
         const auto& env = itWatchReq->second;
         if (env.receiver && (env.sender->value() == env.receiver->value())) {
            itWatchReq = watchRequests_.erase(itWatchReq);
            continue;   // don't timeout requests sent to self
         }
         if ((env.posted + env.repeatInterval) < timeNow) {
            logger_->warn("[Queue::process::{}] request #{} from {} to {} timed out\n{}"
               , name_, env.foreignId(), env.sender->name()
               , env.receiver ? env.receiver->name() : "null", convo::toHex(env.message));
            auto adapters = router_->process(itWatchReq->second);
            try {
               adapters.push_back(router_->obtainAdapterBy(env.sender));
            }
            catch (const std::exception&) {}

            for (const auto& adapter : adapters) {
               try {
                  adapter->processTimeout(env);
               }
               catch (const std::exception& e) {
                  logger_->error("[{}::processTimeout] failed in {}: {} for #{} from {}"
                     , adapter->name(), name_, e.what()
                     , idOf(itWatchReq->second), itWatchReq->second.sender->name());
               }
            }
            itWatchReq = watchRequests_.erase(itWatchReq);
         }
         else {
            ++itWatchReq;
         }
      }
      const auto defQSize = std::max(deferredQueue.size(), deferredIds_.size());
      if ((defQSize > (batchLimit_ / 2)) && (lastDeferredQueueSize_ < defQSize)) {
         logger_->warn("[{}::{}] deferred queue has grown to {}/{} (prev: {})"
            " elements, last 5 messages:", __func__, name_, deferredQueue.size()
            , deferredIds_.size(), lastDeferredQueueSize_);
         for (auto it = deferredQueue.rbegin(); it != deferredQueue.rend(); ++it) {
            if (it == (deferredQueue.rbegin() + 5)) {
               break;
            }
            logger_->info("\t#{}: {} ({}) -> {} r#{}", it->foreignId(), it->sender->name(), it->sender->value()
               , (it->receiver ? it->receiver->name() : "*"), it->rawResponseId());
         }
      }
      lastDeferredQueueSize_ = defQSize;
      if ((timeNow - accTime) >= accountingInterval_) {
         accTime = bus_clock::now();
         router_->reportQueueStats(name_, acc);
         acc.reset();
      }
   }
   router_->reportQueueStats(name_, acc);
   logger_->debug("[Queue::process::{}] finished", name_);
}

void Queue_Locking::bindAdapter(const std::shared_ptr<Adapter> &adapter)
{
   router_->bindAdapter(adapter);
}

std::set<UserValue> Queue_Locking::supportedReceivers() const
{
   return router_->supportedReceivers();
}
