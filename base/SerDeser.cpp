#include "SerDeser.h"
#include "StringUtils.h"

using namespace convo::ser;

void Data::addMeta(const std::string& name, const FieldsMeta& m)
{
   int i = 0;
   for (; i < metadata_.size(); ++i) {
      if (metadata_.at(i).first == name) {
         metadata_[i].second = m;
         return;
      }
   }
   metadata_.push_back({name, m});
}


std::string LegacyData::serialize(const std::string& name, T t, Type v) const
{
   std::string result;
   //TODO
   return result;
}

std::tuple<std::string, T, Type> LegacyData::deserialize(const std::string&) const
{
   //TODO
   return std::tuple<std::string, T, Type>();
}

#ifdef USE_NLOHMANN_JSON
nlohmann::json JSON::deser(const std::string& str)
{
   try {
      return nlohmann::json::parse(str);
   }
   catch (const nlohmann::json::exception&) {
      return {};
   }
}
#endif

#ifndef DISABLE_CAPNP_COMMON
void CapnProto::addMeta(const std::string& name, const FieldsMeta&)
{
   //TODO
}

std::string CapnProto::serialize(const std::string& name, T, Type) const
{
   return {};  //TODO
}

std::tuple<std::string, T, Type> CapnProto::deserialize(const std::string&) const
{
   return {};  //TODO
}
#endif

#include <iostream>

namespace convo {
   namespace ser {
      std::string toString(const Type& v)
      {
         if (std::holds_alternative<uint8_t>(v)) {
            return std::to_string(std::get<uint8_t>(v));
         } else if (std::holds_alternative<int32_t>(v)) {
            return std::to_string(std::get<int32_t>(v));
         } else if (std::holds_alternative<uint32_t>(v)) {
            return std::to_string(std::get<uint32_t>(v));
         } else if (std::holds_alternative<int16_t>(v)) {
            return std::to_string(std::get<int16_t>(v));
         } else if (std::holds_alternative<uint16_t>(v)) {
            return std::to_string(std::get<uint16_t>(v));
         } else if (std::holds_alternative<bool>(v)) {
            return std::to_string(std::get<bool>(v));
         } else if (std::holds_alternative<int64_t>(v)) {
            return std::to_string(std::get<int64_t>(v));
         } else if (std::holds_alternative<uint64_t>(v)) {
            return std::to_string(std::get<uint64_t>(v));
         } else if (std::holds_alternative<float>(v)) {
            return std::to_string(std::get<float>(v));
         } else if (std::holds_alternative<double>(v)) {
            return std::to_string(std::get<double>(v));
         } else if (std::holds_alternative<FixedDecimalValue>(v)) {
            return std::get<FixedDecimalValue>(v).toString();
         } else if (std::holds_alternative<std::string>(v)) {
            return std::get<std::string>(v);
         } else if (std::holds_alternative<convo::ser::BinaryData>(v)) {
            return convo::toHex(std::get<convo::ser::BinaryData>(v));
         }
         return {};
      }

      std::string toString(const Struct& st, const std::string& delim)
      {
         std::string result;
         for (const auto& pair : st) {
            result += pair.first + "=" + toString(pair.second) + delim;
         }
         if (!result.empty()) {
            for (int i = 0; i < delim.size(); ++i) {
               result.pop_back();
            }
         }
         return result;
      }

      bool has(const Struct& st, const std::string& key)
      {
         for (const auto& pair : st) {
            if (pair.first == key) {
               return true;
            }
         }
         return false;
      }

      std::string unquote(const std::string& s)
      {
         if ((s.size() > 2) && (s[0] == '"') && (s[s.size() - 1] == '"')) {
            return s.substr(1, s.size() - 2);
         }
         return s;
      }

#ifdef USE_NLOHMANN_JSON
      nlohmann::json toJson(const Type& t)
      {
         if (std::holds_alternative<bool>(t)) {
            return std::get<bool>(t);
         }
         else if (std::holds_alternative<std::string>(t)) {
            return std::get<std::string>(t);
         }
         else if (std::holds_alternative<double>(t)) {
            return std::get<double>(t);
         }
         else if (std::holds_alternative<float>(t)) {
            return std::get<float>(t);
         }
         else if (std::holds_alternative<int32_t>(t)) {
            return std::get<int32_t>(t);
         }
         else if (std::holds_alternative<FixedDecimalValue>(t)) {
            return std::get<FixedDecimalValue>(t).toDouble();
         }
         else if (std::holds_alternative<BinaryData>(t)) {
            return std::get<BinaryData>(t);
         }
         else if (std::holds_alternative<uint8_t>(t)) {
            return std::get<uint8_t>(t);
         }
         else if (std::holds_alternative<int16_t>(t)) {
            return std::get<int16_t>(t);
         }
         else if (std::holds_alternative<uint16_t>(t)) {
            return std::get<uint16_t>(t);
         }
         else if (std::holds_alternative<uint32_t>(t)) {
            return std::get<uint32_t>(t);
         }
         else if (std::holds_alternative<int64_t>(t)) {
            return std::get<int64_t>(t);
         }
         else if (std::holds_alternative<uint64_t>(t)) {
            return std::get<uint64_t>(t);
         }
         return {};
      }

      nlohmann::json toJson(const Struct& st)
      {
         nlohmann::json result;
         for (const auto& pair : st) {
            if (pair.first.empty()) {
               continue;
            }
            result[pair.first] = convo::ser::toJson(pair.second);
         }
         return result;
      }

      Type fromJsonT(const nlohmann::json& v)
      {
         switch (v.type()) {
         case nlohmann::json::value_t::binary:
            return v.get<BinaryData>();
         case nlohmann::json::value_t::array: {
            if (v.empty()) { return BinaryData{}; }
            const auto firstItem = v.at(0);
            if (firstItem.is_number_integer() || firstItem.is_number_unsigned()) {
               return v.get<BinaryData>();
            }
            return {};
         }
         case nlohmann::json::value_t::boolean:
            return v.get<bool>();
         case nlohmann::json::value_t::string:
            return v.get<std::string>();
         case nlohmann::json::value_t::number_float:
            return v.get<double>();
         case nlohmann::json::value_t::number_integer:
            return v.get<int>();
         case nlohmann::json::value_t::number_unsigned:
            return (int)v.get<unsigned>();
         default: return {};
         }
      }

      Struct fromJson(const nlohmann::json& msg)
      {
         Struct result;
         for (const auto& obj : msg.items()) {
            const auto& val = fromJsonT(obj.value());
            if (std::holds_alternative<std::monostate>(val)) {
               continue;
            }
            result.push_back({ obj.key(), val });
         }
         return result;
      }
#endif
   }
}
