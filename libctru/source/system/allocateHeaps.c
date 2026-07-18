#include <3ds/svc.h>
#include <3ds/allocator/mappable.h>
#include <3ds/env.h>
#include <3ds/os.h>
#include <3ds/result.h>

extern char *fake_heap_start;
extern char* fake_heap_end;

u32 __ctru_heap;
u32 __ctru_linear_heap;

__attribute__((weak)) u32 __ctru_heap_size        = 0;
__attribute__((weak)) u32 __ctru_linear_heap_size = 0;

void __attribute__((weak)) __system_allocateHeaps(void) {
	Result rc;

	// Free memory - the real homemenu does this too, speculatively, to
	// shrink away any heap left over from a previous run (e.g. a warm
	// relaunch). On a freshly-created process there is nothing mapped at
	// these addresses yet, so this legitimately fails with "invalid
	// argument" - the real homemenu's own boot code ignores that failure
	// and moves straight on to the ALLOC calls below, so we do the same
	// instead of treating it as fatal.
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

	// Allocate the linear heap
	rc = svcControlMemory(&__ctru_linear_heap, 0x0, 0x0, __ctru_linear_heap_size, MEMOP_ALLOC_LINEAR, MEMPERM_READ | MEMPERM_WRITE);
	if (R_FAILED(rc)) {
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
