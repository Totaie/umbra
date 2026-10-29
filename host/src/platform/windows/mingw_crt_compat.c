/**
 * @file src/platform/windows/mingw_crt_compat.c
 * @brief CRT functions the prebuilt FFmpeg libraries still link against.
 *
 * The x264 and x265 inside third-party/build-deps were built against an older
 * mingw-w64 runtime, which exported ftime64 and an import of fseeko64. Current
 * UCRT64 runtimes no longer do, and sunshine.exe then fails to link with
 * "undefined reference to `ftime64'" and "`__imp_fseeko64'". These forward to the
 * functions UCRT does have. Built only when the toolchain lacks them (see
 * cmake/compile_definitions/windows.cmake). No headers: the current ones declare
 * these differently, or not at all.
 */
struct __timeb64;
struct _iobuf;

void __cdecl _ftime64(struct __timeb64 *);
int __cdecl _fseeki64(struct _iobuf *, long long, int);

#ifdef UMBRA_NEED_FTIME64
void ftime64(struct __timeb64 *tb) {
  _ftime64(tb);
}
#endif

#ifdef UMBRA_NEED_FSEEKO64
static int __cdecl umbra_fseeko64(struct _iobuf *file, long long offset, int whence) {
  return _fseeki64(file, offset, whence);
}

// What a dllimport reference resolves to: a pointer to the function.
int (*__imp_fseeko64)(struct _iobuf *, long long, int) = umbra_fseeko64;
#endif
