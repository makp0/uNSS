#include "remote.hpp"

#include <string.h>
#include <chrono>
#include <sys/stat.h>
#include <cstdio>

#include "http.hpp"
#include "zipio.hpp"
#include "fileio.hpp"

#include "utils.hpp"


#define HTTP_REMOTE_SUCCESS (0)
#define HTTP_REMOTE_NETWORK_ERROR (-1)
#define HTTP_REMOTE_SERVER_ERROR (-2)
#define HTTP_REMOTE_FILE_ERROR (-3)


HTTPRemoteStore::HTTPRemoteStore(const std::string& serverUrl, const std::string& saveDataPath)
    : serverUrl(serverUrl), saveDataPath(saveDataPath)
{
}


HTTPRemoteStore::~HTTPRemoteStore()
{
}


void HTTPRemoteStore::configureProgress(HTTPClient& client, const std::string& direction)
{
    if (!progressLog) return;
    const auto start = std::chrono::steady_clock::now();
    client.setProgressCallback([this, direction, start, last = start, lastBytes = (curl_off_t)-1](curl_off_t total, curl_off_t current) mutable
    {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - start).count();
        const double since = std::chrono::duration<double>(now - last).count();
        const bool complete = total > 0 && current >= total;
        if (current == lastBytes || (!complete && since < 2.0)) return;
        last = now;
        lastBytes = current;
        char line[192];
        const double speed = elapsed > 0 ? current / elapsed / 1024.0 : 0;
        if (total > 0)
            snprintf(line, sizeof(line), "\r%s %.1f/%.1f MiB (%lld%%), %.1f KiB/s", direction.c_str(), current / 1048576.0, total / 1048576.0, (long long)(current * 100 / total), speed);
        else
            snprintf(line, sizeof(line), "\r%s %.1f MiB, %.1f KiB/s", direction.c_str(), current / 1048576.0, speed);
        progressLog(line);
    });
}


int HTTPRemoteStore::_issueSaveDataRevisionId(const std::string userName, const u64 titleId, std::string& revisionId)
{
    HTTPClient& client = httpClient.reset();
    const std::string url = serverUrl + "/users/" + userName + "/saves/" + toHex(titleId) + "/revisions";
    client.setUrl(url).setMethod("POST")
    .setReceiveCallback([&revisionId](const void* data, size_t size, size_t& actualSize)
    {
        revisionId = std::string(static_cast<const char*>(data), size);
        actualSize = size;
        return true;
    });

    requestStage = "revision-create";
    int ret = client.perform();
    lastHttpResult = ret;
    lastCurlCode = client.getLastCurlCode();

    if (ret < 0)
    {
        return HTTP_REMOTE_NETWORK_ERROR;
    }
    else if (ret != 200)
    {
        return HTTP_REMOTE_SERVER_ERROR;
    }

    return HTTP_REMOTE_SUCCESS;
}


int HTTPRemoteStore::_uploadSaveDataRevision(const std::string userName, const u64 titleId, const std::string& revisionId)
{
    FILE* file = NULL;

    const Defer defer([&]()
    {
        if (file != NULL)
        {
            fclose(file);
        }
    });

    const std::string hexTitleId = toHex(titleId);
    const std::string filePath = saveDataPath + "/" + hexTitleId + ".sar";

    file = fopen(filePath.c_str(), "rb");
    if (file == NULL)
    {
        return HTTP_REMOTE_FILE_ERROR;
    }

    if (fseek(file, 0, SEEK_END) != 0) return HTTP_REMOTE_FILE_ERROR;
    const long fileSize = ftell(file);
    if (fileSize < 0 || fseek(file, 0, SEEK_SET) != 0) return HTTP_REMOTE_FILE_ERROR;
    HTTPClient& client = httpClient.reset();
    const std::string url = serverUrl + "/users/" + userName + "/saves/" + hexTitleId + "/revisions/" + revisionId;
    client.setUrl(url).setMethod("POST")
    .setSendCallback([&file](void* data, size_t size, size_t& actualSize)
    {
        actualSize = fread(data, 1, size, file);
        if (ferror(file)) return false;

        return true;
    });

    client.setSendSize(fileSize).setHeader("Content-Type", "application/octet-stream").setHeader("Expect", "");
    configureProgress(client, "Uploading");
    const auto started = std::chrono::steady_clock::now();
    requestStage = "archive-upload";
    int ret = client.perform();
    lastHttpResult = ret;
    lastCurlCode = client.getLastCurlCode();

    if (ret < 0)
    {
        return HTTP_REMOTE_NETWORK_ERROR;
    }
    else if (ret != 200)
    {
        return HTTP_REMOTE_SERVER_ERROR;
    }

    if (progressLog)
    {
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        char message[160];
        snprintf(message, sizeof(message), "Uploaded %.1f MiB in %.1f s (%.1f KiB/s)", fileSize / 1048576.0, elapsed, elapsed > 0 ? fileSize / elapsed / 1024.0 : 0);
        progressLog(message);
    }
    return HTTP_REMOTE_SUCCESS;
}


