#pragma once
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <curl/curl.h>

// HTTP 클라이언트 에러 코드
#define HTTPCLIENT_ERROR_INIT_FAILED    -1
#define HTTPCLIENT_ERROR_UNSUPPORTED    -100

// libcurl 에러 코드
#define HTTPCLIENT_ERROR_CURL_URL       -2    // CURLcode::CURLE_URL_MALFORMAT
#define HTTPCLIENT_ERROR_CURL_CONNECT   -3    // CURLcode::CURLE_COULDNT_CONNECT
#define HTTPCLIENT_ERROR_CURL_DNS       -4    // CURLcode::CURLE_COULDNT_RESOLVE_HOST
#define HTTPCLIENT_ERROR_CURL_SSL       -5    // CURLcode::CURLE_SSL_CONNECT_ERROR
#define HTTPCLIENT_ERROR_CURL_TIMEOUT   -6    // CURLcode::CURLE_OPERATION_TIMEDOUT
#define HTTPCLIENT_ERROR_CURL_MEMORY    -7    // CURLcode::CURLE_OUT_OF_MEMORY
#define HTTPCLIENT_ERROR_CURL_OTHER     -8    // 기타 libcurl 에러


class HTTPClient
{
private:
    CURL* curl;
    std::string url;
    std::string method;
    std::map<std::string, std::string> headers;
    struct curl_slist* curl_headers;

    std::function<bool(const void* data, size_t size, size_t& actualSize)> onReceive;
    std::function<bool(void* data, size_t size, size_t& actualSize)> onSend;

    CURLcode lastCurlCode = CURLE_OK;
    std::string lastDiagnostic;
    curl_off_t sendSize = -1;
    std::function<void(curl_off_t, curl_off_t)> onProgress;
    static int progressCallback(void* userp, curl_off_t downTotal, curl_off_t downNow, curl_off_t upTotal, curl_off_t upNow);

    // libcurl 콜백 함수
    static size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp);
    static size_t readCallback(void* ptr, size_t size, size_t nmemb, void* userp);

public:
    HTTPClient();
    ~HTTPClient();

public:
    // 인증서 검증을 켜고 끈다. 프로세스 전체에 적용되며 기본값은 켜짐.
    //
    // 자격증명은 URL 에 들어 있고 libcurl 이 그것을 Authorization 헤더로 먼저
    // 보낸다. 검증이 꺼져 있으면 핸드셰이크에 응답하는 누구나 평문 비밀번호를
    // 받는다 - bcrypt 는 서버에 저장된 해시를 지킬 뿐 이 구간과는 무관하다.
    //
    // 끄는 길을 남겨둔 이유는 하나다: 이 curl 은 libnx SSL 백엔드라 콘솔에
    // 내장된 인증서 목록으로 검증하고, 그 목록은 펌웨어와 함께만 갱신된다.
    // 사설 CA 나 콘솔이 모르는 루트를 쓰면 서버가 멀쩡해도 거절당하고, 그
    // 실패는 "연결 안 됨" 처럼 보인다. 그때는 sdmc:/uNSS/cacert.pem 에 루트를
    // 두는 것이 먼저고, 이 스위치는 마지막 수단이다.
    static void setVerifyTls(bool enabled);

public:
    CURLcode getLastCurlCode() const { return lastCurlCode; }
    const std::string& getLastDiagnostic() const { return lastDiagnostic; }
    HTTPClient& reset();
    HTTPClient& setUrl(const std::string& url);
    HTTPClient& setMethod(const std::string& method);
    HTTPClient& setHeader(const std::string& key, const std::string& value);
    HTTPClient& setReceiveCallback(const std::function<bool(const void* data, size_t size, size_t& actualSize)>& onReceive);
    HTTPClient& setSendCallback(const std::function<bool(void* data, size_t size, size_t& actualSize)>& onSend);
    HTTPClient& setSendSize(curl_off_t size);
    HTTPClient& setProgressCallback(const std::function<void(curl_off_t, curl_off_t)>& callback);
    int perform();
};
