#define _GNU_SOURCE // For strcasestr, MAP_HUGE_*, MADV_*
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h> // mmap, munmap, MAP_*, PROT_*, MADV_*
#include <unistd.h>   // sysconf, getpid
#include <fcntl.h>    // open
#include <errno.h>
#include <inttypes.h> // PRIu64
#include <limits.h>   // ULLONG_MAX

// Define common page sizes
#define PAGE_SIZE_4K (4UL * 1024)
#define PAGE_SIZE_2M (2UL * 1024 * 1024)
#define PAGE_SIZE_1G (1UL * 1024 * 1024 * 1024)

// Size of a Page Table Entry (PTE)
#define PTE_SIZE 8

// Enum to represent the requested page size mode
typedef enum {
    MODE_4K, // Try to force 4k (using MADV_NOHUGEPAGE)
    MODE_THP, // Standard anonymous mapping (rely on system THP)
    MODE_2M, // Explicit HugeTLB 2MB
    MODE_1G  // Explicit HugeTLB 1GB
} PageSizeMode;

// --- Helper Functions ---

// Helper function to parse size strings like "1G", "512M", "1024K"
size_t parse_size(const char *size_str) {
    char *endptr;
    // Use unsigned long long for parsing to detect negative inputs via overflow later
    unsigned long long val_ull = strtoull(size_str, &endptr, 10);

    // Check for parsing errors or empty string
    if (endptr == size_str || (val_ull == 0 && errno != 0)) {
        fprintf(stderr, "Error: Invalid size value '%s'. Not a valid number.\n", size_str);
        return 0;
    }
    if (val_ull == ULLONG_MAX && errno == ERANGE) {
        fprintf(stderr, "Error: Size value '%s' is too large.\n", size_str);
        return 0;
    }

    size_t multiplier = 1;
    char suffix = '\0';
    if (*endptr != '\0') {
        suffix = *endptr;
        switch (suffix) {
            case 'G': case 'g': multiplier = 1024 * 1024 * 1024; break;
            case 'M': case 'm': multiplier = 1024 * 1024; break;
            case 'K': case 'k': multiplier = 1024; break;
            default:
                fprintf(stderr, "Error: Invalid size suffix '%c' in '%s'. Use K, M, or G.\n", suffix, size_str);
                return 0;
        }
        // Check if there are extra characters after the suffix
        if (*(endptr + 1) != '\0') {
            fprintf(stderr, "Error: Trailing characters after suffix in '%s'\n", size_str);
            return 0;
        }
    }

    // Check for overflow when applying multiplier
    if (val_ull > ULLONG_MAX / multiplier) {
        fprintf(stderr, "Error: Size value '%s' causes overflow.\n", size_str);
        return 0;
    }

    size_t final_size = (size_t)val_ull * multiplier;

    // Check if the final size is zero, which is invalid for mmap
    if (final_size == 0 && val_ull == 0) {
        fprintf(stderr, "Error: Mapping size cannot be zero.\n");
        return 0;
    }


    return final_size;
}

// Helper function to read VmPTE from /proc/self/status
long get_vmpte_kb() {
    static char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/%d/status", getpid());

    FILE *f = fopen(proc_path, "r");
    if (!f) {
        // Don't print perror here, let caller decide based on return value
        return -1;
    }

    char line[256];
    long vmpte = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmPTE:", 6) == 0) {
            if (sscanf(line + 6, "%ld", &vmpte) == 1) {
                break; // Found it
            } else {
                // Suppress warning here, return -1 indicates failure
                vmpte = -1;
                break;
            }
        }
    }
    fclose(f);
    // If vmpte is still -1 here, it means not found or parse error
    return vmpte; // Returns value in kB, or -1 on error/not found
}

// Function to calculate theoretical overhead (PTEs only)
size_t calculate_overhead(size_t total_size, size_t page_size) {
    if (page_size == 0) return 0;
    // Use ceiling division: (A + B - 1) / B
    size_t num_pages = (total_size + page_size - 1) / page_size;
    return num_pages * PTE_SIZE;
}

// Function to check THP status
typedef enum { THP_UNKNOWN, THP_ALWAYS, THP_MADVISE, THP_NEVER } ThpStatus;
ThpStatus check_thp_status() {
    FILE *f = fopen("/sys/kernel/mm/transparent_hugepage/enabled", "r");
    if (!f) return THP_UNKNOWN;

    char line[256];
    ThpStatus status = THP_UNKNOWN;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "[always]")) { status = THP_ALWAYS; break; }
        if (strstr(line, "[madvise]")) { status = THP_MADVISE; break; }
        if (strstr(line, "[never]")) { status = THP_NEVER; break; }
    }
    fclose(f);
    return status;
}

