/*
*/
#ifndef _CONVO_LUA_H_
#define _CONVO_LUA_H_
#ifdef BUILD_LUA
extern "C" {
# include <lstate.h>
}
#endif
#include <map>
#include "SerDeser.h"

namespace convo {

   using Table = std::map<std::string, ser::Type>;

#ifdef BUILD_LUA
   class Lua {
   public:
      Lua(const std::string& initialFN = {});
      ~Lua();

      void stop();
      std::string lastError() const { return lastError_; }

      bool doFile(const std::string& fn);
      bool doBuffer(const std::string&);

      std::string getStringValue(const std::string& var) const;
      bool getBoolValue(const std::string& var) const;
      std::vector<std::string> getStrings(const std::string& var) const;
      //std::vector<ComplexTab> getTables(const std::string& var) const;
      Table getTable(const std::string& var) const;

   private:
      const std::string initialFN_;
      lua_State* state_{ nullptr };
      mutable std::string lastError_;
   };
#endif
}  //convo

#endif //_CONVO_LUA_H_
