#pragma once
#include <string>

// SHA-256 of sorted relative paths and file contents, including empty directories.
// On any read or traversal error no fingerprint is returned.
bool fingerprintTree(const std::string& root, std::string& fingerprint);
