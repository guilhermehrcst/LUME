#!/bin/bash
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
CHECK="$HERE/check_physical_host.sh"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/bare.txt" <<'EOF'
[    0.019654] Booting paravirtualized kernel on bare hardware
EOF
out=$(bash "$CHECK" --classify-dmesg-file "$TMP/bare.txt")
[ "$out" = "DMESG_HYPERVISOR=no" ] || { echo "bare hardware misclassified: $out" >&2; exit 1; }

cat > "$TMP/kvm.txt" <<'EOF'
[    0.000000] Hypervisor detected: KVM
EOF
if bash "$CHECK" --classify-dmesg-file "$TMP/kvm.txt" > "$TMP/kvm.out"; then
  echo "KVM line was not classified as a hypervisor" >&2
  exit 1
fi
grep -qx 'DMESG_HYPERVISOR=yes' "$TMP/kvm.out"

cat > "$TMP/paravirt_guest.txt" <<'EOF'
[    0.000000] Booting paravirtualized kernel on Xen
EOF
if bash "$CHECK" --classify-dmesg-file "$TMP/paravirt_guest.txt" > "$TMP/paravirt.out"; then
  echo "guest paravirtualized-kernel line was not classified as a hypervisor" >&2
  exit 1
fi
grep -qx 'DMESG_HYPERVISOR=yes' "$TMP/paravirt.out"

cat > "$TMP/mixed.txt" <<'EOF'
[    0.019654] Booting paravirtualized kernel on bare hardware
[    0.020000] Hypervisor detected: KVM
EOF
if bash "$CHECK" --classify-dmesg-file "$TMP/mixed.txt" > "$TMP/mixed.out"; then
  echo "real hypervisor signal was hidden by bare-hardware exception" >&2
  exit 1
fi
grep -qx 'DMESG_HYPERVISOR=yes' "$TMP/mixed.out"

echo "physical host dmesg classifier regression tests passed"
