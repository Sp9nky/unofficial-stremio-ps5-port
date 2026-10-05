// POSIX calls that behave differently inside a PS5 title's sandbox. Both are
// adapted from the Kodi port for PS5 (shims/native-app/thread_stack.c and
// pipe_fallback.c, GPL-2.0-or-later), where each problem was found on
// hardware. Linked with --wrap=pthread_create --wrap=pipe (native/build.sh).

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

int sceKernelDebugOutText(int channel, const char* text);

// ---- Thread stacks ---------------------------------------------------------------------------
// A title's default thread stack is 64 KB. std::thread, FFmpeg's decoders and
// curl all create threads with default attributes; Kodi overflowed even 1 MB
// decoding video. Every thread gets at least 8 MB, smaller if refused.

#define MIN_STACK (8u << 20)

int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg);

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg) {
	pthread_attr_t local;
	size_t size = 0;
	if (attr) {
		// Callers hand over their own attributes; raising the stack is harmless.
		if (pthread_attr_getstacksize(attr, &size) == 0 && size < MIN_STACK)
			pthread_attr_setstacksize((pthread_attr_t*)attr, MIN_STACK);
		return __real_pthread_create(thread, attr, start, arg);
	}
	if (pthread_attr_init(&local) != 0) return __real_pthread_create(thread, NULL, start, arg);
	static const size_t sizes[] = {MIN_STACK, 4u << 20, 1u << 20};
	int r = -1;
	for (unsigned i = 0; i < 3 && r != 0; i++) {
		pthread_attr_setstacksize(&local, sizes[i]);
		r = __real_pthread_create(thread, &local, start, arg);
	}
	pthread_attr_destroy(&local);
	return r;
}

// ---- pipe() ----------------------------------------------------------------------------------
// The kernel refuses pipe() inside the sandbox; curl's name-lookup wake-up is
// a pipe, so curl could never start. A local socket pair does the same job.

int __real_pipe(int fds[2]);

int __wrap_pipe(int fds[2]) {
	if (__real_pipe(fds) == 0) return 0;
	return socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
}
