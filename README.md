# mmap Page Table Overhead Estimator

This C program is designed for Linux systems to help estimate and observe the effects of using `mmap` with different memory mapping strategies on process page table size. It's particularly useful for understanding the trade-offs involved when considering huge pages (Transparent Huge Pages or explicit HugeTLB) versus standard 4KB pages, relevant for developing applications like database buffer managers.

## Description 

The program performs the following steps:

1. Maps a specified amount of virtual memory using `mmap` with one of the selected strategies (4k, thp, 2m, 1g).
2. Touches the allocated memory to trigger page faults and force backing physical memory allocation and page table population.
3. Reads the process's total page table size (`VmPTE` from `/proc/self/status`) before and after the mapping+touching steps to observe the change.
4. Calculates the theoretical overhead for the lowest-level page table entries (e.g., PTEs, PMDs) assuming the entire mapping used 4KB, 2MB, or 1GB pages.

**Important Limitation:** The observed `VmPTE` change is an indirect indicator for the entire process, not a precise measurement of the overhead solely for the test `mmap` region. It includes overhead for all process mappings and all levels of page tables. For direct confirmation of THP usage on a specific mapping, inspecting `/proc/<pid>/smaps` (`AnonHugePages` field) is more reliable.

## Compilation

You need `gcc` (or a compatible C compiler) and `make` installed.
```
make
```

This will create an executable file named `mmap_overhead`.

## Usage

```
./mmap_overhead <size[K|M|G]> <mode:4k|thp|2m|1g>
```

### Arguments
- `<size[K|M|G]>`: The total size of the memory region to map. Use suffixes K, M, or G for Kilobytes, Megabytes, or Gigabytes (e.g., `256M`, `1G`).
- `<mode>`: The paging strategy to use:4k: Attempts to use standard 4KB pages. Issues a `madvise(MADV_NOHUGEPAGE)` hint, but the kernel might still use THP if configured with `[always]`.
- `thp`: Standard anonymous mapping (`MAP_ANONYMOUS | MAP_PRIVATE`). Relies on system Transparent Huge Page settings. Issues `madvise(MADV_HUGEPAGE)` hint if system THP is `[madvise]`.
- `2m`: Uses explicit 2MB HugeTLB pages (`MAP_HUGETLB | MAP_HUGE_2MB`). Requires 2MB HugeTLB pages to be pre-configured in the kernel. Mapping size must be a multiple of 2MB.
- `1g`: Uses explicit 1GB HugeTLB pages (`MAP_HUGETLB | MAP_HUGE_1GB`). Requires 1GB HugeTLB pages to be pre-configured and supported. Mapping size must be a multiple of 1GB.

### Examples

```
# Observe using standard mapping (allow THP) for 512MB
./mmap_overhead 512M thp

# Attempt to use 4KB pages for 1GB
./mmap_overhead 1G 4k

# Use explicit 2MB HugeTLB pages for 1GB (Requires config!)
./mmap_overhead 1G 2m
```
## Interpreting Output

- **Initial/Final VmPTE & Change:** Shows the total process page table size before and after the test. The change gives a rough idea of the mapping's impact but is not a precise overhead measurement for the mapping itself (see Limitation above).
- **Theoretical Overhead Calculation:** Shows the calculated size required only for the lowest-level page table entries (PTEs for 4k, PMDs for 2M/1G assuming PTE size) if the entire mapping used that specific page size. This helps compare potential best-case scenarios but ignores higher-level table costs.
- **System Notes/Hints:** The program may print warnings about system THP settings or specific error hints if `mmap` fails (especially for HugeTLB modes).

## HugeTLB Configuration (for `2m` and `1g` modes)

Using `2m` or `1g` modes requires configuring the Linux kernel to reserve HugeTLB pages before running the program.

1. Check current configuration:
```
grep Huge /proc/meminfo
```
2. Reserve Huge Pages (Example: 512 x 2MB pages = 1GB total):
```
# Temporary (resets on reboot) - Requires root/sudo
sudo sysctl vm.nr_hugepages=512
# Permanent: Edit /etc/sysctl.conf and add vm.nr_hugepages=512, then run sudo sysctl -p
```

Adjust the number (`512`) based on the amount of memory you want to reserve and the default huge page size (check `cat /proc/meminfo | grep Hugepagesize`). For specific sizes (like 1GB), you might need to use `/sys/kernel/mm/hugepages/hugepages-*/nr_hugepages`.
