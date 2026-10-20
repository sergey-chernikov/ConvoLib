#include <fstream>
#include <sstream>
#ifdef BUILD_CURL
#include <curl/curl.h>
#include <curl/header.h>
#endif
#include <spdlog/spdlog.h>
#include "WorkQueue/HttpWorker.h"
#include "base/StringUtils.h"

namespace convo {
   std::function<ProcessingResult(const std::shared_ptr<OutData>&)>
      makeHttpCallback(const HttpCallback& cb)
   {
      return [cb](const std::shared_ptr<OutData>& out)
      {
         const auto& data = std::static_pointer_cast<HttpOut>(out);
         if (!data) {
            cb(0, {}, {});
            return ProcessingResult::Error;
         }
         cb(data->code, data->response, data->headers);
         return ProcessingResult::Success;
      };
   }
}

using namespace convo;

HttpGetHandler::HttpGetHandler(const std::shared_ptr<spdlog::logger>& logger
   , const MTLSsettings& mtls, bool dumpComm, const ProxySettings& proxy
   , const HttpBasicAuth& auth)
   : HttpHandler(logger, mtls, dumpComm, proxy, auth)
{
#ifdef BUILD_CURL
   curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1);
   curl_easy_setopt(curl_, CURLOPT_ACCEPT_ENCODING, NULL); 
#endif
}

std::shared_ptr<HttpOut> HttpGetHandler::processData(const std::shared_ptr<InHttpGet>& in)
{
   auto result = HttpHandler::processData(in);
   if (!result) {
      return nullptr;
   }
   handleRequest(in->method(), result, in->headers, in->url, in->params, {}
      , in->dumpResponse, in->dumpRequest);
   return result;
}


std::shared_ptr<HttpOut> HttpPostHandler::processData(const std::shared_ptr<InHttpPost>& in)
{
   auto result = HttpHandler::processData(in);
   if (!result) {
      return nullptr;
   }
   handleRequest(in->method(), result, in->headers, in->url, in->params, in->data
      , in->dumpResponse, in->dumpRequest);
   return result;
}


static size_t writeToString(void* ptr, size_t size, size_t count, std::string* stream)
{
   const size_t resSize = size * count;
   stream->append((char*)ptr, resSize);
   return resSize;
}

HttpHandler::HttpHandler(const std::shared_ptr<spdlog::logger>& logger
   , const MTLSsettings& mtls, bool dumpComm, const ProxySettings& proxy
   , const HttpBasicAuth& basicAuth)
   : logger_(logger), dumpComm_(dumpComm)
#ifdef BUILD_JWT
   , jwtPrivKey_(proxy.jwtPrivKey), jwtKid_(proxy.jwtKId)
#endif
   , merchantId_(proxy.merchantId)
{
#ifdef BUILD_CURL
   curl_ = curl_easy_init();
   //curl_easy_setopt(curl_, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2);
   curl_easy_setopt(curl_, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
   curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, writeToString);
   curl_easy_setopt(curl_, CURLOPT_PIPEWAIT, 0L);

   if (!basicAuth.empty()) {
      curl_easy_setopt(curl_, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
      curl_easy_setopt(curl_, CURLOPT_USERNAME, basicAuth.user.c_str());
      if (!basicAuth.password.empty()) {
         curl_easy_setopt(curl_, CURLOPT_PASSWORD, basicAuth.password.c_str());
      }
   }
   if (!mtls.empty()) {
      curl_easy_setopt(curl_, CURLOPT_SSLCERT, mtls.ourCert.c_str());
      curl_easy_setopt(curl_, CURLOPT_KEYPASSWD, mtls.ourCertPass.c_str());
      curl_easy_setopt(curl_, CURLOPT_SSLKEY, mtls.ourKey.c_str());
      //curl_easy_setopt(curl_, CURLOPT_SSLKEYTYPE, "PEM"|"DER"|"ENG");
      //curl_easy_setopt(curl_, CURLOPT_SSLCERTTYPE, "PEM"|"DER"|"ENG");
      curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 1);

      if (mtls.caRemoteCert.empty()) {
         curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 2);
      }
      else {
         curl_easy_setopt(curl_, CURLOPT_CAINFO, mtls.caRemoteCert.c_str());
      }
   }
   if (dumpComm_ && false) {
      curl_easy_setopt(curl_, CURLOPT_VERBOSE, 1L);
   }
   if (!proxy.url.empty()) {
      curl_easy_setopt(curl_, CURLOPT_PROXY, proxy.url.c_str());
      curl_easy_setopt(curl_, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);

      if (!proxy.sslCert.empty()) {
         curl_easy_setopt(curl_, CURLOPT_PROXY_SSLCERT, proxy.sslCert.c_str());
      }
      if (!proxy.sslKey.empty()) {
         curl_easy_setopt(curl_, CURLOPT_PROXY_SSLKEY, proxy.sslKey.c_str());
      }
      if (!proxy.jwtPrivKey.empty()) {
         if (proxy.jwtKId.empty()) {
            throw std::invalid_argument("jwt_key_id is not set");
         }
         if (proxy.merchantId.empty()) {
            throw std::invalid_argument("merchantId is not set");
         }
         std::ifstream f(proxy.jwtPrivKey);
         if (!f.good()) {
            throw std::invalid_argument("can't open " + proxy.jwtPrivKey + " for reading");
         }
      }
   }

   if (mtls.empty()) {
      if (proxy.rootCA.empty()) {
         curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 0L);
         curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 0L);
      } else {
         curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 1L);
         curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 2L);
         const auto result = curl_easy_setopt(curl_, CURLOPT_CAINFO, proxy.rootCA.c_str());
         if (logger_ && (result != CURLE_OK)) {
            logger_->error("[{}] failed to set CAinfo {}: {}", __func__
               , proxy.rootCA, (int)result);
         }
      }
   }
   //mHandle_ = curl_multi_init();
   //curl_multi_setopt(mHandle_, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
