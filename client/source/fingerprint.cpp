#include "fingerprint.hpp"
#include <switch.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include <vector>

namespace {
void addSize(Sha256Context& ctx, u64 value)
{
    u8 bytes[8];
    for (int i = 7; i >= 0; --i) { bytes[i] = value & 255; value >>= 8; }
    sha256ContextUpdate(&ctx, bytes, sizeof(bytes));
}

bool hashDirectory(const std::string& root, const std::string& relative, Sha256Context& ctx)
{
    const std::string path = root + "/" + relative;
    DIR* dir = opendir(path.c_str());
    if (!dir) return false;
    std::vector<std::string> names;
    bool ok = true;
    while (true)
    {
        errno = 0;
        dirent* entry = readdir(dir);
        if (!entry) { ok = errno == 0; break; }
        const std::string name = entry->d_name;
        if (name != "." && name != "..") names.push_back(name);
    }
    if (closedir(dir) != 0) ok = false;
    if (!ok) return false;
    std::sort(names.begin(), names.end());
    // Heap allocation keeps this safe on the background service's 16 KiB stack.
    std::vector<u8> buffer(32 * 1024);
    for (const std::string& name : names)
    {
        const std::string child = relative.empty() ? name : relative + "/" + name;
        const std::string full = root + "/" + child;
        struct stat st;
        if (lstat(full.c_str(), &st) != 0) return false;
        const char kind = S_ISDIR(st.st_mode) ? 'D' : 'F';
        if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) return false;
        sha256ContextUpdate(&ctx, &kind, 1);
        addSize(ctx, child.size());
        sha256ContextUpdate(&ctx, child.data(), child.size());
        if (kind == 'D')
        {
            if (!hashDirectory(root, child, ctx)) return false;
            continue;
        }
        addSize(ctx, st.st_size);
        FILE* file = fopen(full.c_str(), "rb");
        if (!file) return false;
        u64 total = 0;
        size_t count;
        while ((count = fread(buffer.data(), 1, buffer.size(), file)) != 0)
        {
            sha256ContextUpdate(&ctx, buffer.data(), count);
            total += count;
        }
        ok = !ferror(file) && total == (u64)st.st_size;
        if (fclose(file) != 0) ok = false;
        if (!ok) return false;
    }
    return true;
}
}

bool fingerprintTree(const std::string& root, std::string& fingerprint)
{
    fingerprint.clear();
    Sha256Context ctx;
    sha256ContextCreate(&ctx);
    if (!hashDirectory(root, "", ctx)) return false;
    u8 hash[SHA256_HASH_SIZE];
    sha256ContextGetHash(&ctx, hash);
    const char* digits = "0123456789abcdef";
    for (u8 byte : hash) { fingerprint += digits[byte >> 4]; fingerprint += digits[byte & 15]; }
    return true;
}
