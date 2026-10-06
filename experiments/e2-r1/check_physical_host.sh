#!/bin/bash
# Fail-closed physical-host check for E2-R1. Prints key=value evidence and a verdict.
# exit 0: no virtualization or container signal found (PHYSICAL=yes)
# exit 1: at least one signal found (PHYSICAL=no)
# usage: check_physical_host.sh [--report]     (--report always exits 0)
set -u
REPORT=0; [ "${1:-}" = "--report" ] && REPORT=1
signals=()
note() { echo "$1"; }

# Return success only when kernel log text contains an actual hypervisor signal.
# Linux may emit "Booting paravirtualized kernel on bare hardware" on physical
# machines; that sentence is explicitly benign and must not classify the host as a VM.
dmesg_reports_hypervisor() {
  grep -iE 'hypervisor detected|kvm: (hypervisor|support)|Booting paravirtualized kernel' |
    grep -viE 'Booting paravirtualized kernel on bare hardware'
}

# Deterministic test seam for the classifier above. Not used by the replication runner.
if [ "${1:-}" = "--classify-dmesg-file" ]; then
  [ "${2:-}" ] || { echo "usage: $0 --classify-dmesg-file <path>" >&2; exit 2; }
  if dmesg_reports_hypervisor < "$2" >/dev/null; then
    echo "DMESG_HYPERVISOR=yes"
    exit 1
  fi
  echo "DMESG_HYPERVISOR=no"
  exit 0
fi

arch=$(uname -m); note "arch=$arch"

# 1. systemd-detect-virt (covers VMs and containers)
if command -v systemd-detect-virt >/dev/null 2>&1; then
  vm=$(systemd-detect-virt --vm 2>/dev/null || true); ct=$(systemd-detect-virt --container 2>/dev/null || true)
  note "systemd_detect_virt_vm=${vm:-none}"; note "systemd_detect_virt_container=${ct:-none}"
  [ -n "${vm:-}" ] && [ "$vm" != "none" ] && signals+=("systemd-detect-virt --vm=$vm")
  [ -n "${ct:-}" ] && [ "$ct" != "none" ] && signals+=("systemd-detect-virt --container=$ct")
else
  note "systemd_detect_virt=unavailable"
fi

# 2. CPU hypervisor flag (x86); lscpu hypervisor vendor (any arch)
if [ -r /proc/cpuinfo ] && grep -qiE '^flags.*\bhypervisor\b' /proc/cpuinfo; then
  note "cpuinfo_hypervisor_flag=yes"; signals+=("cpuinfo flags contain 'hypervisor'")
else
  note "cpuinfo_hypervisor_flag=no"
fi
if command -v lscpu >/dev/null 2>&1; then
  hv=$(lscpu 2>/dev/null | sed -n 's/^Hypervisor vendor:[[:space:]]*//p' | head -1)
  note "lscpu_hypervisor_vendor=${hv:-none}"
  [ -n "${hv:-}" ] && signals+=("lscpu Hypervisor vendor: $hv")
fi

# 3. DMI strings naming a hypervisor or cloud VM. Narrow exception: a bare-metal cloud instance
#    (product name ending in .metal) with no other signal.
dmi_hit=""
for f in sys_vendor product_name bios_vendor board_vendor product_version; do
  v=$(cat "/sys/class/dmi/id/$f" 2>/dev/null || true)
  [ -n "$v" ] && note "dmi_$f=$v"
  if echo "$v" | grep -qiE 'kvm|qemu|vmware|virtualbox|innotek|xen|bochs|amazon ec2|google compute|microsoft corporation|hyper-v|parallels|openstack|alibaba cloud|digitalocean|firecracker|bhyve|cloud hypervisor'; then
    dmi_hit="$f=$v"
  fi
done
if [ -n "$dmi_hit" ]; then
  pn=$(cat /sys/class/dmi/id/product_name 2>/dev/null || true)
  if echo "$pn" | grep -qE '\.metal$' && [ "${#signals[@]}" -eq 0 ]; then
    note "dmi_exception=bare-metal cloud instance ($pn); no other signal"
  else
    signals+=("DMI names a hypervisor/cloud VM: $dmi_hit")
  fi
fi

# 4. hypervisor interfaces
[ -s /sys/hypervisor/type ] && { note "sys_hypervisor_type=$(cat /sys/hypervisor/type)"; signals+=("/sys/hypervisor/type=$(cat /sys/hypervisor/type)"); }
for d in /proc/device-tree/hypervisor /sys/firmware/devicetree/base/hypervisor; do
  [ -e "$d" ] && { note "devicetree_hypervisor=$d"; signals+=("device-tree hypervisor node $d"); }
done
if dmesg 2>/dev/null | dmesg_reports_hypervisor >/dev/null; then
  note "dmesg_hypervisor=yes"; signals+=("dmesg reports a hypervisor")
else
  note "dmesg_hypervisor=no"
fi

# 5. containers
[ -e /.dockerenv ] && signals+=("/.dockerenv exists")
[ -e /run/.containerenv ] && signals+=("/run/.containerenv exists")
for c in /proc/1/cgroup /proc/self/cgroup; do
  if [ -r "$c" ] && grep -qiE 'docker|kubepods|containerd|lxc|libpod|crio' "$c"; then signals+=("container cgroup in $c"); break; fi
done

if [ "${#signals[@]}" -eq 0 ]; then
  echo "PHYSICAL=yes"
  exit 0
fi
for s in "${signals[@]}"; do echo "signal: $s"; done
echo "PHYSICAL=no"
[ "$REPORT" -eq 1 ] && exit 0
exit 1
