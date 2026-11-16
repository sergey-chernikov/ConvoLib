#include "Lua.h"
#include <iostream>
#ifdef BUILD_LUA
extern "C" {
#  include <lua.h>
#  include <lualib.h>
#  include <lauxlib.h>
}
#endif

using namespace convo;

#ifdef BUILD_LUA
Lua::Lua(const std::string& initialFN)
   : initialFN_(initialFN), state_{luaL_newstate()}
{
   luaL_openlibs(state_);

   if (!initialFN.empty()) {
      if (doFile(initialFN)) {
         lastError_ = std::format("failed to run {}", initialFN);
      }
   }
}

Lua::~Lua()
{
   stop();
}

void Lua::stop()
{
   lua_close(state_);
//   lua_closethread(state_, nullptr);
}

bool Lua::doFile(const std::string& fn)
{
   return luaL_dofile(state_, fn.c_str());
}

bool Lua::doBuffer(const std::string& buf)
{
   return luaL_dostring(state_, buf.c_str());
}

std::string Lua::getStringValue(const std::string& var) const
{
   const auto valType = lua_getglobal(state_, var.c_str());
   if (valType != LUA_TSTRING) {
      lastError_ = std::format("[Lua::getStringValue] wrong {} type {}", var, valType);
      return {};
   }
   return lua_tostring(state_, -1);
}

bool Lua::getBoolValue(const std::string& var) const
{
   const auto valType = lua_getglobal(state_, var.c_str());
   if (valType != LUA_TBOOLEAN) {
      lastError_ = std::format("[Lua::getBoolValue] wrong {} type {}", var, valType);
      return false;
   }
   return lua_toboolean(state_, -1);
}

std::vector<std::string> Lua::getStrings(const std::string& var) const
{
   const auto valType = lua_getglobal(state_, var.c_str());
   if (valType != LUA_TTABLE) {
      lastError_ = std::format("[Lua::getStrings] wrong {} type {}", var, valType);
      return {};
   }
   const auto len = lua_objlen(state_, -1);
   if (len <= 0) {
      return {};
   }
   std::vector<std::string> result;
   result.reserve(len);
   for (int i = 1; i <= len; ++i) {
      lua_getglobal(state_, var.c_str());
      const auto rc = lua_geti(state_, -1, i);
      if (rc == LUA_TSTRING) {
         result.push_back(lua_tostring(state_, -1));
      }
      else {
         lastError_ = std::format("[Lua::getStrings::{}] invalid format: {}", i, rc);
         std::cerr << lastError_ << "\n";
      }
   }
   return result;
}

convo::Table Lua::getTable(const std::string& var) const
{
   auto valType = lua_getglobal(state_, var.c_str());
   if (valType != LUA_TTABLE) {
      lastError_ = std::format("[Lua::getTable] wrong {} type {}", var, valType);
      std::cerr << "valType=" << valType << "\n";
      return {};
   }
   lua_pushnil(state_);
   std::string key;
   Table result;
   while (lua_next(state_, -2)) {
      key = lua_tostring(state_, -2);
      auto& val = result[key];
      valType = lua_type(state_, -1);
      switch (valType) {
      case LUA_TNIL:
      case LUA_TTABLE:
      case LUA_TNONE:
         break;
      case LUA_TNUMBER: {
         const double num = lua_tonumber(state_, -1);
         if (num == std::floor(num)) { //is an integer
            val = (int)num;
         }
         else {
            val = num;
         }
      } break;
      case LUA_TSTRING:
         val = std::string{ lua_tostring(state_, -1) };
         break;
      case LUA_TBOOLEAN:
         val = lua_toboolean(state_, -1);
         break;
      default:
         lastError_ = std::format("[getTable] unhandled valType {}", valType);
         std::cerr << lastError_ << "\n";
      }
      lua_pop(state_, 1);
   }
   lua_pop(state_, 1);
   return result;
}
#endif   //BUILD_LUA