// --- Main Logic ---

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <size[K|M|G]> <mode:4k|thp|2m|1g>\n", argv[0]);
        fprintf(stderr, "  size: Mapping size (e.g., 1G, 256M)\n");
        fprintf(stderr, "  mode: Page size strategy\n");
        fprintf(stderr, "    4k:  Attempt 4KB pages (using madvise hint)\n");
        fprintf(stderr, "    thp: Standard anonymous mapping (allow Transparent Huge Pages)\n");
        fprintf(stderr, "    2m:  Explicit 2MB HugeTLB pages (requires configuration)\n");
        fprintf(stderr, "    1g:  Explicit 1GB HugeTLB pages (requires configuration)\n");
        fprintf(stderr, "\nExample: %s 1G 2m\n", argv[0]);
        return 1;
    }

    // --- Parse Arguments ---
    size_t map_size = parse_size(argv[1]);
    if (map_size == 0) {
        return 1; // Error message already printed
    }

    PageSizeMode mode;
    int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS;
    size_t huge_page_size = 0; // Relevant for HugeTLB modes
    size_t touch_step_size = PAGE_SIZE_4K; // Default step for touching

    if (strcmp(argv[2], "4k") == 0) {
        mode = MODE_4K;
        printf("Mode: Attempting 4KB pages (using MADV_NOHUGEPAGE hint)\n");
    } else if (strcmp(argv[2], "thp") == 0) {
        mode = MODE_THP;
        printf("Mode: Standard anonymous mapping (allowing THP)\n");
    } else if (strcmp(argv[2], "2m") == 0) {
        mode = MODE_2M;
        mmap_flags |= MAP_HUGETLB | MAP_HUGE_2MB;
        huge_page_size = PAGE_SIZE_2M;
        touch_step_size = PAGE_SIZE_2M;
        printf("Mode: Explicit 2MB HugeTLB pages\n");
    } else if (strcmp(argv[2], "1g") == 0) {
        mode = MODE_1G;
        mmap_flags |= MAP_HUGETLB | MAP_HUGE_1GB;
        huge_page_size = PAGE_SIZE_1G;
        touch_step_size = PAGE_SIZE_1G;
        printf("Mode: Explicit 1GB HugeTLB pages\n");
    } else {
        fprintf(stderr, "Error: Invalid mode '%s'. Use 4k, thp, 2m, or 1g.\n", argv[2]);
        return 1;
    }

    // --- Pre-mmap Checks ---
    if ((mode == MODE_2M || mode == MODE_1G) && (map_size % huge_page_size != 0)) {
        fprintf(stderr, "Error: Mapping size %zu bytes must be a multiple of the huge page size (%zu bytes) for mode %s.\n",
                map_size, huge_page_size, argv[2]);
        return 1;
    }

    ThpStatus thp_status = check_thp_status();
    if ((mode == MODE_4K || mode == MODE_THP) && thp_status == THP_NEVER) {
        printf("Warning: System THP is set to 'never'. Kernel will likely use 4KB pages.\n");
    }
    if (mode == MODE_THP && thp_status == THP_UNKNOWN) {
        printf("Warning: Could not determine system THP status.\n");
    }


    // --- Get baseline VmPTE ---
    long vmpte_before = get_vmpte_kb();
    if (vmpte_before < 0) {
        fprintf(stderr, "Error: Could not get initial VmPTE from /proc/self/status. Aborting.\n");
        return 1;
    }
    printf("Initial VmPTE: %ld kB\n", vmpte_before);
    printf("Mapping size: %zu bytes (%.2f MB / %.2f GB)\n",
           map_size, (double)map_size / (1024*1024), (double)map_size / (1024*1024*1024));

    // --- mmap the memory ---
    printf("--- Mapping Memory ---\n");
    errno = 0;
    void *addr = mmap(NULL, map_size, PROT_READ | PROT_WRITE, mmap_flags, -1, 0);

    if (addr == MAP_FAILED) {
        int err = errno; // Capture errno immediately
        fprintf(stderr, "Error: mmap failed: %s (errno %d)\n", strerror(err), err);
        if ((mode == MODE_2M || mode == MODE_1G)) {
            if (err == ENOMEM) {
                fprintf(stderr, "  Hint: This often means insufficient HugeTLB pages are configured.\n");
                fprintf(stderr, "        Check/increase '/proc/sys/vm/nr_hugepages' (for default size)\n");
                fprintf(stderr, "        or '/sys/kernel/mm/hugepages/hugepages-%zukB/nr_hugepages' (for specific size).\n", huge_page_size / 1024);
                fprintf(stderr, "        Ensure enough contiguous memory is available.\n");
            } else if (err == EINVAL) {
                fprintf(stderr, "  Hint: Check if mapping size (%zu) is a multiple of huge page size (%zu),\n", map_size, huge_page_size);
                fprintf(stderr, "        or if the system supports HugeTLB pages of this size.\n");
                fprintf(stderr, "        Flags used: 0x%x\n", mmap_flags);
            }
        }
        return 1;
    }

    // --- Apply madvise hints (after successful mmap) ---
    if (mode == MODE_4K) {
        if (madvise(addr, map_size, MADV_NOHUGEPAGE) == -1) {
            fprintf(stderr, "Warning: madvise(MADV_NOHUGEPAGE) failed: %s\n", strerror(errno));
        }
    } else if (mode == MODE_THP && thp_status == THP_MADVISE) {
        if (madvise(addr, map_size, MADV_HUGEPAGE) == -1) {
            fprintf(stderr, "Warning: madvise(MADV_HUGEPAGE) failed: %s\n", strerror(errno));
        }
    }

    // --- Touch the memory ---
    printf("--- Touching Memory (1 byte per %zu KB page/stride) ---\n", touch_step_size / 1024);
    volatile char *ptr = (volatile char *)addr;
    size_t touched_count = 0;
    for (size_t i = 0; i < map_size; i += touch_step_size) {
        ptr[i] = (char)(i % 256);
        touched_count++;
    }
    printf("Touched %zu strides.\n", touched_count);

    // --- Get VmPTE after mapping and touching ---
    long vmpte_after = get_vmpte_kb();
    if (vmpte_after < 0) {
        fprintf(stderr, "Warning: Could not get final VmPTE. Change calculation skipped.\n");
    } else {
        printf("Final VmPTE:   %ld kB\n", vmpte_after);
        long vmpte_diff = vmpte_after - vmpte_before;
        printf("VmPTE Change:  %ld kB\n", vmpte_diff);
        printf("--------------------------------------------------\n");
        printf("NOTE: VmPTE reflects the *total* process page table size.\n");
        printf("      The change observed is an *indicator*, not a precise measure\n");
        printf("      of the overhead for *this specific mapping* alone.\n");
        printf("      It can be influenced by THP, other allocations, etc.\n");
        printf("--------------------------------------------------\n");
    }

    // --- Calculate Theoretical Overheads ---
    printf("\n--- Theoretical Overhead Calculation (Lowest Level Entries Only) ---\n");
    size_t overhead_4k = calculate_overhead(map_size, PAGE_SIZE_4K);
    printf("If using 4KB pages: %zu entries * %d bytes = %zu bytes (%.2f KB / %.2f MB)\n",
           (map_size + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K, PTE_SIZE, overhead_4k,
           (double)overhead_4k / 1024, (double)overhead_4k / (1024*1024));

    if (map_size >= PAGE_SIZE_2M) {
        size_t overhead_2m = calculate_overhead(map_size, PAGE_SIZE_2M);
        printf("If using 2MB pages: %zu entries * %d bytes = %zu bytes (%.2f KB / %.2f MB)\n",
               (map_size + PAGE_SIZE_2M - 1) / PAGE_SIZE_2M, PTE_SIZE, overhead_2m,
               (double)overhead_2m / 1024, (double)overhead_2m / (1024*1024));
    } else {
        printf("If using 2MB pages: N/A (mapping size < 2MB)\n");
    }

    if (map_size >= PAGE_SIZE_1G) {
        size_t overhead_1g = calculate_overhead(map_size, PAGE_SIZE_1G);
        printf("If using 1GB pages: %zu entries * %d bytes = %zu bytes (%.2f KB / %.2f MB)\n",
               (map_size + PAGE_SIZE_1G - 1) / PAGE_SIZE_1G, PTE_SIZE, overhead_1g,
               (double)overhead_1g / 1024, (double)overhead_1g / (1024*1024));
    } else {
        printf("If using 1GB pages: N/A (mapping size < 1GB)\n");
    }
    printf("--------------------------------------------------\n");
    printf("NOTE: These calculations show potential lowest-level entry overhead only.\n");
    printf("      They don't include intermediate page directory costs.\n");
    printf("      Actual overhead depends on kernel behavior (THP, etc.).\n");
    printf("--------------------------------------------------\n");

    printf("PID: %d - You may inspect `/proc/%d/smaps` now, then press Enter...\n", getpid(), getpid());
    getchar();

    // --- Cleanup ---
    printf("\n--- Unmapping Memory ---\n");
    if (munmap(addr, map_size) == -1) {
        // Report error on failure
        perror("Error: munmap failed");
    }

    return 0;
}

