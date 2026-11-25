#ifndef CONVO_WORKER_H
#define CONVO_WORKER_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <thread>
#include <vector>

namespace convo {

   enum class ProcessingResult : uint8_t {
      Retry = 0,  // this is to map obsoleted boolean value
      Success,
      Ignored,
      TimedOut,
      Error,
      Captured    // special value for supervising adapter only
   };

   using WorkerClock = std::chrono::steady_clock;

   struct InData {
      virtual ~InData() = default;
      virtual std::string name() const { return "BaseInData"; }

      WorkerClock::time_point  execAfter{};
   };

   struct OutData {
      virtual ~OutData() = default;
   };

   using Callback = std::function<ProcessingResult(const std::shared_ptr<OutData>&)>;
   class Worker;
   class WorkerPool;

   class WorkerQueue
   {
   public:
      virtual ~WorkerQueue();
      virtual void assign(WorkerPool*);
      virtual bool accepts(const std::shared_ptr<InData>&) { return true; }
      virtual std::string name() const = 0;
      virtual bool needsNewThread() const;
      virtual bool needsNewInstance(const std::shared_ptr<InData>&) const { return false; }
      virtual std::shared_ptr<WorkerQueue> makeInstance(const std::shared_ptr<InData>&) = 0;
      virtual WorkerQueue* getInstance(const std::shared_ptr<InData>&) { return this; }
      virtual void newThread(const std::shared_ptr<InData>&);
      virtual void stopUnusedThreads();

      size_t size() const;
      size_t nbPending() const;
      bool empty() const;
      void cancel();

      void clear()
      {
         std::unique_lock<std::mutex> dataLock(dataMutex_);
         queue_.clear();
      }

      void notifyAll()
      {
         std::unique_lock<std::mutex> lock(cvMutex_);
         cvQueue_.notify_all();
      }

      void notify()
      {
         std::unique_lock<std::mutex> lock(cvMutex_);
         cvQueue_.notify_one();
      }

      void push(const std::pair<std::shared_ptr<InData>, Callback>& v)
      {
         std::unique_lock<std::mutex> lock(dataMutex_);
         queue_.push_back(v);
      }

      std::pair<std::shared_ptr<InData>, Callback> pop();

      void wait()
      {
         std::unique_lock<std::mutex> lock(cvMutex_);
         cvQueue_.wait_for(lock, procDelay_);
      }

   protected:
      explicit WorkerQueue(unsigned nbInst) : nbInstances_{ nbInst } {}
      virtual void stop();
      virtual void doProcess(const std::shared_ptr<Worker>&
         , const std::shared_ptr<InData>&, const Callback&);

   protected:
      std::deque<std::pair<std::shared_ptr<InData>, Callback>> queue_;
      std::condition_variable cvQueue_;
      std::mutex              cvMutex_;
      mutable std::mutex      dataMutex_, thrMutex_;
      WorkerPool* wp_{ nullptr };
      unsigned threshSize_{ 1 }, limit_{ 1 };
      std::chrono::milliseconds  procDelay_{ 10 };
      unsigned nbInstances_{ 0 };
      std::vector<std::jthread>  threads_;
      std::vector<std::weak_ptr<Worker>>  workers_;
      std::atomic_int   nbProcessing_{ 0 };
   };

   class Worker
   {
   public:
      virtual ~Worker() { cancel(); }
      virtual std::vector<std::shared_ptr<OutData>> process(const std::shared_ptr<InData>&) = 0;

      virtual void cancel() { isRunning_ = false; }
      bool isRunning() const { return isRunning_; }
      virtual bool isReady() const { return true; }
      virtual void setQueue(WorkerQueue* q) { queue_ = q; }
      virtual void pushBack(const std::shared_ptr<InData>&, const Callback&);

   protected:
      std::atomic_bool  isRunning_{ true };
      std::atomic<WorkerQueue*> queue_{ nullptr };
   };

   class Handler
   {
   public:
      virtual ~Handler() = default;
      virtual std::vector<std::shared_ptr<OutData>> process(const std::shared_ptr<InData>&) = 0;
   };

   class WorkerImpl : public Worker
   {
   public:
      using Handlers = std::vector<std::shared_ptr<Handler>>;
      WorkerImpl(const Handlers& handlers)
         : handlers_(handlers) {}

      std::vector<std::shared_ptr<OutData>> process(const std::shared_ptr<InData>& inData) override
      {
         std::vector<std::shared_ptr<OutData>> result;
         for (const auto& handler : handlers_) {
            if (!isRunning()) {
               return { nullptr };
            }
            const auto& outData = handler->process(inData);
            if (!outData.empty()) {
               result.insert(result.end(), outData.cbegin(), outData.cend());
            }
         }
         return result;
      }

   private:
      const std::vector<std::shared_ptr<Handler>>  handlers_;
   };

   template <typename TIn, typename TOut>
   class HandlerImpl : public Handler
   {
   public:
      std::vector<std::shared_ptr<OutData>> process(const std::shared_ptr<InData>& inData) override final
      {
         std::shared_ptr<TIn> data = std::dynamic_pointer_cast<TIn>(inData);
         if (data == nullptr) {
            return {};
         }
         return { std::static_pointer_cast<OutData>(processData(data)) };
      }

   protected:
      virtual std::shared_ptr<TOut> processData(const std::shared_ptr<TIn>&) = 0;
   };


   class WorkerPool
   {
      friend class WorkerQueue;
   public:
      WorkerPool(const std::vector<std::shared_ptr<WorkerQueue>>& queues = {}
         , const unsigned nbThreadsThreshold = 1
         , const unsigned threadLimit = std::thread::hardware_concurrency() / 2);
      virtual ~WorkerPool();

      virtual void processQueued(const std::shared_ptr<InData>&, const Callback&);
      void cancel();
      virtual bool finished() const;

   protected:  // allow different worker types based on input data to allow by-data thread affinity
      virtual std::shared_ptr<Worker> worker(const std::shared_ptr<InData>&) = 0;

   protected:
      const unsigned int threadLimit_;
      const unsigned int nbThreadsThreshold_;
      std::vector<std::shared_ptr<WorkerQueue>> queues_;
   };

}

#endif	// CONVO_WORKER_H
