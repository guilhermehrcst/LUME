#!/bin/bash
# Capture the host description required by the E2-R1 preregistration (section 2) as key=value text.
# usage: capture_environment.sh <repo-root> > environment.txt
set -u
ROOT=${1:-.}
kv() { printf '%s=%s\n' "$1" "$2"; }
first() { "$@" 2>/dev/null | head -1; }

kv captured_utc "$(date -u +%FT%TZ)"
kv uname "$(uname -srvm)"                       # nodename intentionally omitted
kv arch "$(uname -m)"
kv git_sha "$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
kv git_dirty_files "$(git -C "$ROOT" status --porcelain 2>/dev/null | wc -l)"
kv cpu_model "$(sed -n 's/^model name[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo | head -1)"
[ -z "$(sed -n 's/^model name[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo | head -1)" ] && kv cpu_model_lscpu "$(lscpu 2>/dev/null | sed -n 's/^Model name:[[:space:]]*//p' | head -1)"
kv cpu_implementer_part "$(sed -n 's/^CPU implementer[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo | head -1)/$(sed -n 's/^CPU part[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo | head -1)"
kv cpu_family_model_stepping "$(sed -n 's/^cpu family[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo | head -1)/$(sed -n 's/^model[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo | head -1)/$(sed -n 's/^stepping[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo | head -1)"
kv logical_cpus "$(nproc --all 2>/dev/null)"
if command -v lscpu >/dev/null 2>&1; then
  kv threads_per_core "$(lscpu | sed -n 's/^Thread(s) per core:[[:space:]]*//p')"
  kv cores_per_socket "$(lscpu | sed -n 's/^Core(s) per socket:[[:space:]]*//p')"
  kv sockets "$(lscpu | sed -n 's/^Socket(s):[[:space:]]*//p')"
  kv lscpu_max_mhz "$(lscpu | sed -n 's/^CPU max MHz:[[:space:]]*//p')"
  kv lscpu_min_mhz "$(lscpu | sed -n 's/^CPU min MHz:[[:space:]]*//p')"
  kv numa_nodes "$(lscpu | sed -n 's/^NUMA node(s):[[:space:]]*//p')"
fi
kv microarchitecture "unknown (record manually from the CPU model if known)"
# cache hierarchy of cpu0 (level, type, size, ways, line size, shared_cpu_list)
for d in /sys/devices/system/cpu/cpu0/cache/index*; do
  [ -d "$d" ] || continue
  kv "cache_$(basename "$d")" "L$(cat "$d/level" 2>/dev/null) $(cat "$d/type" 2>/dev/null) size=$(cat "$d/size" 2>/dev/null) ways=$(cat "$d/ways_of_associativity" 2>/dev/null) line=$(cat "$d/coherency_line_size" 2>/dev/null) shared_with=$(cat "$d/shared_cpu_list" 2>/dev/null)"
done
kv mem_total "$(sed -n 's/^MemTotal:[[:space:]]*//p' /proc/meminfo)"
kv mem_available "$(sed -n 's/^MemAvailable:[[:space:]]*//p' /proc/meminfo)"
kv swap_total "$(sed -n 's/^SwapTotal:[[:space:]]*//p' /proc/meminfo)"
kv swap_free "$(sed -n 's/^SwapFree:[[:space:]]*//p' /proc/meminfo)"
kv swap_devices "$(tail -n +2 /proc/swaps 2>/dev/null | wc -l)"
kv page_size "$(getconf PAGESIZE)"
kv thp "$(cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo unavailable)"
kv kernel "$(cat /proc/sys/kernel/osrelease)"
kv glibc "$(first getconf GNU_LIBC_VERSION)"
kv ldd "$(first ldd --version)"
kv governors "$(cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort | uniq -c | tr '\n' ';' | sed 's/  */ /g')"
kv scaling_driver "$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_driver 2>/dev/null || echo unavailable)"
kv intel_pstate_no_turbo "$(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo unavailable)"
kv cpufreq_boost "$(cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null || echo unavailable)"
kv numa_topology "$(ls -d /sys/devices/system/node/node* 2>/dev/null | wc -l) node(s)"
command -v numactl >/dev/null 2>&1 && kv numactl_hardware "$(numactl --hardware 2>/dev/null | head -3 | tr '\n' ';')"
for c in g++ gcc clang++ clang cmake; do
  if command -v "$c" >/dev/null 2>&1; then kv "tool_$c" "$("$c" --version 2>&1 | head -1)"; else kv "tool_$c" "not installed"; fi
done
kv perf_event_paranoid "$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo unavailable)"
if command -v perf >/dev/null 2>&1; then kv perf_tool "$(perf --version 2>&1 | head -1)"; else kv perf_tool "not installed (the PMU binary uses perf_event_open directly)"; fi
kv virtualization "$("$(dirname "$0")/check_physical_host.sh" --report 2>&1 | grep -E '^PHYSICAL=' )"
