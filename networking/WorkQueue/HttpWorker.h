#ifndef CONVO_HTTP_WORKER_H
#define CONVOHTTP_WORKER_H

#include <map>
#include <unordered_set>
#include "WorkQueue/Worker.h"

namespace spdlog {
   class logger;
}

namespace convo {

   struct MTLSsettings
   {
      std::string caRemoteCert;
      std::string ourKey;
      std::string ourCert;
      std::string ourCertPass;

      bool empty() const { return (caRemoteCert.empty() && ourKey.empty() &&
         ourCert.empty()); }
   };

   struct ProxySettings
   {
      std::string url;
      std::string rootCA;
      std::string sslCert;
      std::string sslKey;
      std::string jwtPrivKey;
      std::string jwtKId;
      std::string merchantId;
   };

   struct HttpBasicAuth
   {
      std::string user;
      std::string password;
      bool empty() const { return user.empty(); }
   };

   using HttpHeaders = std::map<std::string, std::string>;
   using HttpParams = std::map<std::string, std::string>;
   using HttpHeaderNames = std::unordered_set<std::string>;

   enum DumpOptions
   {
      NoDump = 0,
      Text,
      OnlySize
   };

   struct HttpIn : public InData
   {
      ~HttpIn() override = default;
      std::string name() const override { return "HttpIn"; }
      virtual std::string method() const = 0;
      std::string url;
      HttpHeaders headers{};
      HttpParams  params{};
      HttpHeaderNames responseHdr{};

      DumpOptions dumpResponse{ Text };
      DumpOptions dumpRequest{ Text };
   };
   struct InHttpGet : public HttpIn
   {
      std::string name() const override { return "HttpGet"; }
      std::string method() const override { return "GET"; }
      ~InHttpGet() override = default;
   };
   struct InHttpPut : public HttpIn
   {
      std::string name() const override { return "HttpPut"; }
      std::string method() const override { return "PUT"; }
      ~InHttpPut() override = default;
      std::string data{};
   };
   struct InHttpPost : public HttpIn
   {
      std::string name() const override { return "HttpPost"; }
      std::string method() const override { return "POST"; }
      ~InHttpPost() override = default;
      std::string data{};
   };
   struct InHttpDelete : public HttpIn
   {
      std::string name() const override { return "HttpDelete"; }
      std::string method() const override { return "DELETE"; }
      ~InHttpDelete() override = default;
      std::string data{};
   };
   struct HttpOut : public OutData
   {
      ~HttpOut() override = default;
      std::string response;
      long        code{ 0 };
      HttpHeaders headers{};
   };

   using HttpCallback = std::function<void(long, const std::string&, const HttpHeaders&)>;
   std::function<ProcessingResult(const std::shared_ptr<OutData>&)>
      makeHttpCallback(const HttpCallback&);

   class HttpHandler
   {
   public:
      HttpHandler(const std::shared_ptr<spdlog::logger>&, const MTLSsettings&
         , bool dumpComm, const ProxySettings& proxySettings = {}
         , const HttpBasicAuth& auth = {});
      ~HttpHandler();

      static std::string urlEncoded(const HttpParams&);

   protected:
      void dumpHeaders(const HttpHeaders&);
      std::shared_ptr<HttpOut> processData(const std::shared_ptr<HttpIn>&);
      void handleRequest(const std::string& method, const std::shared_ptr<HttpOut>&
         , HttpHeaders&, std::string url, const HttpParams&, std::string body = {}
         , DumpOptions dumpResponse = Text, DumpOptions dumpRequest = Text);

   protected:
      std::shared_ptr<spdlog::logger>  logger_;
      const bool dumpComm_;
      const std::string jwtPrivKey_;
      const std::string merchantId_;
#ifdef BUILD_CURL
      void* curl_{ nullptr };
#endif
      mutable std::mutex   mtx_;
#ifdef BUILD_JWT
      std::string jwtKey_, jwtKid_;
#endif
   };

   class HttpGetHandler : public HandlerImpl<InHttpGet, HttpOut>, public HttpHandler
   {
   public:
      HttpGetHandler(const std::shared_ptr<spdlog::logger>&, const MTLSsettings&
         , bool dumpComm = true, const ProxySettings& proxySettings = {}
         , const HttpBasicAuth& auth = {});
      ~HttpGetHandler() override = default;

      std::shared_ptr<HttpOut> processData(const std::shared_ptr<InHttpGet>&) override;
   };


   class HttpDeleteHandler : public HandlerImpl<InHttpDelete, HttpOut>, public HttpHandler
   {
   public:
      HttpDeleteHandler(const std::shared_ptr<spdlog::logger>&, const MTLSsettings&
         , bool dumpComm = true, const ProxySettings& proxySettings = {}
         , const HttpBasicAuth& auth = {});
      ~HttpDeleteHandler() override = default;

      std::shared_ptr<HttpOut> processData(const std::shared_ptr<InHttpDelete>&) override;
   };


   class HttpPutHandler : public HandlerImpl<InHttpPut, HttpOut>, public HttpHandler
   {
   public:
      HttpPutHandler(const std::shared_ptr<spdlog::logger>&, const MTLSsettings&
         , bool dumpComm = true, const ProxySettings& proxySettings = {}
         , const HttpBasicAuth& auth = {});
      ~HttpPutHandler() override = default;

      std::shared_ptr<HttpOut> processData(const std::shared_ptr<InHttpPut>&) override;
   };


   class HttpPostHandler : public HandlerImpl<InHttpPost, HttpOut>, public HttpHandler
   {
   public:
      HttpPostHandler(const std::shared_ptr<spdlog::logger>& logger
         , const MTLSsettings& mtls, bool dumpComm = true
         , const ProxySettings& proxySettings = {}, const HttpBasicAuth& auth = {})
         : HttpHandler(logger, mtls, dumpComm, proxySettings, auth) {}
      ~HttpPostHandler() override = default;

      std::shared_ptr<HttpOut> processData(const std::shared_ptr<InHttpPost>&) override;
   };

   using MkWorkerFn = std::function<std::shared_ptr<Worker>(bool)>;

   class HttpWorker : public WorkerImpl
   {
   public:
      HttpWorker(const std::shared_ptr<spdlog::logger>&, const MTLSsettings&
         , bool dumpComm = true, const ProxySettings& proxySettings = {});
   };

   class HttpWorkerQueue : public WorkerQueue
   {
   public:
      bool accepts(const std::shared_ptr<InData>&) const override;
   };

}
#endif // CONVO_HTTP_WORKER_H
