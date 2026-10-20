#ifndef __CONVO_SER_DESER_H__
#define __CONVO_SER_DESER_H__

#include <string>
#include <variant>
#include <vector>
#ifdef USE_NLOHMANN_JSON
#include <nlohmann/json.hpp>
#endif


namespace convo {
   class FixedDecimalValue
   {
   public:
      FixedDecimalValue(const FixedDecimalValue& v) : precision_(v.precision_)
         , significand_(v.significand_) {}
      FixedDecimalValue(uint8_t precision = 2, int64_t val = 0) : precision_(precision)
         , significand_{ val } {}
      FixedDecimalValue(double val, uint8_t precision = 2) : precision_(precision)
         , significand_(val * powerOf10(precision)) {}
      ~FixedDecimalValue() noexcept = default;

      inline void reset() { significand_ = 0; }
      inline int64_t getRawValue() const { return significand_; }
      inline uint8_t getPrecision() const { return precision_; }
      inline bool isZero() const { return (significand_ == 0); }

      inline bool operator==(const FixedDecimalValue& v) const
      {
         return (significand_ == v.significand_) && (precision_ == v.precision_);
      }
      inline FixedDecimalValue operator=(const FixedDecimalValue& v)
      {
         significand_ = v.significand_;
         precision_ = v.precision_;
         return *this;
      }
      inline FixedDecimalValue operator+=(const FixedDecimalValue& v)
      {
         significand_ += v.significand_;
         return *this;
      }
      inline bool operator!() const
      {
         return significand_ == 0;
      }

      inline std::string toString() const
      {
         return std::format("{:.{}f}", toDouble(), precision_);
      }
      inline double toDouble() const
      {
         return significand_ / powerOf10(precision_);
      }

   protected:
      uint8_t precision_{ 2 };
      int64_t significand_{ 0 };

   private:
      static double powerOf10(uint8_t prec) {
         if (prec > 0) {
            return 10 * powerOf10(prec - 1);
         }
         return 1;
      }
   };

   namespace ser {
      using BinaryData = std::vector<uint8_t>;
      using Type = std::variant<std::monostate, bool, uint8_t, int16_t, uint16_t
         , int32_t, uint32_t, int64_t, uint64_t, float, double, FixedDecimalValue
         , std::string, BinaryData>;

      std::string toString(const Type&);
      using Struct = std::vector<std::pair<std::string, Type>>;
      std::string toString(const Struct&, const std::string& delim = "; ");
      bool has(const Struct&, const std::string&);

#ifdef USE_NLOHMANN_JSON
      nlohmann::json toJson(const Type&);
      Type fromJsonT(const nlohmann::json&);

      nlohmann::json toJson(const Struct&);
      Struct fromJson(const nlohmann::json&);
#endif

      enum class T : uint8_t
      {
         UNDEF,
         BOOL,
         UINT8,
         INT16,
         UINT16,
         INT32,
         UINT32,
         INT64,
         UINT64,
         FLOAT,
         DOUBLE,
         FIXED,
         STRING,
         BINARY,
         COMPLEX
      };
      using FieldsMeta = std::vector<std::pair<std::string, T>>;

      std::string unquote(const std::string& s);

#define DATA_FIELD(T, TT, N, D) \
public:\
   T N{addField_##N()};\
protected:\
   T addField_##N() { addMeta(convo::ser::unquote("\""#N"\""), {{{},TT}}); return D; }

#define NESTED_DATA(T, N, M, D) \
public:\
   T N{addField_##N()};\
protected:\
   T addField_##N() { addMeta(convo::ser::unquote("\""#N"\""), M); return D; }

      struct Data
      {
         Data(const std::string& name) : name_(name) {}
         virtual ~Data() = default;
         virtual std::string serialize() const = 0;

      protected:
         virtual void addMeta(const std::string& name, const FieldsMeta&);
         virtual std::string serialize(const std::string& name, T, Type) const = 0;
         virtual std::tuple<std::string, T, Type> deserialize(const std::string&) const = 0;

         const std::string name_;
         std::vector<std::pair<std::string, FieldsMeta>> metadata_;
      };

      struct LegacyData : public Data
      {
         LegacyData(const std::string& name) : Data(name) {}

      protected:
         std::string serialize(const std::string& name, T, Type) const override final;
         std::tuple<std::string, T, Type> deserialize(const std::string&) const override final;
      };

#ifdef USE_NLOHMANN_JSON
      struct JSON : public Data, public nlohmann::json
      {
         JSON(const std::string& name) : Data(name) {}
         std::string serialize() const { return dump(); }
         static nlohmann::json deser(const std::string&);

      protected:
         void addMeta(const std::string& name, const FieldsMeta&) override final {}
         std::string serialize(const std::string& name, T, Type) const override final { return {}; }
         std::tuple<std::string, T, Type> deserialize(const std::string&) const override final { return {}; }
      };
#endif

#ifndef DISABLE_CAPNP_COMMON
      struct CapnProto : public Data
      {
      protected:
         void addMeta(const std::string& name, const FieldsMeta&) override final;
         std::string serialize(const std::string& name, T, Type) const override final;
         std::tuple<std::string, T, Type> deserialize(const std::string&) const override final;
      };
#endif
   }  //namespace ser
}  //namespace convo

#endif	//__CONVO_SER_DESER_H__
