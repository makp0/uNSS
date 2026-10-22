#include "fileio.hpp"

#include <string>
#include <functional>
#include <memory>
#include <new>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>


int walk(const std::string& path, std::function<void(const std::string&, bool isDir)> callback)
{
    DIR* dir = opendir(path.c_str());
    if (dir == NULL)
    {
        return -1;
    }

    dirent* entry;
    errno = 0;
    while ((entry = readdir(dir)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }

        const std::string fullPath = path[path.size() - 1] == '/' ? path + entry->d_name : path + "/" + entry->d_name;

        if (entry->d_type == DT_DIR)
        {
            if (walk(fullPath, callback) != 0) { closedir(dir); return -1; }
            callback(fullPath, true);
        }
        else
        {
            callback(fullPath, false);
        }
        errno = 0;
    }

    const bool ok = errno == 0;
    return closedir(dir) == 0 && ok ? 0 : -1;
}


int recursiveMkdir(const std::string& path, mode_t mode) 
{
    std::string::size_type pos = path.find(':');
    if (pos == std::string::npos) 
    {
        return -1;
    }
    
    pos = path.find('/', pos);
    if (pos == std::string::npos) 
    {
        return 0;
    }
    
    while ((pos = path.find('/', pos + 1)) != std::string::npos) 
    {
        std::string subpath = path.substr(0, pos);
        int status = mkdir(subpath.c_str(), mode);
        if (status != 0 && errno != EEXIST) 
        {
            return status;
        }
    }
    
    int status = mkdir(path.c_str(), mode);
    if (status != 0 && errno != EEXIST) 
    {
        return status;
    }
    
    return 0;
}


bool replaceCacheFile(const std::string& temporary, const std::string& destination)
{
    const std::string previous = destination + ".previous";
    struct stat st;
    if (stat(destination.c_str(), &st) != 0)
    {
        if (errno != ENOENT) return false;
        if (stat(previous.c_str(), &st) == 0)
        {
            if (rename(previous.c_str(), destination.c_str()) != 0) return false;
        }
        else if (errno != ENOENT) return false;
    }
    if (rename(temporary.c_str(), destination.c_str()) == 0)
    {
        remove(previous.c_str());
        return true;
    }
    if (errno != EEXIST) return false;
    // Horizon rename does not replace an existing file. Keep the old file until install succeeds.
    if (remove(previous.c_str()) != 0 && errno != ENOENT) return false;
    if (rename(destination.c_str(), previous.c_str()) != 0) return false;
    if (rename(temporary.c_str(), destination.c_str()) != 0)
    {
        rename(previous.c_str(), destination.c_str());
        return false;
    }
    remove(previous.c_str());
    return true;
}


int createSaveData(const AccountUid accountUid, const u64 titleID)
{
    // 0x24000 - 144KB 짜리다. 시스템 모듈 스택은 16KB 뿐이라 스택에 두면
    // 넘친다. 던지지 않는 new 를 쓰는 이유도 title.cpp 의 같은 구조체를 참고.
    const std::unique_ptr<NsApplicationControlData> controlDataHolder(
        new (std::nothrow) NsApplicationControlData());
    if (!controlDataHolder) return -1;

    NsApplicationControlData& controlData = *controlDataHolder;

    size_t actualSize;

    Result rc = nsInitialize();
    if (R_FAILED(rc))
    {
        return -1;
    }

    rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, titleID, &controlData, sizeof(controlData), &actualSize);
    nsExit();

    if (R_FAILED(rc))
    {
        return -1;
    }

    s64 saveDataSize = controlData.nacp.user_account_save_data_size;
    s64 journalSize = controlData.nacp.user_account_save_data_journal_size;

    if (saveDataSize == 0)
    {
        saveDataSize = 0x40000;  // 256KB fallback
    }
    if (journalSize == 0)
    {
        journalSize = 0x40000;  // 256KB fallback
    }

    FsSaveDataAttribute attr = {};
    attr.application_id = titleID;
    attr.uid = accountUid;
    attr.save_data_type = FsSaveDataType_Account;

    FsSaveDataCreationInfo creation = {};
    creation.save_data_size = saveDataSize;
    creation.journal_size = journalSize;
    creation.available_size = 0x4000;
    creation.owner_id = titleID;
    creation.save_data_space_id = FsSaveDataSpaceId_User;

    FsSaveDataMetaInfo meta = {};

    rc = fsCreateSaveDataFileSystem(&attr, &creation, &meta);
    if (R_FAILED(rc))
    {
        // 0x22CA 오류 -> 이미 세이브데이터 파일시스템 있으므로 무시
        if (R_VALUE(rc) == 0x22CA)
        {
            return 0;
        }
        return -1;
    }

    return 0;
}


int mountSaveData(const std::string& mountPoint, const AccountUid accountUid, const u64 titleID)
{
    Result rc = fsdevMountSaveData(mountPoint.c_str(), titleID, accountUid);
    if (R_FAILED(rc))
    {
        // "그런 세이브는 없다" 와 "열지 못했다" 는 다른 일이다. 앞의 것은
        // 이 계정이 그 게임을 한 번도 저장하지 않았다는 뜻일 뿐이고, 흔하다.
        // 둘을 뭉뚱그리면 정상적인 백업이 매번 오류로 끝난다.
        if (R_MODULE(rc) == 2 && R_DESCRIPTION(rc) == 1002)   // fs: TargetNotFound
        {
            return MOUNT_TARGET_NOT_FOUND;
        }

        return -1;
    }

    return 0;
}


int mountBcatSaveData(const std::string& mountPoint, const u64 titleID)
{
    Result rc = fsdevMountBcatSaveData(mountPoint.c_str(), titleID);
    if (R_FAILED(rc))
    {
        if (R_MODULE(rc) == 2 && R_DESCRIPTION(rc) == 1002) return MOUNT_TARGET_NOT_FOUND;
        return -1;
    }

    return 0;
}


int unmount(const std::string& mountPoint)
{
    Result rc = fsdevUnmountDevice(mountPoint.c_str());
    if (R_FAILED(rc))
    {
        return -1;
    }

    return 0;
}