#pragma once
#include <string>
#include <vector>

#include <switch.h>
#include "http.hpp"


class IRemoteStoreIO
{
public:
    virtual int push(const std::string userName, const u64 titleId) = 0;
    virtual int pull(const std::string userName, const u64 titleId) = 0;
    virtual int push(const std::string userName, const u64 titleId, const std::string& revision) = 0;
    virtual int pull(const std::string userName, const u64 titleId, const std::string& revision) = 0;
};


// Not used yet
class IRemoteStoreQuery
{
public:
    virtual int hasTitle(const std::string userName, const u64 titleId) = 0;
    virtual int queryTitles(const std::string userName, std::vector<u64>& outTitles) = 0;
    virtual int queryRevisions(const std::string userName, const u64 titleId, std::vector<std::string>& outRevisions) = 0;
};


class HTTPRemoteStore : public IRemoteStoreIO
{
private:
    HTTPClient httpClient;
    std::function<void(const std::string&)> progressLog;
    void configureProgress(HTTPClient& client, const std::string& direction);
    std::string serverUrl;
    std::string saveDataPath;

    // 마지막으로 실패한 HTTP 호출이 돌려준 값 그대로. push / pull 은 실패를
    // 전부 -1 로 뭉개기 때문에, 그것만 보면 "서버가 400 을 줬다" 와
    // "연결조차 못 했다" 를 구별할 수 없다. 실제로 그 구별이 없어서 TLS 가
    // 열리지 않는 것을 한참 못 찾았다 (2026-07-31). 반환값 규약은 그대로
    // 두고, 원인만 여기에 남긴다.
    int lastHttpResult = 0;
    CURLcode lastCurlCode = CURLE_OK;
    const char* requestStage = "none";

public:
    HTTPRemoteStore(const std::string& serverUrl, const std::string& saveDataPath);
    ~HTTPRemoteStore();
    void setProgressCallback(const std::function<void(const std::string&)>& callback) { progressLog = callback; }

public:
    // 음수면 HTTPCLIENT_ERROR_* (연결 자체가 안 된 것), 양수면 HTTP 상태 코드.
    int getLastHttpResult() const { return lastHttpResult; }
    CURLcode getLastCurlCode() const { return lastCurlCode; }
    std::string getLastDiagnostic() const { return std::string("stage=") + requestStage + " " + httpClient.getLastDiagnostic(); }

private:
    int _issueSaveDataRevisionId(const std::string userName, const u64 titleId, std::string& revisionId);
    int _uploadSaveDataRevision(const std::string userName, const u64 titleId, const std::string& revisionId);
    int _getLatestSaveDataRevision(const std::string userName, const u64 titleId, std::string& revisionId);
    int _downloadSaveDataRevision(const std::string userName, const u64 titleId, const std::string& revisionId);

public:
    int push(const std::string userName, const u64 titleId) override;
    int pull(const std::string userName, const u64 titleId) override;
    int push(const std::string userName, const u64 titleId, const std::string& revision) override;
    int pull(const std::string userName, const u64 titleId, const std::string& revision) override;
};
