#include "sync.hpp"

#include <cstdio>
#include <cerrno>
#include <ctime>
#include <vector>
#include <map>
#include "fingerprint.hpp"

#include <sys/stat.h>

#include "fileio.hpp"
#include "remote.hpp"
#include "savedata.hpp"
#include "title.hpp"
#include "utils.hpp"


namespace
{

std::string lastSyncPath(const std::string& saveDataPath)
{
    return saveDataPath + "/.lastautosync";
}


std::string titleNameOrUnknown(u64 titleID)
{
    std::string titleName;
    if (getTitleName(titleID, titleName) != 0)
        titleName = "Unknown";
    return titleName;
}


// 어떤 타이틀이 마지막으로 어떤 상태였는지 적어두는 파일.
// 한 줄에 "타이틀ID 시각" 형식.
std::string syncStatePath(const std::string& saveDataPath)
{
    return saveDataPath + "/.syncstate-sha256-v1";
}


std::string toHexId(u64 titleID)
{
    char buffer[17];
    snprintf(buffer, sizeof(buffer), "%016llX", (unsigned long long)titleID);
    return std::string(buffer);
}


bool saveFingerprint(const AccountUid uid, u64 titleID, std::string& fingerprint, bool* missing = nullptr)
{
    fingerprint.clear();
    if (missing) *missing = false;
    const int mounted = mountSaveData("unsschk", uid, titleID);
    if (mounted != 0) { if (missing) *missing = mounted == MOUNT_TARGET_NOT_FOUND; return false; }
    std::string account;
    const bool ok = fingerprintTree("unsschk:/", account);
    unmount("unsschk");
    if (!ok) return false;
    const int bcat = mountBcatSaveData("unssbchk", titleID);
    std::string extra = "absent";
    if (bcat == 0)
    {
        const bool bcatOk = fingerprintTree("unssbchk:/", extra);
        unmount("unssbchk");
        if (!bcatOk) return false;
    }
    else if (bcat != MOUNT_TARGET_NOT_FOUND) return false;
    fingerprint = account + ":" + extra;
    return true;
}

std::string readSyncedFingerprint(const std::string& saveDataPath, u64 titleID)
{
    FILE* fp = fopen(syncStatePath(saveDataPath).c_str(), "r");
    if (!fp && errno == ENOENT) fp = fopen((syncStatePath(saveDataPath) + ".previous").c_str(), "r");
    if (!fp) return "";
    const std::string wanted = toHexId(titleID);
    char id[32], fingerprint[160];
    std::string found;
    while (fscanf(fp, "%31s %159s", id, fingerprint) == 2)
        if (wanted == id) { found = fingerprint; break; }
    fclose(fp);
    return found;
}

bool writeSyncedFingerprint(const std::string& saveDataPath, u64 titleID, const std::string& fingerprint)
{
    const std::string path = syncStatePath(saveDataPath);
    const std::string wanted = toHexId(titleID);
    std::string rebuilt;
    FILE* fp = fopen(path.c_str(), "r");
    if (fp)
    {
        char id[32], existing[160];
        while (fscanf(fp, "%31s %159s", id, existing) == 2)
            if (wanted != id) rebuilt += std::string(id) + " " + existing + "\n";
        if (ferror(fp)) { fclose(fp); return false; }
        fclose(fp);
    }
    rebuilt += wanted + " " + fingerprint + "\n";
    const std::string temporary = path + ".tmp";
    fp = fopen(temporary.c_str(), "w");
    if (!fp) return false;
    bool ok = fwrite(rebuilt.data(), 1, rebuilt.size(), fp) == rebuilt.size();
    if (fclose(fp) != 0) ok = false;
    if (ok) ok = replaceCacheFile(temporary, path);
    if (!ok) remove(temporary.c_str());
    return ok;
}

} // namespace


bool isGameRunning()
{
    if (R_FAILED(pmdmntInitialize()))
        return false;

    u64 pid = 0;
    const Result rc = pmdmntGetApplicationProcessId(&pid);
    pmdmntExit();

    // 실행 중인 애플리케이션이 없으면 실패를 돌려준다.
    return R_SUCCEEDED(rc) && pid != 0;
}


bool hasSaveDataChanged(const SyncOptions& options, u64 titleID)
{
    std::string current;
    bool missing = false;
    if (!saveFingerprint(options.uid, titleID, current, &missing)) return !missing;
    return current != readSyncedFingerprint(options.saveDataPath, titleID);
}


