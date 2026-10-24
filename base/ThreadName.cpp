#include "ThreadName.h"

#ifdef WIN32

void convo::setCurrentThreadName(const std::string& /*name*/)
{
   //TODO
}

#else

#include <pthread.h>

void convo::setCurrentThreadName(const std::string& n)
{
   auto name = n;
   constexpr size_t maxNameLen = 15;
   if (name.size() > maxNameLen) {
      name.resize(maxNameLen);
   }
   pthread_setname_np(pthread_self(), name.c_str());
}

#endif
