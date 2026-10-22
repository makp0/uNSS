#pragma once

#include <functional>
#include <string>

#include <switch.h>


// 동기화 진행 상황을 알리는 콜백. GUI 든 콘솔이든 동일하게 사용한다.
using SyncLogFunc = std::function<void(const std::string&)>;


struct SyncOptions
{
    AccountUid uid = {};
    std::string nickname;
    std::string saveDataPath;
    std::string serverUrl;
    bool remoteEnabled = false;

    // "created" 또는 "all"
    std::string archiveBy = "created";
    std::string restoreBy = "all";

    std::string excludedTitleIds;
    std::string excludedTitleNames;

    // 마지막 업로드 이후 바뀌지 않은 타이틀은 건너뛴다.
    bool skipUnchanged = true;
    bool stopOnUploadError = false;
};


// 게임이 실행 중인가. 실행 중이면 세이브가 열려 있을 수 있어
// 그 상태의 백업은 일관성을 보장하지 못한다.
bool isGameRunning();

// Compare Account/BCAT paths and content SHA-256 against the successful snapshot.
bool hasSaveDataChanged(const SyncOptions& options, u64 titleID);

// 올릴 것이 있는 타이틀 수. 네트워크를 열기 전에 물어볼 수 있다 -
// Content is hashed locally. Returns -1 if the title list cannot be read.
//
// 0 이면 정말로 할 일이 없다는 뜻이고, 그러면 소켓도 무선랜도 건드릴
// 이유가 없다. 시스템 모듈에서는 그 차이가 크다.
int countChangedTitles(const SyncOptions& options);

// 모든 세이브를 아카이브한 뒤 서버로 업로드한다.
int pushAllSaves(const SyncOptions& options, SyncLogFunc log);

// 서버에서 내려받아 복원한다. remoteEnabled 가 false 면 로컬 아카이브에서 복원한다.
int pullAllSaves(const SyncOptions& options, SyncLogFunc log);


// 마지막 자동 동기화 시각 (Unix time) 을 기록하는 파일. 없으면 0 을 반환한다.
time_t readLastAutoSyncTime(const std::string& saveDataPath);
void writeLastAutoSyncTime(const std::string& saveDataPath, time_t when);

// intervalHours 가 지났는지 확인한다. intervalHours <= 0 이면 항상 true.
bool isAutoSyncDue(const std::string& saveDataPath, int intervalHours);
