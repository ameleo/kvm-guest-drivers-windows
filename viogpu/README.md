# viogpu — VirtIO GPU display-only driver (Ameleo fork)

> **⚠️ This is not the official virtio-win driver.** This repository is [Ameleo](https://www.ameleo.fr)'s
> fork of the official [virtio-win/kvm-guest-drivers-windows](https://github.com/virtio-win/kvm-guest-drivers-windows)
> project. The viogpu driver on this branch carries features that are **not upstream** (multi-monitor,
> blob scanout, VSync control) and is not WHQL-signed. For the official drivers, use the
> [virtio-win releases](https://github.com/virtio-win/kvm-guest-drivers-windows/releases) or your
> distribution's virtio-win packages.

Windows display-only (KMDOD) driver for the QEMU/KVM `virtio-vga` / `virtio-gpu-pci` device,
based on [virtio-win/kvm-guest-drivers-windows](https://github.com/virtio-win/kvm-guest-drivers-windows),
extended for remote-desktop / DaaS use: multi-monitor, arbitrary client-driven resolutions,
zero-copy scanout and a real vsync.

## Features

| Feature | Description | Host requirements |
|---|---|---|
| **Multi-monitor (extend)** | Up to 8 heads with per-head framebuffers, VidPN sources/targets, EDIDs and cursor routing. Windows composes a native extended desktop. | `-device virtio-vga,max_outputs=N` |
| **Routing by target** | The pointer and the agent's resolution requests go to the head a source is actually shown on, not the head numbered like the source, so they stay right after Win+P ("second screen only") or a reordered topology. A head a source leaves is released instead of keeping a frozen last frame. | — |
| **Per-head hotplug** | Heads follow the host's `enabled` state: connect/disconnect are delivered to Windows as monitor arrivals/departures (extend at boot, clean removal). | — |
| **Arbitrary resolutions** | Client-driven resizes commit any size (not just the mode list), per head, with monotonic resource ids and detach-before-destroy to survive resize storms. | — |
| **External monitor reporting** | Every head reports as an external (HD15) output, so Windows applies desktop-monitor logic (auto-extend) instead of laptop/projection behavior. | — |
| **Resolution persistence** | Per-head resolution persisted so topology changes self-heal instead of flashing through stale modes. | — |
| **Distinct monitor identities** | Colliding EDID identity blocks (manufacturer/product/serial) are nudged per head — at boot, on hotplug, on the copy-of-primary fallback, and on the built-in EDID when the host gives none — so Windows never conflates twin monitors. | — |
| **Guest-RAM blob scanout** | `VIRTIO_GPU_F_RESOURCE_BLOB`: the framebuffer is exported host-side as a linear dmabuf (udmabuf) — no per-frame `TRANSFER_TO_HOST` copy. Double-buffered `SET_SCANOUT_BLOB` flip per present; idle desktops emit nothing. Rows are aligned to 256 bytes so host GPUs import the dmabuf directly. Feature-gated: without the host feature the classic 2D path is used unchanged. When the host refuses the blob resources for a mode (no memfd backend, a mode above udmabuf's size limit), that head falls back to the 2D path for this mode instead of staying black. | `-device virtio-vga,blob=on` + `memory-backend-memfd,share=on` (plain device, not `-gl`). Large modes need the host's `udmabuf` limits raised (`list_limit`, `size_limit_mb`, 64 MiB by default: an 8K head is about 132 MB) |
| **Write-combined framebuffer** | The blob framebuffer is allocated write-combined (real-hardware semantics): CPU writes bypass the cache, so when KVM honors the guest PAT the host sees WC pages and a dmabuf import has no CPU cache to flush — eliminates the per-frame `wbinvd`/IPI storms on the host (measured: ~20% of a host core per active VM). | recommended: `-accel kvm,honor-guest-pat=on` — without it KVM downgrades the guest's WC mapping to cached and the host-side benefit is lost (the driver still works) |
| **VSync control** | Full KMDOD VSync contract: `DxgkDdiControlInterrupt` drives a simulated vblank timer at the rate of the host EDID's detailed timing (host keeps control of the refresh), notified **per connected target** (a single-target notify trips dxgkrnl's per-target watchdog on multi-head). The timer is a high-resolution one with the period in 100 ns units, so the vblanks come at the announced rate (a millisecond timer gave about 64 Hz for 60). Real frequencies in the signal info; DWM gets a paced clock. | — |
| **EDID hygiene** | Range-limits relaxed so no mode is pruned against the virtual monitor's analog caps; the block-0 checksum is recomputed after every edit; real `ActiveSize` in the video signal info. | — |
| **Hardened sync waiters** | The synchronous command waiters use a refcounted heap wait-context: a completion firing after a timeout can no longer corrupt a dead stack frame, and timeouts are survivable fallbacks instead of leaks. A command that cannot be queued (full virtqueue) is reported and released instead of being dropped silently. | — |
| **Agent compatibility** | The resolution escape accepts both the multi-head layout of this fork's `viogpuap` and the 8-byte single-head layout of upstream tools (read as a request for head 0). The INF does not install `viogpuap.exe`, so a guest may run an agent older or newer than the driver. | — |
| **Virgl host safety** | On a `-gl` device (`VIRTIO_GPU_F_VIRGL`), blob scanout is not negotiated and the 2D path is used. The features the host offers are written under the device's registry key (`HostFeatureVirgl`, `HostFeatureResourceBlob`, ...) so the path a VM takes can be checked from inside the guest. | — |

All of the above is on the `viogpu-main` branch.

## Known limitations

| Limitation | Notes |
|---|---|
| "Active signal mode" cosmetic | Windows may display a stale small target mode in Settings. Three alternative target-mode semantics were tried and each broke something functional (see the comment in `AddSingleTargetMode`); the real fix requires teaching the commit path about `source != target`. |
| `DxgkDdiGetScanLine` | `STATUS_NOT_IMPLEMENTED` (like remote desktop); the vsync interrupt itself is delivered. |
| Blob double buffer | Both blob resources map the same guest pages, written in place: a fast-moving image can tear. A true double buffer would need a second framebuffer per head. |
| Blob on `-gl` devices | The virgl command processor of `virtio-vga-gl` does not scan out a guest-RAM blob, so the driver does not negotiate blob there and uses the 2D path. Use the plain `virtio-vga` device for the blob path. On a `-gl` device the display backend must honor the resource's `y0_top` flag, or the image is upside down. |

## Testing

`viogpu-stress.ps1` (shipped next to the driver package) is a guest-side mini-HCK harness:
PnP disable/enable reliability cycles, full mode-switch sweeps on every attached head, an optional
sleep phase, and a verdict built from the device problem code plus a System-log scan
(TDR/WATCHDOG/live dumps). Run as administrator inside the guest:

```powershell
powershell -ExecutionPolicy Bypass -File .\viogpu-stress.ps1 -PnpCycles 25 -ModeCycles 10
```

The upstream AutoHCK playlist (Red Hat CI) is the reference this harness approximates.

## Signing & sponsoring

These builds are self-signed: loading them requires test signing or a CKS/Secure Boot setup in the
guest. Producing drivers that load on a stock Windows (attestation signing through the Microsoft
Hardware Dev Center) requires an **EV code-signing certificate** — a few hundred euros per year that
this project currently doesn't have. If this fork is useful to you and you'd like to see properly
signed releases, sponsoring the certificate is the single most useful contribution. 🙂

## Building

Built with the EWDK. The driver version is passed at build time and never committed, so
`build/Driver.RHEL.props` keeps its upstream values:

```
msbuild viogpu\viogpu.sln -t:Rebuild -p:Configuration="Win11 Release" -p:Platform=x64 -p:SpectreMitigation=false ^
  -p:_RHEL_RELEASE_VERSION_=200 -p:_BUILD_MAJOR_VERSION_=<build> -p:_BUILD_MINOR_VERSION_=0
```

This gives `100.200.<build>.0`. Pass all three properties, and raise the build number for every
deployment: the guest's PnP only adopts a version above the installed one. Self-signed deployments
need CKS + Secure Boot or test signing in the guest.
