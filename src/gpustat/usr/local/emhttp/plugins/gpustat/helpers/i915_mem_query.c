/*
 * i915_mem_query.c
 *
 * Queries Intel i915 DRM memory regions via DRM_IOCTL_I915_QUERY /
 * DRM_I915_QUERY_MEMORY_REGIONS -- the same uAPI Mesa and nvtop use to
 * report GPU memory (VRAM) total/used for discrete Arc (DG1/DG2) cards
 * and shared system memory for integrated GPUs.
 *
 * "used" accounting (unallocated_size) requires CAP_PERFMON or
 * CAP_SYS_ADMIN. Run this as root (the gpustat plugin already runs
 * its PHP backend as root on the Unraid host), otherwise unallocated
 * will just equal probed size and used will read as 0.
 *
 * Usage: i915_mem_query /dev/dri/renderD128
 * Output (stdout): {"regions":[{"class":1,"instance":0,"total_bytes":N,"used_bytes":N}, ...]}
 *
 * Build: gcc -O2 -Wall -o i915_mem_query i915_mem_query.c
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* ---- Minimal local copies of the stable i915 uAPI structs ----
 * These mirror include/uapi/drm/i915_drm.h. Defined locally so this
 * builds without needing kernel headers installed on the build host.
 */

#define DRM_IOCTL_BASE       'd'
#define DRM_COMMAND_BASE     0x40
#define DRM_I915_QUERY       0x39

struct drm_i915_query_item {
    uint64_t query_id;
    int32_t  length;
    uint32_t flags;
    uint64_t data_ptr;
};

struct drm_i915_query {
    uint32_t num_items;
    uint32_t flags;
    uint64_t items_ptr;
};

#define DRM_I915_QUERY_MEMORY_REGIONS 4

struct drm_i915_gem_memory_class_instance {
    uint16_t memory_class;
    uint16_t memory_instance;
};

#define I915_MEMORY_CLASS_SYSTEM 0
#define I915_MEMORY_CLASS_DEVICE 1

struct drm_i915_memory_region_info {
    struct drm_i915_gem_memory_class_instance region;
    uint32_t rsvd0;
    uint64_t probed_size;
    uint64_t unallocated_size;
    union {
        uint64_t rsvd1[8];
        struct {
            uint64_t probed_cpu_visible_size;
            uint64_t unallocated_cpu_visible_size;
        };
    };
};

struct drm_i915_query_memory_regions {
    uint32_t num_regions;
    uint32_t rsvd[3];
    struct drm_i915_memory_region_info regions[];
};

/* DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_QUERY, struct drm_i915_query) */
#define DRM_IOCTL_I915_QUERY \
    _IOC(_IOC_READ | _IOC_WRITE, DRM_IOCTL_BASE, \
         DRM_COMMAND_BASE + DRM_I915_QUERY, sizeof(struct drm_i915_query))

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s /dev/dri/renderDXXX\n", argv[0]);
        return 2;
    }

    int fd = open(argv[1], O_RDWR);
    if (fd < 0) {
        printf("{\"error\":\"open failed\"}\n");
        return 1;
    }

    struct drm_i915_query_item item;
    memset(&item, 0, sizeof(item));
    item.query_id = DRM_I915_QUERY_MEMORY_REGIONS;

    struct drm_i915_query q;
    memset(&q, 0, sizeof(q));
    q.num_items = 1;
    q.items_ptr = (uint64_t)(uintptr_t)&item;

    /* First pass: kernel fills item.length with the required buffer size */
    if (ioctl(fd, DRM_IOCTL_I915_QUERY, &q) < 0 || item.length <= 0) {
        printf("{\"error\":\"query length failed\"}\n");
        close(fd);
        return 1;
    }

    void *buf = calloc(1, (size_t)item.length);
    if (!buf) {
        printf("{\"error\":\"alloc failed\"}\n");
        close(fd);
        return 1;
    }
    item.data_ptr = (uint64_t)(uintptr_t)buf;

    /* Second pass: actually fill the buffer */
    if (ioctl(fd, DRM_IOCTL_I915_QUERY, &q) < 0) {
        printf("{\"error\":\"query fetch failed\"}\n");
        free(buf);
        close(fd);
        return 1;
    }

    struct drm_i915_query_memory_regions *regions = buf;

    printf("{\"regions\":[");
    for (uint32_t i = 0; i < regions->num_regions; i++) {
        struct drm_i915_memory_region_info *r = &regions->regions[i];
        uint64_t used = 0;
        if (r->probed_size >= r->unallocated_size) {
            used = r->probed_size - r->unallocated_size;
        }
        printf("%s{\"class\":%u,\"instance\":%u,\"total_bytes\":%llu,\"used_bytes\":%llu}",
               i == 0 ? "" : ",",
               r->region.memory_class,
               r->region.memory_instance,
               (unsigned long long)r->probed_size,
               (unsigned long long)used);
    }
    printf("]}\n");

    free(buf);
    close(fd);
    return 0;
}
