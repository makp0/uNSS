#include "tlsdiag.hpp"
#ifdef __SWITCH__
#include <switch.h>
#endif
namespace {
TlsDiag::Failure failure = {0, "none"};
#ifdef __SWITCH__
void record(Result rc, const char* operation) {
    // Nonblocking retry is expected; preserve the first real service failure.
    if (R_FAILED(rc) && R_VALUE(rc) != MAKERESULT(123, 204) && !failure.result)
        failure = {rc, operation};
}
#endif
}
void TlsDiag::clear() { failure = {0, "none"}; }
TlsDiag::Failure TlsDiag::lastFailure() { return failure; }
#ifdef __SWITCH__
extern "C" {
Result __real_sslConnectionRead(SslConnection*, void*, u32, u32*);
Result __real_sslConnectionWrite(SslConnection*, const void*, u32, u32*);
Result __wrap_sslConnectionRead(SslConnection* c, void* b, u32 n, u32* out) {
    Result rc = __real_sslConnectionRead(c, b, n, out);
    record(rc, "ssl-read");
    return rc;
}
Result __wrap_sslConnectionWrite(SslConnection* c, const void* b, u32 n, u32* out) {
    Result rc = __real_sslConnectionWrite(c, b, n, out);
    record(rc, "ssl-write");
    return rc;
}
}
#endif