namespace
{

// 백업 대상 타이틀 목록. pushAllSaves 와 countChangedTitles 가 같은 기준을
// 써야 "바뀐 게 없다" 와 "올릴 게 없다" 가 어긋나지 않는다.
int collectTargetTitles(const SyncOptions& options, AccountUid uid, std::vector<u64>& titleIDs)
{
    const int ret = options.archiveBy == "all"
        ? probeAllTitles(uid, titleIDs)
        : probeSaveDataCreatedTitles(uid, titleIDs);
    if (ret != 0) return ret;

    filterExcludedTitles(titleIDs, options.excludedTitleIds, options.excludedTitleNames);
    return 0;
}

} // namespace


int countChangedTitles(const SyncOptions& options)
{
    std::vector<u64> titleIDs;
    if (collectTargetTitles(options, options.uid, titleIDs) != 0) return -1;

    if (!options.skipUnchanged) return (int)titleIDs.size();

    int changed = 0;
    for (const u64 titleID : titleIDs)
    {
        if (hasSaveDataChanged(options, titleID))
            ++changed;
    }

    return changed;
}


int pushAllSaves(const SyncOptions& options, SyncLogFunc log)
{
    HTTPRemoteStore remoteStore(options.serverUrl, options.saveDataPath);
    remoteStore.setProgressCallback(log);
    recursiveMkdir(options.saveDataPath.c_str());

    const ProbeTitlesFunc probeFunc = [&](const AccountUid probeUid, std::vector<u64>& titleIDs) -> int
    {
        const int ret = collectTargetTitles(options, probeUid, titleIDs);
        if (ret != 0) return ret;

        // 안 바뀐 타이틀은 압축조차 하지 않는다. 여기서 걸러야 의미가 있다.
        if (options.skipUnchanged)
        {
            std::vector<u64> changed;
            changed.reserve(titleIDs.size());

            for (const u64 titleID : titleIDs)
            {
                if (hasSaveDataChanged(options, titleID))
                    changed.push_back(titleID);
            }

            const size_t skipped = titleIDs.size() - changed.size();
            if (skipped > 0)
                log("Skipping " + std::to_string(skipped) + " unchanged title(s)");

            titleIDs.swap(changed);
        }

        return 0;
    };

    // 개별 타이틀의 실패는 콜백 안에서만 보인다. archiveAllSaveData 는 목록을
    // 훑는 데 성공하면 OK 를 주기 때문에, 세어두지 않으면 서버가 아예 죽어
    // 있어도 이 함수는 0 을 돌려준다. 그러면 호출하는 쪽이 백업을 마쳤다고
    // 믿고 마지막 시각을 남기고, 24 시간 동안 다시 시도하지 않는다.
    int failures = 0;
    std::map<u64, std::string> archivedFingerprints;

    log("Checking save contents (SHA-256)...");
    const int ret = archiveAllSaveData(
        options.uid,
        options.saveDataPath,
        probeFunc,
        [&](int total, int current, u64 titleID) -> bool
        {
            log("[" + padding(current, 3) + "/" + padding(total, 3) + "] " + titleNameOrUnknown(titleID));
            std::string fingerprint;
            bool missing = false;
            if (!saveFingerprint(options.uid, titleID, fingerprint, &missing))
            {
                if (missing) return true;
                log("Cannot read complete save contents - not marked as backed up");
                ++failures;
                return false;
            }
            archivedFingerprints[titleID] = fingerprint;
            return true;
        },
        [&](int total, int current, int ret, u64 titleID) -> bool
        {
            if (ret == SAVEDATA_NO_SAVE_DATA)
            {
                // 이 계정은 그 게임을 저장한 적이 없다. 실패가 아니므로 세지
                // 않는다 - 세면 한 바퀴가 늘 "오류로 끝남" 이 되고, 그러면
                // 마지막 성공 시각이 남지 않아 다음 바퀴가 전부를 다시 한다.
                log("No save data for this account - skipped");
            }
            else if (ret != SAVEDATA_OK)
            {
                log("Failed to archive, ret=" + std::to_string(ret));
                ++failures;
                if (options.stopOnUploadError) return false;
            }
            else
            {
                std::string afterArchive;
                if (!saveFingerprint(options.uid, titleID, afterArchive) || afterArchive != archivedFingerprints[titleID])
                {
                    log("Save changed while archiving - will retry");
                    ++failures;
                    return true;
                }
                int pushRet = remoteStore.push(options.nickname, titleID);
                if (pushRet != 0)
                {
                    // ret 은 늘 -1 이라 아무것도 말해주지 않는다. 뒤의 값이
                    // 진짜 원인이다: 음수면 연결 자체가 안 된 것
                    // (HTTPCLIENT_ERROR_*, 예: -5 = TLS), 양수면 서버가
                    // 돌려준 상태 코드다.
                    log("Failed to push, ret=" + std::to_string(pushRet)
                        + " http=" + std::to_string(remoteStore.getLastHttpResult())
                        + " curl=" + std::to_string((int)remoteStore.getLastCurlCode())
                        + " (" + curl_easy_strerror(remoteStore.getLastCurlCode()) + ") " + remoteStore.getLastDiagnostic());
                    ++failures;
                    if (options.stopOnUploadError) return false;
                }
                else if (options.skipUnchanged)
                {
                    // 성공한 것만 기록한다. 실패한 타이틀은 다음에 다시 올라간다.
                    if (!writeSyncedFingerprint(options.saveDataPath, titleID, archivedFingerprints[titleID]))
                    {
                        log("Upload succeeded but sync state could not be saved - will retry");
                        ++failures;
                    }
                }
            }
            return true;
        }
    );

    if (ret != 0) return ret;

    // 실패한 타이틀 수를 음수로 돌려준다. SAVEDATA_* 코드는 양수라 서로
    // 헷갈리지 않는다. 0 은 "하나도 빠짐없이 올라갔다" 는 뜻이고,
    // 마지막 백업 시각은 그때만 남겨야 한다.
    return failures > 0 ? -failures : 0;
}