#endif
}

HttpHandler::~HttpHandler()
{
#ifdef BUILD_CURL
   curl_easy_cleanup(curl_);
#endif
}

static std::string urlEncode(const std::string& str)
{
   std::string result;
   for (const auto& c : str) {
      if (isalnum(c) || (c == '-') || (c == '_') || (c == '.') || (c == '~')) {
         result += c;
      }
      else {
         result += fmt::format("%{:02X}", (unsigned char)c);
      }
   }
   return result;
}

std::string HttpHandler::urlEncoded(const HttpParams& params)
{
   if (params.empty()) {
      return {};
   }
   std::string result;
   for (const auto& pair : params) {
      if (pair.first.empty()) {
         continue;
      }
      result += pair.first + "=" + urlEncode(pair.second) + "&";
   }
   if (!result.empty()) {
      result.pop_back();
   }
   return result;
}

void HttpHandler::dumpHeaders(const HttpHeaders& hdr)
{
   if (!logger_) {
      return;
   }
   for (const auto& h : hdr) {
      if (h.second.length() <= 32) {
         logger_->debug("[H]   {}:\t{}", h.first, h.second);
      }
      else {
         logger_->debug("[H]   {}:\t{}", h.first, h.second.substr(0, 29) + "...");
      }
   }
}

static std::string readFile(const std::string& filename)
{
   std::ifstream file(filename);
   std::stringstream buffer;
   buffer << file.rdbuf();
   return buffer.str();
}

std::shared_ptr<HttpOut> bs::HttpHandler::processData(const std::shared_ptr<HttpIn>& in)
{
#ifdef BUILD_CURL
   if (!curl_) {
      return nullptr;
   }
#endif
   const auto& out = std::make_shared<HttpOut>();
   for (const auto& hdr : in->responseHdr) {
      out->headers.insert({ hdr, {} });
   }
   return out;
}

