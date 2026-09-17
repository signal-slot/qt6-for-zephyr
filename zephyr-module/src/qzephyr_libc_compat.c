/*
 * Weak libc fallbacks for Qt code the port links but never runs usefully.
 *
 * Qt Quick 3D's asset importer plugin (Assimp and its zip reader) is pulled in
 * by examples that import QtQuick3D.AssetUtils or bring a runtime-loaded model.
 * It calls the LFS stdio entry points (fopen64, fseeko64, ftello64), chdir, and
 * QFileDialog's tilde expansion calls getpwnam. picolibc on this target has
 * none of them. Stage 2 links with --unresolved-symbols=ignore-all, but an
 * undefined function still fails the link on AArch64 ("relocation truncated
 * to fit: R_AARCH64_CALL26"): address 0 is out of branch range of the image.
 * The 64-bit variants map to the plain calls (off_t is 64-bit here); the
 * others report failure. All weak, so a libc that has them wins.
 */
#include <errno.h>
#include <stdio.h>
#include <stddef.h>
#include <sys/types.h>

__attribute__((weak)) FILE *fopen64(const char *path, const char *mode)
{
	return fopen(path, mode);
}

__attribute__((weak)) int fseeko64(FILE *stream, long long offset, int whence)
{
	return fseeko(stream, (off_t)offset, whence);
}

__attribute__((weak)) long long ftello64(FILE *stream)
{
	return (long long)ftello(stream);
}

__attribute__((weak)) int chdir(const char *path)
{
	(void)path;
	errno = ENOSYS;
	return -1;
}

struct passwd;
__attribute__((weak)) struct passwd *getpwnam(const char *name)
{
	(void)name;
	return NULL;
}
