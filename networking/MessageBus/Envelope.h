#ifndef CONVO_MESSAGE_ENVELOPE_H
#define CONVO_MESSAGE_ENVELOPE_H

#include <chrono>
#include <map>
#include <memory>
#include <string>

namespace convo {
   namespace msg {
      using UserValue = int;
      using bus_clock = std::chrono::steady_clock;
      class QueueInterface;

      static_assert(bus_clock::is_steady, "Should be steady clock");
      static_assert(bus_clock::period::den >= 10000, "Should be at least ms");

      using TimeStamp = std::chrono::time_point<bus_clock>;

      class User
      {
      public:
         virtual ~User() = default;
         User(UserValue value) : value_(value) {}
         UserValue value() const { return value_; }
         template<typename T> T value() const { return static_cast<T>(value_); }

         virtual std::string name() const { return std::to_string(value_); }
         virtual bool isSystem() const { return false; }
         virtual bool isBroadcast() const { return false; }
         virtual bool isFallback() const { return false; }

      protected:
         UserValue   value_;
      };


      class UserSystem : public User
      {
      public:
         UserSystem() : User(0) {}

         std::string name() const override { return "System"; }
         bool isSystem() const override { return true; }
      };

      class UserFallback : public User
      {
      public:
         UserFallback() : User(0) {}

         std::string name() const override { return "Fallback"; }
         bool isFallback() const override { return true; }
      };


      using SeqId = uint64_t;
      enum class EnvelopeType : SeqId
      {
         GlobalBroadcast = UINT64_MAX,
         Publish = UINT64_MAX - 1,           // response to subscription request
         Update = UINT64_MAX - 2,            // message from one adapter to another that does not require subscriptions and is not a request
         Processed = UINT64_MAX - 3,         // mark message as processed to prevent infinite broadcast loop across multiple queues
         MaxValue = UINT64_MAX - 15          // all values above should be treated as envelope type values only
      };

      enum class ContentType : uint8_t
      {
         unknown,
         text,
         protobuf,
         json,
         capnp
      };
      ContentType fromString(const std::string&);


      class Envelope
      {
         friend class QueueInterface;
         friend class Router;
         friend class RelayAdapter;

      public:
         Envelope() = default;

         static Envelope makeRequest(const std::shared_ptr<User>& s, const std::shared_ptr<User>& r
            , const std::string& msg, ContentType ct = ContentType::capnp, const TimeStamp& execAt = {}
            , unsigned rptCount = 0, const bus_clock::duration& rptInterval = {})
         {
            return Envelope{ s, r, execAt, msg, ct, 0, rptCount, rptInterval };
         }

         static Envelope makeResponse(const std::shared_ptr<User>& s, const std::shared_ptr<User>& r
            , const std::string& msg, SeqId respId, ContentType ct = ContentType::capnp
            , unsigned rptCount = 0, const bus_clock::duration& rptInterval = {})
         {
            /*if (!respId) {  // possible cause of envelope type mismatches
               respId = static_cast<SeqId>(EnvelopeType::Update);
            }*/
            return Envelope{ s, r, msg, ct, respId, rptCount, rptInterval };
         }

         static Envelope makeBroadcast(const std::shared_ptr<User>& s
            , const std::string& msg, ContentType ct = ContentType::capnp, bool global = false)
         {
            return Envelope{ s, nullptr, msg, ct, global ? (SeqId)EnvelopeType::GlobalBroadcast : 0 };
         }

         void setIdIfUnset(SeqId id)
         {
            if (id_ == 0) {
               setId(id);
            }
         }

         void setId(SeqId id)
         {
            id_ = id;
            if (!foreignId_) {
               foreignId_ = id;
            }
         }

         SeqId foreignId() const { return foreignId_; }
         void setForeignId(SeqId id) { foreignId_ = id; }

         SeqId responseId() const
         {
            if (responseId_ >= static_cast<SeqId>(EnvelopeType::MaxValue)) {
               return 0;
            }
            return responseId_;
         }
         SeqId rawResponseId() const { return responseId_; }

         void resetEnvelopeType() { responseId_ = 0; }
         void setEnvelopeType(const EnvelopeType f) { responseId_ = (SeqId)f; }

         EnvelopeType envelopeType() const
         {
            if (responseId_ < static_cast<SeqId>(EnvelopeType::MaxValue)) {
               return EnvelopeType::MaxValue;
            }
            return static_cast<EnvelopeType>(responseId_);
         }
         void resetGlobalBC()
         {
            if (envelopeType() == EnvelopeType::GlobalBroadcast) {
               responseId_ = 0;
            }
         }

         bool isRequest() const { return (responseId_ == 0); }
         bool isBC() const { return (!receiver || receiver->isBroadcast()); }

         std::shared_ptr<User>   sender;
         std::shared_ptr<User>   receiver;
         TimeStamp   posted;
         TimeStamp   executeAt;
         std::string message;
         ContentType contentType{ ContentType::unknown };
         std::string origin{};
         int   repeatCount{ 0 };
         bus_clock::duration  repeatInterval{};
         std::string routingKey{};
         QueueInterface*  processingQueue{nullptr};
         std::shared_ptr<User>   replyTo; //if set, the request is sent on behalf of this user
         bool relayed{ false };

      private:
         Envelope(const std::shared_ptr<User>& s, const std::shared_ptr<User>& r
            , const std::string& msg, ContentType ct = ContentType::unknown
            , SeqId respId = 0, unsigned rptCount = 0
            , const bus_clock::duration& rptInterval = {})
            : sender(s), receiver(r), message(msg), contentType(ct)
            , repeatCount(rptCount), repeatInterval(rptInterval), responseId_(respId)
         {}
         Envelope(const std::shared_ptr<User>& s, const std::shared_ptr<User>& r
            , const TimeStamp& execAt, const std::string& msg, ContentType ct = ContentType::unknown
            , SeqId respId = 0, unsigned rptCount = 0
            , const bus_clock::duration& rptInterval = {})
            : sender(s), receiver(r), executeAt(execAt), message(msg), contentType(ct)
            , repeatCount(rptCount), repeatInterval(rptInterval), responseId_(respId)
         {}

         SeqId id() const { return id_; } // no need to know the internal id for all regular users

         SeqId id_{ 0 };         // always unique and growing (no 2 envelopes can have the same id)
         SeqId foreignId_{ 0 };  // used at gatewaying from external bus
         SeqId responseId_{ 0 }; // should be set in reply and for special values of EnvelopeType
      };

      using QueueId = unsigned int;

   } // namespace message
} // namespace convo

bool operator==(const std::shared_ptr<convo::msg::User>& a, const std::shared_ptr<convo::msg::User>& b);
bool operator<(const std::shared_ptr<convo::msg::User>& a, const std::shared_ptr<convo::msg::User>& b);

#endif	// CONVO_MESSAGE_ENVELOPE_H