int pullAllSaves(const SyncOptions& options, SyncLogFunc log)
{
    HTTPRemoteStore remoteStore(options.serverUrl, options.saveDataPath);
    remoteStore.setProgressCallback(log);
    recursiveMkdir(options.saveDataPath.c_str());

    if (!options.remoteEnabled)
    {
        log("Remote is disabled, restoring from local...");
        return restoreAllSaveData(
            options.uid, options.saveDataPath,
            [&log](int total, int current, u64 titleID) -> bool
            {
                log("[" + padding(current, 3) + "/" + padding(total, 3) + "] " + titleNameOrUnknown(titleID));
                return true;
            },
            [&log](int total, int current, int ret, u64 titleID) -> bool
            {
                if (ret != SAVEDATA_OK)
                    log("Failed to restore, ret=" + std::to_string(ret));
                return true;
            }
        );
    }

    std::vector<u64> titleIDs;
    int probeRet = options.restoreBy == "all"
        ? probeAllTitles(options.uid, titleIDs)
        : probeSaveDataCreatedTitles(options.uid, titleIDs);
    if (probeRet != 0)
    {
        log("Failed to probe titles");
        return probeRet;
    }
    filterExcludedTitles(titleIDs, options.excludedTitleIds, options.excludedTitleNames);

    int failures = 0;
    for (size_t i = 0; i < titleIDs.size(); ++i)
    {
        log("[" + padding(i + 1, 3) + "/" + padding(titleIDs.size(), 3) + "] " + titleNameOrUnknown(titleIDs[i]));

        if (remoteStore.pull(options.nickname, titleIDs[i]) != 0)
        {
            log("Failed to pull from server");
            ++failures;
        }
        else
        {
            const int restored = restoreSaveData(options.uid, titleIDs[i], options.saveDataPath);
            if (restored != SAVEDATA_OK) { log("Restore failed, ret=" + std::to_string(restored)); ++failures; }
        }
    }

    return failures ? -failures : 0;
}


time_t readLastAutoSyncTime(const std::string& saveDataPath)
{
    FILE* fp = fopen(lastSyncPath(saveDataPath).c_str(), "r");
    if (!fp) return 0;

    long long value = 0;
    if (fscanf(fp, "%lld", &value) != 1)
        value = 0;
    fclose(fp);

    return (time_t)value;
}


void writeLastAutoSyncTime(const std::string& saveDataPath, time_t when)
{
    recursiveMkdir(saveDataPath.c_str());

    FILE* fp = fopen(lastSyncPath(saveDataPath).c_str(), "w");
    if (!fp) return;

    fprintf(fp, "%lld", (long long)when);
    fclose(fp);
}


bool isAutoSyncDue(const std::string& saveDataPath, int intervalHours)
{
    if (intervalHours <= 0) return true;

    const time_t last = readLastAutoSyncTime(saveDataPath);
    if (last == 0) return true;

    const time_t now = time(NULL);
    // 시스템 시계가 뒤로 간 경우 (RTC 재설정 등) 그냥 실행한다.
    if (now < last) return true;

    return (now - last) >= (time_t)intervalHours * 3600;
}
