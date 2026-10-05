// Stand-ins for optional hooks that the payload SDK's libraries reference
// weakly. A payload gets them from the ELF loader; a native app has none, and
// the converter needs every symbol resolved. Each reports "not available".
#include <stddef.h>

// (zstd's tracing hooks are in console_curl.c.)

// Dynamic loading (libc.a's dlopen family): not supported in the app.
void* __dlopen(const char* path, int mode) { (void)path; (void)mode; return NULL; }
void* __dlsym(void* handle, const char* name) { (void)handle; (void)name; return NULL; }
int __dlclose(void* handle) { (void)handle; return -1; }
char* __dlerror(void) { return (char*)"dynamic loading is not supported"; }
int __dladdr(const void* addr, void* info) { (void)addr; (void)info; return 0; }

// Kernel memory protection from the jailbreak's kernel helpers.
int kernel_mprotect(int pid, unsigned long addr, unsigned long len, int prot) {
	(void)pid; (void)addr; (void)len; (void)prot;
	return -1;
}
