#ifndef CONVO_MSGBUS_ADAPTER_H
#define CONVO_MSGBUS_ADAPTER_H

#include <memory>
#include <set>
#ifdef WITH_CAPNP
#include <capnp/message.h>
#endif
#include "MessageBus/Envelope.h"
#include "WorkQueue/Worker.h"
#include "base/PerfAccounting.h"


namespace convo {
   namespace msg {
      class QueueInterface;

      class Adapter
      {
      public:
         struct cmpUsers {
            bool operator() (const std::shared_ptr<User>& a, const std::shared_ptr<User>& b) const
            {
               return (a->value() < b->value());
            }
         };
         using Users = std::set<std::shared_ptr<User>, cmpUsers>;
         using ContentTypes = std::set<ContentType>;

         virtual ~Adapter() = default;

         virtual ProcessingResult process(const Envelope &) = 0;
         virtual void processTimeout(const Envelope&) {}

         // if false is returned, it's not counted in processing stats (broadcast will never return back anyway)
         virtual bool processBroadcast(const Envelope&) = 0;

         virtual Users supportedReceivers() const = 0;
         virtual ContentTypes supportedContent() const { return {}; }   // empty means "accept everything"
         virtual std::string name() const = 0;
         virtual bool isSupervisor() const { return false; };

         virtual void reportProcessingStats(const std::string& /*queueName*/
            , const PerfAccounting&) {}

         virtual void setQueue(const std::shared_ptr<QueueInterface> &queue)
         {
            if (!queue_) {
               queue_ = queue;
            }
         }

         void setCurrentQueue(const QueueId id)
         {
            currentQueue_ = id;
         }

      protected:
         virtual void reportInvalidSender(const std::shared_ptr<User>&, SeqId) {}
         virtual bool pushFill(Envelope &);

         SeqId pushRequest(const std::shared_ptr<User>& sender
            , const std::shared_ptr<User>& receiver
            , const std::string& msg, ContentType ct = ContentType::text
            , const TimeStamp& execAt = {}, unsigned rptCount = 0
            , const bus_clock::duration& rptInterval = {});
         SeqId pushTimeoutRequest(const std::shared_ptr<User>& sender
            , const std::shared_ptr<User>& receiver
            , const std::string& msg, const bus_clock::duration& rptInterval
            , ContentType ct = ContentType::text, unsigned rptCount = 0);
         SeqId pushResponse(const std::shared_ptr<User>& sender
            , const std::shared_ptr<User>& receiver
            , const std::string& msg, ContentType ct = ContentType::text
            , SeqId respId = (SeqId)EnvelopeType::Update);
         virtual SeqId pushResponse(const std::shared_ptr<User>& sender
            , const Envelope& envReq, const std::string& msg
            , ContentType ct = ContentType::text, unsigned rptCount = 0
            , const bus_clock::duration& rptInterval = {});
         SeqId pushUpdate(const std::shared_ptr<User>& sender
            , const std::shared_ptr<User>& receiver, const std::string&
            , ContentType ct = ContentType::json
            , SeqId respId = (SeqId)EnvelopeType::Update);
         SeqId pushBroadcast(const std::shared_ptr<User>& sender
            , const std::string& msg, bool global = false, ContentType ct = ContentType::text);

#ifdef BUILD_CAPNP
         SeqId pushRequest(const std::shared_ptr<User>& sender
            , const std::shared_ptr<User>& receiver, capnp::MessageBuilder&
            , const TimeStamp& execAt = {}, unsigned rptCount = 0
            , const bus_clock::duration& rptInterval = {});
         SeqId pushUpdate(const std::shared_ptr<User>& sender
            , const std::shared_ptr<User>& receiver, capnp::MessageBuilder&
            , SeqId respId = (SeqId)EnvelopeType::Update);
         virtual SeqId pushResponse(const std::shared_ptr<User>& sender
            , const Envelope& envReq, capnp::MessageBuilder&
            , unsigned rptCount = 0, const bus_clock::duration& rptInterval = {});
         SeqId pushBroadcast(const std::shared_ptr<User>& sender
            , capnp::MessageBuilder& msg, bool global = false);
#endif

      protected:
         std::shared_ptr<QueueInterface>  queue_;
         QueueId  currentQueue_{ 0 };
      };

      class SharedAdapter : public Adapter   // allows sharing of one adapter
      {                                      // instance between multiple buses
      public:                                // in one process
         ~SharedAdapter() override = default;

         void setQueue(const std::shared_ptr<QueueInterface>&) override;
         bool pushFill(Envelope&) override;

      private:
         std::vector<std::shared_ptr<QueueInterface>> queues_;
      };

      class RelayAdapter : public Adapter
      {
      public:
         RelayAdapter() {}
         RelayAdapter(const std::shared_ptr<User> &user)
            : fallbackUser_(user) {}

         Users supportedReceivers() const override;
         std::string name() const override { return "Relay"; }

         void setQueue(const std::shared_ptr<QueueInterface>&) override;

      protected:
         bool pushFill(Envelope&) override;
         ProcessingResult process(const Envelope &) override;
         bool processBroadcast(const Envelope&) override;
         ProcessingResult relay(const Envelope&);
         bool isInitialized() const;

      protected:
         std::shared_ptr<User>   fallbackUser_;
         std::map<UserValue, std::shared_ptr<QueueInterface>>  queueByUser_;

      private:
         std::set<std::shared_ptr<QueueInterface>>             queues_;
      };

      class PipeAdapter : public Adapter
      {
      public:
         PipeAdapter() {}
         PipeAdapter(const std::shared_ptr<PipeAdapter>& endpoint)
            : endpoint_(endpoint) {}

         void setEndpoint(const std::shared_ptr<PipeAdapter>& endpoint) {
            endpoint_ = endpoint;
         }

      protected:
         ProcessingResult process(const Envelope&) override;
         bool processBroadcast(const Envelope&) override;

      private:
         std::shared_ptr<PipeAdapter>  endpoint_;
      };

   } // namespace msg
} // namespace convo

#endif	// CONVO_MSGBUS_ADAPTER_H