int HTTPRemoteStore::_getLatestSaveDataRevision(const std::string userName, const u64 titleId, std::string& revisionId)
{
    HTTPClient& client = httpClient.reset();
    const std::string url = serverUrl + "/users/" + userName + "/saves/" + toHex(titleId) + "/revisions";
    client.setUrl(url).setMethod("GET")
    .setReceiveCallback([&revisionId](const void* data, size_t size, size_t& actualSize)
    {
        char* buf = (char*)malloc(size + 1);
        strncpy(buf, static_cast<const char*>(data), size);
        buf[size] = '\0';
        revisionId = std::string(buf);
        free(buf);

        actualSize = size;
        return true;
    });

    requestStage = "revision-query";
    int ret = client.perform();
    lastHttpResult = ret;
    lastCurlCode = client.getLastCurlCode();

    if (ret < 0)
    {
        return HTTP_REMOTE_NETWORK_ERROR;
    }
    else if (ret != 200)
    {
        return HTTP_REMOTE_SERVER_ERROR;
    }

    return HTTP_REMOTE_SUCCESS;
}


int HTTPRemoteStore::_downloadSaveDataRevision(const std::string userName, const u64 titleId, const std::string& revisionId)
{
    const std::string filePath = saveDataPath + "/" + toHex(titleId) + ".sar";
    const std::string temporaryPath = filePath + ".download";
    FILE* file = NULL;

    const Defer defer([&]()
    {
        if (file != NULL)
        {
            fclose(file);
        }
        remove(temporaryPath.c_str());
    });

    file = fopen(temporaryPath.c_str(), "wb");
    if (file == NULL)
    {
        return HTTP_REMOTE_FILE_ERROR;
    }

    HTTPClient& client = httpClient.reset();
    const std::string url = serverUrl + "/users/" + userName + "/saves/" + toHex(titleId) + "/revisions/" + revisionId + "/data";
    client.setUrl(url).setMethod("GET")
    .setReceiveCallback([&file](const void* data, size_t size, size_t& actualSize)
    {
        if ((actualSize = fwrite(data, 1, size, file)) != size)
        {
            return false;
        }

        return true;
    });

    configureProgress(client, "Downloading");
    requestStage = "archive-download";
    int ret = client.perform();
    lastHttpResult = ret;
    lastCurlCode = client.getLastCurlCode();

    if (ret < 0)
    {
        return HTTP_REMOTE_NETWORK_ERROR;
    }
    else if (ret != 200)
    {
        return HTTP_REMOTE_SERVER_ERROR;
    }

    const int closed = fclose(file);
    file = NULL;
    if (closed != 0) return HTTP_REMOTE_FILE_ERROR;
    ZipReader archive;
    if (!archive.open(temporaryPath) || !archive.validate()) return HTTP_REMOTE_FILE_ERROR;
    archive.close();
    if (!replaceCacheFile(temporaryPath, filePath)) return HTTP_REMOTE_FILE_ERROR;
    return HTTP_REMOTE_SUCCESS;
}


int HTTPRemoteStore::push(const std::string userName, const u64 titleId)
{
    int ret;
    std::string revisionId;

    ret = _issueSaveDataRevisionId(userName, titleId, revisionId);
    if (ret != HTTP_REMOTE_SUCCESS)
    {
        return -1;
    }

    return push(userName, titleId, revisionId);
}


int HTTPRemoteStore::pull(const std::string userName, const u64 titleId)
{
    int ret;
    std::string revisionId;

    ret = _getLatestSaveDataRevision(userName, titleId, revisionId);
    if (ret != HTTP_REMOTE_SUCCESS)
    {
        return -1;
    }

    return pull(userName, titleId, revisionId);
}


int HTTPRemoteStore::push(const std::string userName, const u64 titleId, const std::string& revision)
{
    int ret;

    ret = _uploadSaveDataRevision(userName, titleId, revision);
    if (ret != HTTP_REMOTE_SUCCESS)
    {
        return -1;
    }

    return 0;
}


int HTTPRemoteStore::pull(const std::string userName, const u64 titleId, const std::string& revision)
{
    int ret;

    ret = _downloadSaveDataRevision(userName, titleId, revision);
    if (ret != HTTP_REMOTE_SUCCESS)
    {
        return -1;
    }

    return 0;
}
