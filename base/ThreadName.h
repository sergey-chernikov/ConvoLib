#ifndef CONVO_THREAD_NAME_H
#define CONVO_THREAD_NAME_H

#include <string>

namespace convo {

   // Sets name that is visible in crash dumps and in debugger
   // Implemented for Linux and macOS only for now
   // max length for name is 16 symbols including trailing zero (will be trimmed otherwise)
   void setCurrentThreadName(const std::string&);

}

#endif
