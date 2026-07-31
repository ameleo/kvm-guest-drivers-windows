/*
 * Copyright (C) 2019-2020 Red Hat, Inc.
 *
 * Written By: Vadim Rozenfeld <vrozenfe@redhat.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met :
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and / or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of their contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#pragma once

//
// ============================ ESCAPE ABI ============================
//
// The 3D escapes below are the Windows transposition of the Linux DRM virtgpu uapi, ioctl for ioctl, and each
// one names the ioctl it stands for. That is deliberate and it is the design rule here: where a Windows shape
// could be invented, follow what the Linux driver already does instead. It keeps guest behaviour comparable
// across the two, and it is what makes this reviewable by people who know virtio-gpu but not WDDM.
//
// Numbering follows osy/virtio-win-mesa, whose user-mode driver expects exactly these values -- and which is
// itself an extension of this driver's own escape numbering.
//
// NOTE: SET_CUSTOM_RESOLUTION used to sit at 0x002, where the 3D ABI puts GET_CAPS. It moved rather than the
// other, because a third-party UMD cannot be asked to renumber. The tool and the driver ship together, but a
// stale viogpuap against a new driver would ask for a capset while meaning a resolution.
//
#define VIOGPU_GET_DEVICE_ID         0x000
#define VIOGPU_GET_CUSTOM_RESOLUTION 0x001
#define VIOGPU_GET_CAPS              0x002 // DRM_IOCTL_VIRTGPU_GET_CAPS
#define VIOGPU_GET_PCI_INFO          0x003 // no ioctl: Linux reads this from sysfs via drmGetDevice
#define VIOGPU_SET_CUSTOM_RESOLUTION 0x00F

#define VIOGPU_RES_INFO 0x100 // DRM_IOCTL_VIRTGPU_RESOURCE_INFO
#define VIOGPU_RES_BUSY 0x101 // DRM_IOCTL_VIRTGPU_WAIT

#define VIOGPU_CTX_INIT 0x200 // DRM_IOCTL_VIRTGPU_CONTEXT_INIT

//
// User-mode pointers and handles cross this interface in fixed 64-bit fields, never pointer-sized ones: a
// 32-bit UMD under WOW64 and a 64-bit KMD must agree on every byte offset. The two-step cast through ULONG_PTR
// is what makes that round trip lossless.
//
typedef ULONGLONG VIOGPU_UM_PTR;

#define VIOGPU_UM_PTR_AS(type, v) ((type)(ULONG_PTR)(v))

/* VIOGPU_GET_CAPS: drm_virtgpu_get_caps {cap_set_id, cap_set_ver, addr, size} */
#pragma pack(1)
typedef struct _VIOGPU_CAPSET_REQ
{
    ULONG CapsetId;
    ULONG Version;
    ULONG Size;
    VIOGPU_UM_PTR Capset; // output buffer, Size bytes
} VIOGPU_CAPSET_REQ, *PVIOGPU_CAPSET_REQ;
#pragma pack()

/* VIOGPU_GET_PCI_INFO: what drmGetDevice reports on Linux, so Mesa can match the adapter to a PCI device */
#pragma pack(1)
typedef struct _VIOGPU_PCI_INFO_REQ
{
    ULONG Domain;
    ULONG Bus;
    ULONG Dev;
    ULONG Func;
} VIOGPU_PCI_INFO_REQ, *PVIOGPU_PCI_INFO_REQ;
#pragma pack()

/*
 * VIOGPU_CTX_INIT: drm_virtgpu_context_init, whose three params are CAPSET_ID, NUM_RINGS and POLL_RINGS_MASK.
 * NumRings is a guest-side count (Mesa asks for 64); the host is only ever told the capset. CtxId comes back
 * because Windows has no equivalent of the per-file context Linux creates implicitly.
 */
#pragma pack(1)
typedef struct _VIOGPU_CTX_INIT_REQ
{
    ULONG CapsetId;
    ULONG NumRings;
    UCHAR DebugName[64];
    ULONG CtxId; // out
} VIOGPU_CTX_INIT_REQ, *PVIOGPU_CTX_INIT_REQ;
#pragma pack()

/* VIOGPU_RES_INFO: drm_virtgpu_resource_info -- a WDDM allocation handle in, its virtio resource id out */
#pragma pack(1)
typedef struct _VIOGPU_RES_INFO_REQ
{
    ULONG ResHandle; // D3DKMT_HANDLE of the allocation
    ULONG Id;        // out: virtio resource id
    ULONG IsBlob;    // out
    ULONG BlobMem;   // out: VIRTIO_GPU_BLOB_MEM_*
    ULONGLONG BlobId;
    ULONGLONG Size;
} VIOGPU_RES_INFO_REQ, *PVIOGPU_RES_INFO_REQ;
#pragma pack()