void HttpHandler::handleRequest(const std::string& method, const std::shared_ptr<HttpOut>& result
   , HttpHeaders& headers, std::string url, const HttpParams& params, std::string body
   , DumpOptions dumpResponse, DumpOptions dumpRequest)
{
   if (!params.empty()) {
      if (body.empty() && (method != "GET")) {
         body = urlEncoded(params);
         headers["Content-length"] = std::to_string(body.length());
         if ((headers.find("Content-Type") == headers.end()) &&
            (headers.find("Content-type") == headers.end())) {
            headers["Content-type"] = "application/x-www-form-urlencoded";
         }
      } else {
         const auto itParams = url.find('?');
         if (itParams == std::string::npos) {   //skip adding params for an existing param string
            url += (url.empty() ? "/" : "?") + urlEncoded(params);
         }
      }
   }
   if (logger_ && dumpComm_ && (dumpRequest != NoDump)) {
      std::string printableBody;
      switch (dumpRequest) {
      case Text:
         if (body.size() > 4090) {
            printableBody = body.substr(0, 2000) + fmt::format("<-- {} bytes here -->"
               , body.size() - 4000) + body.substr(body.size() - 2000);
         }
         else {
            printableBody = body;
         }
         break;
      case OnlySize:
         printableBody = fmt::format("{} bytes", body.size());
         break;
      default: break;
      }
      logger_->debug("[{}] {} -> {}", method, printableBody, url);
#ifdef DUMP_PARAMS
      for (const auto& p : params) {
         logger_->debug("[P]   {}: {}", p.first, p.second);
      }
#endif
      dumpHeaders(headers);
   }
#ifdef BUILD_CURL
   struct curl_slist* curlHeaders = NULL;
   for (const auto& h : headers) {
      curlHeaders = curl_slist_append(curlHeaders, (h.first + ": " + h.second).c_str());
   }
   curlHeaders = curl_slist_append(curlHeaders, ("Keep-Alive: timeout=5, max=10000"));
   curlHeaders = curl_slist_append(curlHeaders, ("Connection: Keep-Alive"));

   std::string response;
   if (body == "<empty>") {
      body.clear();
   }
   mtx_.lock();
   curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, curlHeaders);
   curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
   if (!body.empty() || (method == "POST")) {
      curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, body.data());
   }
   curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);

   const auto res = curl_easy_perform(curl_);
   mtx_.unlock();
   curl_slist_free_all(curlHeaders);

   if ((res != CURLE_OK) && (res != CURLE_RECV_ERROR)) {
      if (logger_) {
         logger_->error("[{}] failed {} to {}: {}", method, body, url, (int)res);
      }
      result->code = -res;
      return;
   }
   long respCode = 0;
   mtx_.lock();
   curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &respCode);

   if (!result->headers.empty()) {
      struct curl_header *hdr, *prev = NULL;
      while ((hdr = curl_easy_nextheader(curl_, CURLH_HEADER, 0, prev))) {
         prev = hdr;
         std::string hdrName{hdr->name};
         if (hdrName.empty()) {
            continue;
         }
         if (std::islower(hdrName[0]) && (result->headers.find(hdrName)
            == result->headers.end())) {
            hdrName[0] = std::toupper(hdrName[0]);
         }
         else if (std::isupper(hdrName[0]) && (result->headers.find(hdrName)
            == result->headers.end())) {
            hdrName[0] = std::tolower(hdrName[0]);
         }
         if (result->headers.find(hdrName) != result->headers.end()) {
            result->headers[hdrName] = hdr->value;
         }
      }
   }
   mtx_.unlock();
   if (logger_ && dumpComm_ && (dumpResponse != NoDump)) {
      dumpHeaders(result->headers);
      std::string printableResp;
      switch (dumpResponse) {
      case Text:
         if (response.size() > 4090) {
            printableResp = response.substr(0, 2000) + fmt::format("<-- {} bytes here -->"
               , response.size() - 4000) + response.substr(response.size() - 2000);
         } else {
            printableResp = response;
         }
         break;
      case OnlySize:
         printableResp = fmt::format("{} bytes", response.size());
         break;
      default: break;
      }
      logger_->debug("[{}] response: [{}] {}", method, respCode, printableResp);
   }
   result->code = respCode;
   result->response = response;
#endif
}

HttpPutHandler::HttpPutHandler(const std::shared_ptr<spdlog::logger>& logger
   , const MTLSsettings& mtls, bool dumpComm, const ProxySettings& proxy
   , const HttpBasicAuth& auth)
   : HttpHandler(logger, mtls, dumpComm, proxy, auth)
{
#ifdef BUILD_CURL
   curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "PUT");
#endif
}

std::shared_ptr<HttpOut> HttpPutHandler::processData(const std::shared_ptr<InHttpPut>& in)
{
   auto result = HttpHandler::processData(in);
   if (!result) {
      return nullptr;
   }
   handleRequest(in->method(), result, in->headers, in->url, in->params, in->data
      , in->dumpResponse, in->dumpRequest);
   return result;
}

HttpDeleteHandler::HttpDeleteHandler(const std::shared_ptr<spdlog::logger>& logger
   , const MTLSsettings& mtls, bool dumpComm, const ProxySettings& proxy
   , const HttpBasicAuth& auth)
   : HttpHandler(logger, mtls, dumpComm, proxy, auth)
{
#ifdef BUILD_CURL
   curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "DELETE");
#endif
}

std::shared_ptr<HttpOut> HttpDeleteHandler::processData(const std::shared_ptr<InHttpDelete>& in)
{
   auto result = HttpHandler::processData(in);
   if (!result) {
      return nullptr;
   }
   handleRequest(in->method(), result, in->headers, in->url, in->params, in->data
      , in->dumpResponse, in->dumpRequest);
   return result;
}


HttpWorker::HttpWorker(const std::shared_ptr<spdlog::logger>& logger
   , const MTLSsettings& mtls, bool dumpComm, const ProxySettings& proxy)
   : WorkerImpl(std::move(WorkerImpl::Handlers
      { std::make_shared<HttpGetHandler>(logger, mtls, dumpComm, proxy)
      , std::make_shared<HttpPostHandler>(logger, mtls, dumpComm, proxy)
      , std::make_shared<HttpDeleteHandler>(logger, mtls, dumpComm, proxy)
      , std::make_shared<HttpPutHandler>(logger, mtls, dumpComm, proxy)
      }))
{}


bool HttpWorkerQueue::accepts(const std::shared_ptr<InData>& in) const
{
   if (!in) {
      return false;
   }
   if ((typeid(*in) == typeid(InHttpGet)) || (typeid(*in) == typeid(InHttpPost))
      || (typeid(*in) == typeid(InHttpPut)) || (typeid(*in) == typeid(InHttpDelete)))
   {
      return true;
   }
   return false;
}
