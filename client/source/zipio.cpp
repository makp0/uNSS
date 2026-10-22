#include "zipio.hpp"

#include "miniz.h"
#include "fileio.hpp"
#include <set>
#include <sstream>


ZipWriter::ZipWriter()
{
    this->zipPath = "";
    this->pZip = NULL;
}


ZipWriter::ZipWriter(ZipWriter&& other) noexcept
{
    this->zipPath = std::move(other.zipPath);
    this->pZip = other.pZip;
    other.pZip = NULL;
}


ZipWriter::ZipWriter(const std::string& zipPath)
{
    this->zipPath = zipPath;
    this->pZip = NULL;
    _openZipArchive(zipPath.c_str());
}


ZipWriter::~ZipWriter()
{
    this->close();
}


ZipWriter& ZipWriter::operator=(ZipWriter&& other) noexcept
{
    this->zipPath = std::move(other.zipPath);
    this->pZip = other.pZip;
    other.pZip = NULL;
    return *this;
}


bool ZipWriter::add(const std::string& sourcePath, const std::string& destPath)
{
    return add(sourcePath, destPath, "");
}


bool ZipWriter::add(const std::string& sourcePath, const std::string& destPath, const std::string& comment)
{
    if (pZip && mz_zip_writer_add_file((mz_zip_archive*)pZip, destPath.c_str(), sourcePath.c_str(), comment.c_str(), (mz_uint16)comment.size(), MZ_DEFAULT_COMPRESSION))
    {
        return true;
    }
    return false;
}


bool ZipWriter::addDirectory(const std::string& path)
{
    const std::string name = path.back() == '/' ? path : path + "/";
    return pZip && mz_zip_writer_add_mem((mz_zip_archive*)pZip, name.c_str(), NULL, 0, 0);
}


bool ZipWriter::open(const std::string& path)
{
    close();
    zipPath = path;
    return _openZipArchive(zipPath.c_str());
}


bool ZipWriter::close()
{
    return _closeZipArchive();
}


bool ZipWriter::_openZipArchive(const char* path)
{
    pZip = (mz_zip_archive*)malloc(sizeof(mz_zip_archive));
    if (pZip != NULL)
    {
        memset(pZip, 0, sizeof(mz_zip_archive));
        if (mz_zip_writer_init_file((mz_zip_archive*)pZip, path, 0)) return true;
        free(pZip);
        pZip = NULL;
    }
    return false;
}


bool ZipWriter::_closeZipArchive()
{
    if (!pZip) return true;
    const bool finalized = mz_zip_writer_finalize_archive((mz_zip_archive*)pZip);
    const bool ended = mz_zip_writer_end((mz_zip_archive*)pZip);
    free(pZip);
    pZip = NULL;
    zipPath.clear();
    return finalized && ended;
}


ZipReader::ZipReader()
{
    this->zipPath = "";
    this->pZip = NULL;
}


ZipReader::ZipReader(ZipReader&& other) noexcept
{
    this->zipPath = std::move(other.zipPath);
    this->pZip = other.pZip;
    other.pZip = NULL;
}


ZipReader::ZipReader(const std::string& zipPath)
{
    this->zipPath = zipPath;
    this->pZip = NULL;
    _openZipArchive(zipPath.c_str());
}


ZipReader::~ZipReader()
{
    this->close();
}


ZipReader& ZipReader::operator=(ZipReader&& other)
{
    this->zipPath = std::move(other.zipPath);
    this->pZip = other.pZip;
    other.pZip = NULL;
    return *this;
}


bool ZipReader::extractAll(const std::string& path)
{
    bool ret = true;
    const bool walked = walk([&](const std::string& fileName, ZipReader::EXTRACT_ONE_FUNC extractOne)
        {
            ret = extractOne(path + "/" + fileName);
            return ret;
        }
    );
    return walked && ret;
}


bool ZipReader::extractOne(const std::string& sourcePath, const std::string& destPath)
{
    const std::string::size_type lastSlash = destPath.rfind('/');
    if (lastSlash != std::string::npos)
    {
        const std::string parentDir = destPath.substr(0, lastSlash);
        recursiveMkdir(parentDir);
    }

    if (mz_zip_reader_extract_file_to_file((mz_zip_archive*)pZip, sourcePath.c_str(), destPath.c_str(), 0))
    {
        return true;
    }
    return false;
}


bool ZipReader::walk(const ARCHIVE_WALK_CALLBACK_FUNC& callback)
{
    if (!pZip) return false;
    const mz_uint numCount = mz_zip_reader_get_num_files((mz_zip_archive*)pZip);
    for (mz_uint i = 0; i < numCount; ++i)
    {
        mz_zip_archive_file_stat fileStat;
        if (!mz_zip_reader_file_stat((mz_zip_archive*)pZip, i, &fileStat)) return false;
        if (mz_zip_reader_get_filename((mz_zip_archive*)pZip, i, NULL, 0) > sizeof(fileStat.m_filename)) return false;
        std::string fileName(fileStat.m_filename);
        if (!callback(
            fileName,
            [this, fileName, isDir = fileStat.m_is_directory](const std::string& destPath)
            {
                if (isDir) return recursiveMkdir(destPath) == 0;
                return extractOne(fileName, destPath);
            }
        ))
        {
            return false;
        }
    }
    return true;
}


bool ZipReader::validate()
{
    if (!pZip || !mz_zip_validate_archive((mz_zip_archive*)pZip, 0)) return false;
    std::set<std::string> names;
    return walk([&](const std::string& name, EXTRACT_ONE_FUNC)
    {
        if (name.empty() || name[0] == '/' || name.find('\\') != std::string::npos
            || name.find(':') != std::string::npos || name.find('\0') != std::string::npos) return false;
        std::stringstream parts(name);
        std::string part;
        bool first = true;
        std::string normalized;
        while (std::getline(parts, part, '/'))
        {
            if (part.empty() || part == "." || part == "..") return false;
            if (first && part != "saves" && part != "bcat") return false;
            if (!normalized.empty()) normalized += "/";
            normalized += part;
            first = false;
        }
        if (normalized == "saves" || normalized == "bcat")
            return name.back() == '/' && names.insert(normalized).second;
        return !first && names.insert(normalized).second;
    });
}


bool ZipReader::open(const std::string& path)
{
    close();
    zipPath = path;
    return _openZipArchive(zipPath.c_str());
}


void ZipReader::close()
{
    _closeZipArchive();
}


bool ZipReader::_openZipArchive(const char* path)
{
    pZip = (mz_zip_archive*)malloc(sizeof(mz_zip_archive));
    if (pZip != NULL)
    {
        memset(pZip, 0, sizeof(mz_zip_archive));
        if (mz_zip_reader_init_file((mz_zip_archive*)pZip, path, 0)) return true;
        free(pZip);
        pZip = NULL;
    }
    return false;
}


void ZipReader::_closeZipArchive()
{
    if (pZip != NULL)
    {
        mz_zip_reader_end((mz_zip_archive*)pZip);
        free(pZip);
        pZip = NULL;
    }
}
