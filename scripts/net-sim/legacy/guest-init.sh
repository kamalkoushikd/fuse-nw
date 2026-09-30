#!/@BUSYBOX@ sh
# PID 1 inside the net-sim guest. No systemd, no udev - just enough to bring
# the virtio-net link up with a static IP and run one side of a fuse
# transfer. Role/IP/peer/size come in via the kernel cmdline (see run-test.sh).
BB=@BUSYBOX@

$BB mount -t proc proc /proc
$BB mount -t sysfs sysfs /sys
$BB mount -t devtmpfs devtmpfs /dev 2>/dev/null || true

read -r CMDLINE < /proc/cmdline
LANES=4
SIZE=64
for arg in $CMDLINE; do
  case "$arg" in
    fuserole=*) ROLE="${arg#fuserole=}" ;;
    fuseip=*) IP="${arg#fuseip=}" ;;
    fusepeer=*) PEER="${arg#fusepeer=}" ;;
    fusesize=*) SIZE="${arg#fusesize=}" ;;
    fuselanes=*) LANES="${arg#fuselanes=}" ;;
  esac
done
PORT=5000
PSK=2142edb68d8ce7923d4843c485d3ed5aef5016d6173f63b09920b526b6c6e76c

export PATH="/opt/fuse/bin:$PATH"
export LD_LIBRARY_PATH="/opt/fuse/lib"

$BB ip link set lo up
$BB ip link set eth0 up
$BB ip addr add "$IP/24" dev eth0

echo "=== fuse-net-sim: role=$ROLE ip=$IP peer=$PEER size=${SIZE}MiB lanes=$LANES ==="

case "$ROLE" in
  rx)
    echo "=== fuse-net-sim: listening on 0.0.0.0:$PORT ==="
    fuse_quickstart_recv 0.0.0.0 "$PORT" /tmp/out.bin "$LANES" "$PSK"
    echo "=== fuse-net-sim RESULT sha256 $($BB sha256sum /tmp/out.bin) ==="
    ;;
  tx)
    $BB dd if=/dev/urandom of=/tmp/in.bin bs=1M count="$SIZE" 2>/dev/null
    echo "=== fuse-net-sim RESULT sha256-source $($BB sha256sum /tmp/in.bin) ==="
    $BB sleep 3
    fuse_quickstart_send "$PEER" "$PORT" /tmp/in.bin "$LANES" "$PSK"
    ;;
  *)
    echo "=== fuse-net-sim: no/unknown fuserole=$ROLE on cmdline, dropping to shell ==="
    exec $BB sh
    ;;
esac

echo "=== fuse-net-sim: done, powering off ==="
$BB sync
$BB poweroff -f
