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

| Feature | Description | Branch | Host requirements |
|---|---|---|---|
| **Multi-monitor (extend)** | Up to 8 heads with per-head framebuffers, VidPN sources/targets, EDIDs and cursor routing. Windows composes a native extended desktop. | `viogpu-multihead` | `-device virtio-vga,max_outputs=N` |
| **Per-head hotplug** | Heads follow the host's `enabled` state: connect/disconnect are delivered to Windows as monitor arrivals/departures (extend at boot, clean removal). | `viogpu-multihead` | — |
| **Arbitrary resolutions** | Client-driven resizes commit any size (not just the mode list), per head, with monotonic resource ids and detach-before-destroy to survive resize storms. | `viogpu-multihead` | — |
| **External monitor reporting** | Every head reports as an external (HD15) output, so Windows applies desktop-monitor logic (auto-extend) instead of laptop/projection behavior. | `viogpu-multihead` | — |
| **Resolution persistence** | Per-head resolution persisted so topology changes self-heal instead of flashing through stale modes. | `viogpu-multihead` | — |
| **100% scaling (DPI neutral)** | The EDID physical size is stripped so Windows always recommends 100% scaling — remote pixels map 1:1. | `viogpu-multihead` | — |
| **Distinct monitor identities** | Colliding EDID identity blocks (manufacturer/product/serial) are nudged per head — at boot, on hotplug and on the copy-of-primary fallback — so Windows never conflates twin monitors. | `viogpu-blob` | — |
| **Guest-RAM blob scanout** | `VIRTIO_GPU_F_RESOURCE_BLOB`: the framebuffer is exported host-side as a linear dmabuf (udmabuf) — no per-frame `TRANSFER_TO_HOST` copy. Double-buffered `SET_SCANOUT_BLOB` flip per present; idle desktops emit nothing. Feature-gated: without the host feature the classic 2D path is used unchanged. | `viogpu-blob` | `-device virtio-vga,blob=on` + `memory-backend-memfd,share=on` (plain device, not `-gl`) |
| **VSync control** | Full KMDOD VSync contract: `DxgkDdiControlInterrupt` drives a simulated vblank timer at the rate of the host EDID's detailed timing (host keeps control of the refresh), notified **per connected target** (a single-target notify trips dxgkrnl's per-target watchdog on multi-head). Real frequencies in the signal info; DWM gets a paced clock. | `viogpu-blob` | — |
| **EDID hygiene** | Host-first monitor name (a host-authored 0xFC descriptor is kept verbatim; "QEMU Monitor" stamped only when absent); range-limits relaxed so no mode is pruned against the virtual monitor's analog caps; real `ActiveSize` in the video signal info. | `viogpu-blob` | — |
| **Hardened sync waiters** | The synchronous command waiters use a refcounted heap wait-context: a completion firing after a timeout can no longer corrupt a dead stack frame, and timeouts are survivable fallbacks instead of leaks. | `viogpu-blob` | — |

`viogpu-blob` is derived from `viogpu-multihead` and contains everything above.

## Known limitations

| Limitation | Notes |
|---|---|
| "Active signal mode" cosmetic | Windows may display a stale small target mode in Settings. Three alternative target-mode semantics were tried and each broke something functional (see the comment in `AddSingleTargetMode`); the real fix requires teaching the commit path about `source != target`. |
| `DxgkDdiGetScanLine` | `STATUS_NOT_IMPLEMENTED` (like remote desktop); the vsync interrupt itself is delivered. |
| Blob on `-gl` devices | The virgl command processor of `virtio-vga-gl` only wires `SET_SCANOUT_BLOB` in recent QEMU/virglrenderer builds (`HAVE_VIRGL_RESOURCE_BLOB`). Use the plain `virtio-vga` device for the blob path. |

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

Built with the EWDK (`build.bat viogpu.sln "Win11" x64 /Rebuild`). The driver version lives in
`build/Driver.RHEL.props` (`100.200.<build>.0` scheme) and must be bumped for the guest's PnP to
adopt a new binary. Self-signed deployments need CKS + Secure Boot or test signing in the guest.
