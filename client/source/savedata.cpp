#include "savedata.hpp"

#include <string.h>

#include "fileio.hpp"
#include "zipio.hpp"

#include "utils.hpp"


int archiveSaveData(AccountUid uid, const u64 titleID, const std::string& outputPath)
{
    const Defer defer(
        []()
        {
            unmount("save");
            unmount("bcat");
        },
        []()
        {
            unmount("save");
            unmount("bcat");
        }
    );


    const int mountRet = mountSaveData("save", uid, titleID);
    if (mountRet == MOUNT_TARGET_NOT_FOUND)
    {
        // 이 계정은 그 게임을 저장한 적이 없다. 올릴 것이 없을 뿐이다.
        return SAVEDATA_NO_SAVE_DATA;
    }
    if (mountRet != 0)
    {
        return SAVEDATA_FAILED_TO_MOUNT;
    }

    const std::string stringfyTitleID = toHex(titleID);

    const std::string archivePath = outputPath + "/" + stringfyTitleID + ".sar";
    const std::string temporaryPath = archivePath + ".tmp";
    const Defer cleanup([&]() { remove(temporaryPath.c_str()); });
    ZipWriter zipWriter;
    if (!zipWriter.open(temporaryPath))
    {
        return SAVEDATA_FAILED_TO_OPEN_ARCHIVE;
    }

    bool success = true;

    const int saveWalk = walk("save:/", [&](const std::string& path, bool isDir)
    {
        if (!success) return;
        const std::string relativePath = path.substr(strlen("save:/"));
        success = isDir ? zipWriter.addDirectory("saves/" + relativePath) : zipWriter.add(path, "saves/" + relativePath);
    });

    if (!success || saveWalk != 0) return SAVEDATA_FAILED_TO_ADD_FILE;

    const int bcatResult = mountBcatSaveData("bcat", titleID);
    if (bcatResult == 0)
    {
        const int bcatWalk = walk("bcat:/", [&](const std::string& path, bool isDir)
        {
            if (!success) return;
            const std::string relativePath = path.substr(strlen("bcat:/"));
            success = isDir ? zipWriter.addDirectory("bcat/" + relativePath) : zipWriter.add(path, "bcat/" + relativePath);
        });
        if (bcatWalk != 0) success = false;
    }
    else if (bcatResult != MOUNT_TARGET_NOT_FOUND) success = false;
    if (!zipWriter.close()) success = false;

    if (!success)
    {
        return SAVEDATA_FAILED_TO_ADD_FILE;
    }

    ZipReader check;
    if (!check.open(temporaryPath) || !check.validate()) return SAVEDATA_FAILED_TO_OPEN_ARCHIVE;
    check.close();
    if (!replaceCacheFile(temporaryPath, archivePath)) return SAVEDATA_FAILED_TO_OPEN_ARCHIVE;
    return SAVEDATA_OK;
}


int archiveAllSaveData(AccountUid uid, const std::string& outputPath, const ProbeTitlesFunc& probeFunc)
{
    return archiveAllSaveData(
        uid,
        outputPath,
        probeFunc,
        [](int, int, u64)
        {
            return true;
        },
        [](int, int, int, u64)
        {
            return true;
        }
    );
}


int archiveAllSaveData(AccountUid uid, const std::string& outputPath, const ProbeTitlesFunc& probeFunc, const std::function<bool(int, int, u64)>& callback, const std::function<bool(int, int, int, u64)>& doneCallback)
{
    std::vector<u64> titleIDs;
    if (probeFunc(uid, titleIDs) != 0)
    {
        return SAVEDATA_FAILED_TO_PROBE_TITLES;
    }

    for (size_t i = 0; i < titleIDs.size(); i++)
    {
        if (!callback(titleIDs.size(), i + 1, titleIDs[i]))
            continue;

        const int ret = archiveSaveData(uid, titleIDs[i], outputPath);

        if (!doneCallback(titleIDs.size(), i + 1, ret, titleIDs[i]))
            return ret;
    }

    return SAVEDATA_OK;
}


