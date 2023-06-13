#define _GNU_SOURCE
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <string.h>
#include <fcntl.h>
#include "ion.h"
#include "owlfb.h"

static volatile sig_atomic_t done = 0;
static void sig_handler(int _)
{
    (void)_;
    done = 1;
}

int main()
{
    int io, ion_fd, owlfb_fd;
    struct ion_handle_data ion_handle_data;
    struct ion_fd_data ion_data;
    struct ion_allocation_data allocation_data;
    struct owlfb_overlay_info info;
    struct owlfb_overlay_args args;
    struct owl_ion_phys_data ion_phys;
    struct ion_custom_data ion_cd;
    struct fb_fix_screeninfo fb_info;
    struct fb_var_screeninfo fb_vinfo;

    void *vaddr;

    // Ensure Ctrl-C stops this mess.
    signal(SIGINT, sig_handler);

    /*
     * First we need to acquire a physically contiguous address space to hold
     * the overlay framebuffer, this is done via the ION allocator.
    */

    // Acquire the ION allocator fd
    ion_fd = open("/dev/ion", O_RDWR, 0);
    if (ion_fd < 0) {
        fprintf(stderr, "OVERLAY: Failed to acquire ION allocator fd: %d.\n", ion_fd);
        goto fail_ion_fd;
    }

    // Acquire the owlfb fd
    owlfb_fd = open("/dev/fb0", O_RDWR, 0);
    if (owlfb_fd < 0) {
        fprintf(stderr, "OVERLAY: Failed to framebuffer fd: %d.\n", owlfb_fd);
        goto fail_ion_alloc_data;
    }

    // Describe the desired buffer
    allocation_data = (struct ion_allocation_data){
        .len = 640 * 480 * sizeof(__u32),
        .heap_id_mask = (1 << ION_HEAP_TYPE_SYSTEM),
        .flags = 0
    };

    // Try to allocate the desired buffer
    io = ioctl(ion_fd, ION_IOC_ALLOC, &allocation_data);
    if (io != 0) {
        fprintf(stderr, "Failure to acquire ION buffer: %d.\n", io);
        goto fail_ion_alloc_data;
    }

    /*
     * Just the buffers by themselves are not enough - we also need to acquire
     * a virtual memory address to it (by acquiring the fd and then memory
     * mapping it) so we can draw to the buffer here, but we also need a
     * physical address to it, so we can configure the overlay itself.
    */

    // First let's start with the virtual memory address's fd
    ion_data = (struct ion_fd_data) {
        .handle = allocation_data.handle
    };
    io = ioctl(ion_fd, ION_IOC_MAP, &ion_data);
    if (io != 0) {
        fprintf(stderr, "Failure to memory map ION buffer: %d.\n", io);
        goto fail_ion_share;
    }

    // We got the fd, now memory map it. vaddr is our virtual address, you can write
    // to it.
    vaddr = mmap(0, allocation_data.len, PROT_READ|PROT_WRITE, MAP_SHARED, ion_data.fd, 0);
    if (vaddr == MAP_FAILED) {
        fprintf(stderr, "Failed to memory map ION buffer.");
        goto fail_ion_share;
    }

    // We need the physical addresses for the ion buffer
    ion_phys = (struct owl_ion_phys_data){
        .handle = allocation_data.handle,
    };

    ion_cd = (struct ion_custom_data){
        .arg = (unsigned long)&ion_phys,
        .cmd = OWL_ION_GET_PHY
    };

    io = ioctl(ion_fd, ION_IOC_CUSTOM, &ion_cd);
    if (io != 0) {
        fprintf(stderr, "Failure to acquire ION buffer physaddr: %d.\n", io);
        goto fail_ion_share;
    }

    printf("Got virt: %p phys: %p (len: %lu).\n", vaddr, ion_phys.phys_addr, ion_phys.size);

    // Zero out the framebuffer
    memset(vaddr, 0, allocation_data.len);

    /*
     * That's done with the basic setup, let's attempt to acquire an overlay with that!
    */
    info = (struct owlfb_overlay_info){};
    args = (struct owlfb_overlay_args){
        .fb_id = 0,
        .overlay_id = 1,
        .overlay_type = OWLFB_OVERLAY_VIDEO,
        .overlay_mem_base = ion_phys.phys_addr,
        .overlay_mem_size = ion_phys.size,
        .uintptr_overly_info = (uintptr_t)&info,
    };
    
    io = ioctl(owlfb_fd, OWLFB_OVERLAY_REQUEST, &args);
    if (io != 0) {
        fprintf(stderr, "Failure requesting overlay: %d.\n", io);
        goto fail_ion_share;
    }

    printf("OWLFB_OVERLAY:\n");
	printf("- fb_id: %lu\n", (unsigned long)args.fb_id);
	printf("- overlay_id: %lu\n", (unsigned long)args.overlay_id);
	printf("- overlay_type: %lu\n", (unsigned long)args.overlay_type);
	printf("- overlay_mem_base: %lu\n", (unsigned long)args.overlay_mem_base);
	printf("- overlay_mem_size: %lu\n", (unsigned long)args.overlay_mem_size);
	printf("- uintptr_overly_info: %lu\n", (unsigned long)args.uintptr_overly_info);

    io = ioctl(owlfb_fd, OWLFB_OVERLAY_GETINFO, &args);
    if (io != 0) {
        fprintf(stderr, "Failure getting overlay info: %d.\n", io);
        goto fail_ion_share;
    }

    /* We now need some details regarding the screen... */
    io = ioctl(owlfb_fd, FBIOGET_FSCREENINFO, &fb_info);
    if (io != 0) {
        fprintf(stderr, "Failure getting fb info: %d.\n", io);
        goto fail_ion_share;
    }

    info.mem_off = ion_phys.phys_addr - fb_info.smem_start;
    info.mem_size = allocation_data.len;
    info.color_mode = OWL_DSS_COLOR_RGBA32;
    info.screen_width = info.out_width;
    info.zorder = 100;

    io = ioctl(owlfb_fd, OWLFB_OVERLAY_SETINFO, &args);
    if (io != 0) {
        fprintf(stderr, "Failure enabling overlay: %d.\n", io);
        goto fail_ion_share;
    }
    io = ioctl(owlfb_fd, OWLFB_OVERLAY_ENABLE, &args);

    while (!done) {
        for (int i = 0; i < 240; i++) {
            for (int j = 0; j < 320; j++) {
                unsigned char c = i^j;
                ((uint32_t*)vaddr)[i * info.width + j] = c << 24 | c << 16 | c << 8 | 0x7F;
            }
        }

        long long f = 0;
        io = ioctl(owlfb_fd, OWLFB_OVERLAY_SETINFO, &args);
        io = ioctl(owlfb_fd, OWLFB_WAITFORVSYNC, &f);
    };

    printf("Execution done.\n");

    io = ioctl(owlfb_fd, OWLFB_OVERLAY_DISABLE, &args);
    munmap(vaddr, allocation_data.len);
    ioctl(ion_fd, ION_IOC_FREE, &ion_handle_data);
    return 0;

    fail_ion_share:
    ion_handle_data = (struct ion_handle_data) {
        .handle = allocation_data.handle
    };
    io = ioctl(ion_fd, ION_IOC_FREE, &ion_handle_data);
    if (io != 0) {
        fprintf(stderr, "Failed to free ION allocated buffer: %d.", io);
    }
    fail_ion_alloc_data:
    close(ion_fd);
    fail_ion_fd:
    return 1;
}