/*
 * VIOGPU_RES_BUSY: drm_virtgpu_3d_wait. Wait == FALSE is the VIRTGPU_WAIT_NOWAIT flag -- report busy instead of
 * blocking; Wait == TRUE blocks until the resource is idle.
 */
#pragma pack(1)
typedef struct _VIOGPU_RES_BUSY_REQ
{
    ULONG ResHandle;
    ULONG Wait;
    ULONG IsBusy; // out
} VIOGPU_RES_BUSY_REQ, *PVIOGPU_RES_BUSY_REQ;
#pragma pack()

//
// ============================ ALLOCATIONS ============================
//
// What a user-mode driver passes to D3DKMTCreateAllocation. There is no ioctl to point at: on Linux a resource
// IS its handle, created by RESOURCE_CREATE_BLOB and named by a res_id. Windows inserts a WDDM allocation in
// between, so the private data describes what the resource should be, and RES_INFO later reports what it became.
//
// Resource creation is DEFERRED, and deliberately so. RESOURCE_CREATE_BLOB carries a context id, and
// CreateAllocation is adapter-scoped -- no context exists yet at that point. The blob is created when a context
// first needs it, which is also when Linux creates it.
//
#define VIOGPU_RESOURCE_TYPE_3D   0
#define VIOGPU_RESOURCE_TYPE_BLOB 1

// Same values as VIRTIO_GPU_BLOB_MEM_* / VIRTIO_GPU_BLOB_FLAG_USE_*: this ABI carries the protocol's own
// constants rather than a private encoding of them.
#define VIOGPU_BLOB_MEM_GUEST        0x0001
#define VIOGPU_BLOB_MEM_HOST3D       0x0002
#define VIOGPU_BLOB_MEM_HOST3D_GUEST 0x0003

#define VIOGPU_BLOB_FLAG_USE_MAPPABLE  0x0001
#define VIOGPU_BLOB_FLAG_USE_SHAREABLE 0x0002

#pragma pack(1)
typedef struct _VIOGPU_RESOURCE_BLOB_OPTIONS
{
    ULONG blob_mem;
    ULONG blob_flags;
    ULONGLONG blob_id;
} VIOGPU_RESOURCE_BLOB_OPTIONS, *PVIOGPU_RESOURCE_BLOB_OPTIONS;
#pragma pack()

/* virtio_gpu_resource_create_3d, minus the header -- same fields, same order */
#pragma pack(1)
typedef struct _VIOGPU_RESOURCE_3D_OPTIONS
{
    ULONG target;
    ULONG format;
    ULONG bind;
    ULONG width;
    ULONG height;
    ULONG depth;
    ULONG array_size;
    ULONG last_level;
    ULONG nr_samples;
    ULONG flags;
} VIOGPU_RESOURCE_3D_OPTIONS, *PVIOGPU_RESOURCE_3D_OPTIONS;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_CREATE_ALLOCATION_EXCHANGE
{
    ULONG Type;
    union {
        VIOGPU_RESOURCE_3D_OPTIONS Options3D;
        VIOGPU_RESOURCE_BLOB_OPTIONS OptionsBlob;
    } DUMMYUNIONNAME;
    ULONGLONG Size;
} VIOGPU_CREATE_ALLOCATION_EXCHANGE, *PVIOGPU_CREATE_ALLOCATION_EXCHANGE;
#pragma pack()

//
// ============================ COMMAND BUFFER ============================
//
// What Linux does with one ioctl per operation, Windows does with a DMA buffer: the user-mode driver builds a
// stream, dxgkrnl schedules it, and the miniport plays it back. So the stream is that ioctl sequence written
// down -- SUBMIT is EXECBUFFER, the transfers are TRANSFER_TO/FROM_HOST_3D, MAP_BLOB is RESOURCE_MAP_BLOB.
//
// Each entry is a header followed by exactly `size` bytes of body. The stream comes from user mode, so every
// header is bounds-checked before anything is played back.
//
#define VIOGPU_CMD_NOP                0x0
#define VIOGPU_CMD_SUBMIT             0x1 // DRM_IOCTL_VIRTGPU_EXECBUFFER
#define VIOGPU_CMD_TRANSFER_TO_HOST   0x2 // DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST
#define VIOGPU_CMD_TRANSFER_FROM_HOST 0x3 // DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST
#define VIOGPU_CMD_MAP_BLOB           0x4 // RESOURCE_MAP_BLOB, body = allocation indices
#define VIOGPU_CMD_UNMAP_BLOB         0x5 // RESOURCE_UNMAP_BLOB, body = allocation indices

// Mirrors the EXECBUFFER flags. RING_IDX says ring_idx below is meaningful; VIRGL picks the virgl context
// rather than the capset context, for a guest that drives both.
#define VIOGPU_EXECBUF_RING_IDX 0x04
#define VIOGPU_EXECBUF_VIRGL    0x08

