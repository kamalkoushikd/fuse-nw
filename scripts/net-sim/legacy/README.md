# net-sim: two-VM bridge harness

Runs a real fuse transfer between two minimal KVM guests joined by a Linux
bridge instead of loopback. That gets you a real UDP/IP kernel path and 1500
MTU (loopback lets fuse adapt into a 16 KiB block a real link never would,
per the top-level README's caveats) while staying host-local - VM-to-VM over
a bridge has no physical NIC in the loop, so it's still fast.

Linux only (bridges, tap devices, KVM). Needs `/dev/kvm` and root once for
network setup; everything else runs as your user.

## Use

```sh
scripts/net-sim/setup-net.sh     # once per boot: bridge fusebr0 + 2 taps (needs root)
scripts/net-sim/build-guest.sh   # once, or after rebuilding fuse: rootfs + disk image (needs root once, for dnf --installroot)
scripts/net-sim/run-test.sh [size-MiB] [lanes]   # defaults: 64 MiB, 4 lanes
scripts/net-sim/teardown.sh      # removes the bridge/taps again
```

`build-guest.sh` picks up whichever `fuse_quickstart_send`/`recv` it finds
most recently built under the repo (`cmake --build --preset default` first).
Rerun it with `--force` after rebuilding fuse to refresh the copied binaries.

Root is needed for: creating the bridge/tap devices (`setup-net.sh`), and
for `dnf --installroot` during the image build, since RPM scriptlets
(ldconfig, chown to root:root) require it even into an alternate root. Both
scripts batch their privileged work into one `pkexec` call (one graphical
password prompt) - there's no controlling tty here for plain `sudo` to
prompt on.

## How it's built

- **Guest kernel**: your own running kernel (`/boot/vmlinuz-$(uname -r)`),
  booted directly via `-kernel`/`-initrd` - no bootloader, no separate
  kernel build.
- **Guest initramfs**: a generic (`--no-hostonly`) dracut image, built once
  and cached. Needed because your host's own initramfs is hostonly-trimmed
  for your bare-metal hardware and doesn't carry the virtio_net module.
- **Guest rootfs**: `dnf --installroot` with `glibc`/`libstdc++`/`libgcc`/
  `busybox`, plus fuse's own binaries + `libfuse_proto.so`/`libwolfssl.so`
  copied in from your build tree. Built straight into an ext4 image via
  `mkfs.ext4 -d` - no loop-mount.
- **Guest init**: `guest-init.sh` runs as PID 1 (`init=/init` on the kernel
  cmdline, bypassing systemd entirely) - it brings the net link up with a
  static IP from the cmdline (`fuserole=`, `fuseip=`, `fusepeer=`,
  `fusesize=`, `fuselanes=`) and runs `fuse_quickstart_send`/`recv`
  directly, then powers off. `net.ifnames=0` is required: dracut's generic
  initramfs still runs systemd-udevd during early boot, which renames the
  virtio NIC from `eth0` before our init ever gets a chance to see it.
- Each VM's disk is the *same* cached image opened with `snapshot=on` (a
  private, ephemeral overlay per VM), so the two VMs never write-share the
  base image.

Everything under `.cache/` is generated and gitignored; delete it any time
to force a clean rebuild.

## Podman: run it as a pod instead

`podman/` packages the same two VMs as a podman pod (`rx` + `tx` containers
sharing a network namespace, plus an init container that creates the
bridge), so the test is one command instead of four scripts:

```sh
scripts/net-sim/build-guest.sh          # once - same as above, still needed
podman build -t fuse-netsim scripts/net-sim/podman
scripts/net-sim/podman/run.sh           # play the pod, print results, tear it down
```

No host bridge, no `setup-net.sh`, no `pkexec` - the bridge and taps live
*inside the pod's own network namespace* (rootless podman gives that
namespace real `CAP_NET_ADMIN`, no host root needed), created once by the
`net-setup` init container and joined by `rx` and `tx` in turn. The container
image is just qemu + iproute2; the kernel (`/boot`), initramfs and disk image
are bind-mounted in from the host at run time (via `hostPath` volumes in
`pod.yaml`), so the image isn't pinned to one kernel version and doesn't
need rebuilding when you rerun `build-guest.sh`.

Two things this needed that are worth knowing if you touch it:
- **`--security-opt label=disable` / `seLinuxOptions: {type: spc_t}`** -
  SELinux (enforcing by default on Fedora) blocks `container_t` from opening
  `/dev/net/tun` and `/dev/kvm` otherwise (confirmed via `ausearch -m avc`).
  This is podman's documented escape hatch for host-device passthrough, not
  a hack - the container already needs `CAP_NET_ADMIN` and raw device
  access, so it's not gaining meaningfully more trust than it already has.
- **`--network=none`** - rootless podman's default network (`pasta`) fails
  outright in this environment (`Failed to remount /: Permission denied`,
  unrelated to fuse). The pod doesn't need outbound connectivity anyway -
  `rx` and `tx` only ever talk to each other over the bridge `net-setup`
  creates - so `--network=none` sidesteps it rather than fixing it.

Only if none of that holds on your machine (SELinux disabled, or `pasta`
works fine, or rootless `/dev/kvm`/`/dev/net/tun` access is locked down by
policy) does this stop being worth it over the four plain scripts above -
which is the "only if it's not limited" case: this is genuinely equivalent
in what it tests, not a lesser version, so there's no reason to fight your
platform for it.

`podman kube down scripts/net-sim/podman/pod.yaml` tears the pod down by
hand if `run.sh` gets interrupted.

## Known ceiling

Boot goes through dracut's full systemd-in-initrd sequence before handing
off to our init - a few seconds of fixed per-run overhead, not part of the
measured transfer. Skipping that would mean hand-rolling a non-systemd
dracut/busybox initramfs; not worth it unless that overhead actually gets in
the way of what you're measuring.
