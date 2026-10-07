// mmap and mprotect through the console's kernel library.
//
// The payload SDK's libc.a implements both with the syscall instruction
// itself. A payload may; an app may not: the console ends it at once
// ("directly issued a syscall 477", SYSTEM_ILLEGAL_FUNCTION_CALL). The OpenGL
// runtime maps staging memory for every texture upload over 64 KB, so the
// first big texture (a 2048x2048 glyph atlas) killed the app. The link wraps
// both calls (--wrap=mmap --wrap=mprotect) and they come here, to libkernel.
//
// munmap and madvise already resolve to libkernel.

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

int sceKernelMmap(void *addr, size_t length, int protection, int flags, int fd, off_t offset, void **result);
int sceKernelMprotect(const void *addr, size_t length, int protection);

// An SCE error carries the errno in its low 16 bits.
static int to_errno(int code)
{
	return code == 0 ? 0 : (code & 0xffff);
}

void *__wrap_mmap(void *addr, size_t length, int protection, int flags, int fd, off_t offset)
{
	void *result = NULL;
	const int code = sceKernelMmap(addr, length, protection, flags, fd, offset, &result);
	if (code != 0 || result == NULL)
	{
		errno = code != 0 ? to_errno(code) : ENOMEM;
		return (void *)-1;
	}
	return result;
}

int __wrap_mprotect(void *addr, size_t length, int protection)
{
	const int code = sceKernelMprotect(addr, length, protection);
	if (code != 0)
	{
		errno = to_errno(code);
		return -1;
	}
	return 0;
}