#pragma pack(1)
typedef struct _VIOGPU_COMMAND_HDR
{
    ULONG type;
    ULONG size; // body bytes following this header
    ULONG flags;
    ULONG ring_idx;
} VIOGPU_COMMAND_HDR, *PVIOGPU_COMMAND_HDR;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_BOX
{
    ULONG x;
    ULONG y;
    ULONG z;
    ULONG width;
    ULONG height;
    ULONG depth;
} VIOGPU_BOX, *PVIOGPU_BOX;
#pragma pack()

/* virtio_gpu_transfer_host_3d, minus the header */
#pragma pack(1)
typedef struct _VIOGPU_TRANSFER_CMD
{
    ULONG res_id;
    VIOGPU_BOX box;
    ULONGLONG offset;
    ULONG level;
    ULONG stride;
    ULONG layer_stride;
} VIOGPU_TRANSFER_CMD, *PVIOGPU_TRANSFER_CMD;
#pragma pack()

//
// QueryAdapterInfo/UMDRIVERPRIVATE. Stands in for what Mesa learns on Linux from GETPARAM: whether the device
// does 3D at all, whether a host-visible window exists, and which capsets are present.
//
#define VIOGPU_IAM 0x56696f475055ull // "VioGPU" in hex, so a UMD can tell this driver from another

//
// NOT packed, deliberately, and this one matters: the reference user-mode driver declares it with natural
// alignment. Packing it would put SupportedCapsetIds at offset 12 instead of 16 and the client would read the
// wrong field -- a silent mismatch, since every value involved is plausible.
//
typedef struct _VIOGPU_ADAPTERINFO
{
    ULONGLONG IamVioGpu; // == VIOGPU_IAM
    struct
    {
        UINT Supports3d : 1;
        UINT HasShmem : 1; // host-visible window present (VIRTGPU_PARAM_HOST_VISIBLE)
        UINT Reserved : 30;
    } Flags;
    ULONGLONG SupportedCapsetIds; // bitmask, bit N set = capset id N present
    LUID AdapterLuid;
} VIOGPU_ADAPTERINFO, *PVIOGPU_ADAPTERINFO;

//
// Venus bring-up escapes. Not the final transport ABI -- these exist to prove, from user mode, that a slice of
// the host memory window can be reached and that both sides see the same bytes. Numbered well clear of the
// display escapes above so the two can never be confused.
//
#define VIOGPU_VENUS_MAP    0x10
#define VIOGPU_VENUS_UNMAP  0x11
#define VIOGPU_VENUS_SUBMIT 0x12

#pragma pack(1)
typedef struct _VIOGPU_VENUS_MAPPING
{
    ULONGLONG UserAddress; // where the window slice landed in the calling process
    ULONGLONG Size;
    ULONG ResourceId;
    ULONG MapInfo;   // host-dictated cache type
    ULONG Pattern;   // seed the driver wrote; user mode checks it, then writes Pattern ^ VIOGPU_VENUS_XOR back
    ULONG Reserved;
} VIOGPU_VENUS_MAPPING, *PVIOGPU_VENUS_MAPPING;
#pragma pack()

// Applied by user mode to every dword so the driver can tell a real round trip from stale or zeroed memory.
#define VIOGPU_VENUS_XOR       0x5A5A5A5Au
#define VIOGPU_VENUS_TEST_SIZE 0x10000

#pragma pack(1)
typedef struct _VIOGPU_DISP_MODE
{
    USHORT XResolution;
    USHORT YResolution;
} VIOGPU_DISP_MODE, *PVIOGPU_DISP_MODE;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_ESCAPE
{
    USHORT Type;
    USHORT DataLength;
    USHORT ScanId; // target head for GET/SET_CUSTOM_RESOLUTION (VidPnSourceId); 0 for GET_DEVICE_ID
    union {
        ULONG Id;
        VIOGPU_DISP_MODE Resolution;
        VIOGPU_VENUS_MAPPING VenusMapping;
        VIOGPU_CAPSET_REQ Capset;
        VIOGPU_PCI_INFO_REQ PciInfo;
        VIOGPU_CTX_INIT_REQ CtxInit;
        VIOGPU_RES_INFO_REQ ResourceInfo;
        VIOGPU_RES_BUSY_REQ ResourceBusy;
    } DUMMYUNIONNAME;
} VIOGPU_ESCAPE, *PVIOGPU_ESCAPE;
#pragma pack()

#define BASE_NAMED_OBJECTS    L"\\BaseNamedObjects\\"
#define GLOBAL_OBJECTS        L"Global\\"
#define RESOLUTION_EVENT_NAME L"VioGpuResolutionEvent"