int restoreSaveData(AccountUid uid, const u64 titleID, const std::string& sourcePath)
{
    const std::string stringfyTitleID = toHex(titleID);
    const std::string sourceFilePath = sourcePath + "/" + stringfyTitleID + ".sar";

    ZipReader zipReader;
    if (!zipReader.open(sourceFilePath) || !zipReader.validate())
    {
        return SAVEDATA_FAILED_TO_OPEN_ARCHIVE;
    }

    const Defer defer(
        []()
        {
            unmount("save");
            unmount("bcat");
        },
        []()
        {
            unmount("save");
            unmount("bcat");
        }
    );


    if (createSaveData(uid, titleID) != 0) return SAVEDATA_FAILED_TO_MOUNT;

    if (mountSaveData("save", uid, titleID) != 0)
    {
        return SAVEDATA_FAILED_TO_MOUNT;
    }

    const int bcatResult = mountBcatSaveData("bcat", titleID);
    const bool bcatMounted = bcatResult == 0;
    if (bcatResult != 0 && bcatResult != MOUNT_TARGET_NOT_FOUND) return SAVEDATA_FAILED_TO_MOUNT;
    bool needsBcat = false;
    zipReader.walk([&](const std::string& path, ZipReader::EXTRACT_ONE_FUNC) { if (startWith(path, "bcat/")) needsBcat = true; return true; });
    if (needsBcat && !bcatMounted) return SAVEDATA_FAILED_TO_MOUNT;

    const auto clearDevice = [](const std::string& name) -> bool
    {
        bool removed = true;
        const int walked = walk(name + ":/", [&](const std::string& path, bool isDir)
        {
            if ((isDir ? rmdir(path.c_str()) : remove(path.c_str())) != 0) removed = false;
        });
        return walked == 0 && removed && R_SUCCEEDED(fsdevCommitDevice(name.c_str()));
    };
    if (!clearDevice("save") || (bcatMounted && !clearDevice("bcat")))
        return SAVEDATA_FAILED_TO_EXTRACT_FILE;

    bool success = true;
    const bool walked = zipReader.walk([&](const std::string& path, ZipReader::EXTRACT_ONE_FUNC extractOneFunc)
    {
        if (!success)
        {
            return false;
        }

        if (startWith(path, "saves/"))
        {
            const std::string savePath = "save:/" + path.substr(strlen("saves/"));

            success = extractOneFunc(savePath);


            if (success)
            {
                // eager commit
                success = R_SUCCEEDED(fsdevCommitDevice("save"));
            }
        }
        else if (startWith(path, "bcat/"))
        {
            const std::string bcatPath = "bcat:/" + path.substr(strlen("bcat/"));
            // bcat extract
            success = extractOneFunc(bcatPath);
            if (success)
            {
                // eager commit
                success = R_SUCCEEDED(fsdevCommitDevice("bcat"));
            }
        }
        return success;
    });
    zipReader.close();

    if (!success || !walked)
    {
        return SAVEDATA_FAILED_TO_EXTRACT_FILE;
    }

    return SAVEDATA_OK;
}


int restoreAllSaveData(AccountUid uid, const std::string& sourcePath)
{
    return restoreAllSaveData(
        uid,
        sourcePath,
        [](int total, int current, u64 titleID)
        {
            return true;
        },
        [](int total, int current, int ret, u64 titleID)
        {
            return true;
        }
    );
}


int restoreAllSaveData(AccountUid uid, const std::string& sourcePath, const std::function<bool(int, int, u64)>& callback, const std::function<bool(int, int, int, u64)>& doneCallback)
{
    std::vector<u64> titleIDs;
    const int walked = walk(sourcePath, [&](const std::string& path, bool isDir)
    {
        if (isDir || !endWith(path, ".sar"))
        {
            return;
        }

        const std::string stringfyTitleID = path.substr(sourcePath.size() + 1, 16);
        titleIDs.push_back(fromHex<u64>(stringfyTitleID));
    });

    if (walked != 0) return SAVEDATA_FAILED_TO_PROBE_TITLES;
    int failures = 0;
    for (size_t i = 0; i < titleIDs.size(); i++)
    {
        if (!callback(titleIDs.size(), i + 1, titleIDs[i]))
            continue;

        const int ret = restoreSaveData(uid, titleIDs[i], sourcePath);
        if (ret != SAVEDATA_OK) ++failures;
        if (!doneCallback(titleIDs.size(), i + 1, ret, titleIDs[i]))
            return ret;
    }

    return failures ? -failures : SAVEDATA_OK;
}
