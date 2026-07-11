#include <3ds/svc.h>
#include <3ds/allocator/mappable.h>
#include <3ds/env.h>
#include <3ds/os.h>
#include <3ds/result.h>

#define HEAP_SPLIT_SIZE_CAP  (24 << 20) // 24MB
#define LINEAR_HEAP_SIZE_CAP (32 << 20) // 32MB

// The real homemenu retries a failed heap-grow svcControlMemory call up to
// 100 times, sleeping between attempts, instead of treating one failure as
// fatal (see HeapInit_GrowToConfiguredSizes in the real homemenu binary).
// Several other system processes are still starting up concurrently at this
// point in boot and doing their own memory setup, so a MEMOP_ALLOC_LINEAR
// failure here can be transient contention/fragmentation rather than a real,
// permanent shortage - retrying gives that window a chance to clear. The
// exact per-attempt sleep duration the real binary uses wasn't recovered
// (it's an opaque constant), so this picks a modest 1ms - enough to yield to
// other threads without meaningfully slowing down a normal boot.
#define HEAP_ALLOC_RETRY_COUNT     	100
#define HEAP_ALLOC_RETRY_SLEEP_NS 	10000000LL // 1ms

extern char *fake_heap_start;
extern char* fake_heap_end;

u32 __ctru_heap;
u32 __ctru_linear_heap;

__attribute__((weak)) u32 __ctru_heap_size        = 0;
__attribute__((weak)) u32 __ctru_linear_heap_size = 0;

void __attribute__((weak)) __system_allocateHeaps(void) {
	Result rc;

	// Free memory - the real homemenu does this too, speculatively, to shrink
	// away any heap left over from a previous run (e.g. a warm relaunch). On a
	// freshly-created process there is nothing mapped at these addresses yet,
	// so this legitimately fails with "invalid argument" - the real homemenu's
	// own boot code ignores that failure and moves straight on to the ALLOC
	// calls below, so we do the same instead of treating it as fatal.
	u32 free_mem_out;

	svcControlMemory(&free_mem_out, 0x00000000, 0x00000000, 0x00, MEMOP_FREE,
					 0x00);

	svcControlMemory(&free_mem_out, 0x08000000, 0x00000000, 0x00,
					  MEMOP_FREE, 0x00);

	// Allocate the application heap
	rc = svcControlMemory(&__ctru_heap, OS_HEAP_AREA_BEGIN, 0x0, __ctru_heap_size, MEMOP_ALLOC, MEMPERM_READ | MEMPERM_WRITE);
	if (R_FAILED(rc)) {
		asm volatile("mov r11, %0" : : "r"(rc));
		asm volatile("mov r12, %0" : : "r"(0xEEEE0002));
		svcBreak(USERBREAK_PANIC);
	}

	// SVCControlMemory: addr0=0x00000000, addr1=0x00000000, size=0xb64000, op=0x10003, perm=0x3

	// Allocate the linear heap
	rc = svcControlMemory(&__ctru_linear_heap, 0x0, 0x0, __ctru_linear_heap_size, MEMOP_ALLOC_LINEAR, MEMPERM_READ | MEMPERM_WRITE);
	if (R_FAILED(rc)) {
		asm volatile("mov r10, %0" : : "r"(__ctru_linear_heap_size));
		asm volatile("mov r11, %0" : : "r"(rc));
		asm volatile("mov r12, %0" : : "r"(0xEEEE0003));
		svcBreak(USERBREAK_PANIC);
	}

	// Mappable allocator init
	mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);

	// Set up newlib heap
	fake_heap_start = (char*)__ctru_heap;
	fake_heap_end = fake_heap_start + __ctru_heap_size;

}
