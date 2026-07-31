/*
 * Copyright (C) 2021-2022 Red Hat, Inc.
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

#include "pch.h"
#include <iostream>

extern const char *LogFilePath = "viogpuap.log";

//
// Venus bring-up check: ask the driver for a slice of the host memory window, verify we can read what it
// seeded there, write our own pattern over it, and hand it back. The driver then re-reads through its own
// kernel mapping, which is what proves the two mappings are the same memory rather than two copies.
//
// Deliberately standalone: it opens the adapter itself instead of going through GpuAdaptersMgr, so a bring-up
// experiment cannot disturb the resolution service.
//
static int VenusMapTest(void)
{
    D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME open = {0};
    wcscpy_s(open.DeviceName, L"\\\\.\\DISPLAY1");

    NTSTATUS status = D3DKMTOpenAdapterFromGdiDisplayName(&open);
    if (!NT_SUCCESS(status))
    {
        PrintMessage(L"OpenAdapter failed 0x%x\n", status);
        return 1;
    }

    VIOGPU_ESCAPE data{0};
    data.Type = VIOGPU_VENUS_MAP;
    data.DataLength = sizeof(VIOGPU_VENUS_MAPPING);

    D3DKMT_ESCAPE escape = {0};
    escape.hAdapter = open.hAdapter;
    escape.pPrivateDriverData = &data;
    escape.PrivateDriverDataSize = sizeof(data);

    status = D3DKMTEscape(&escape);
    if (!NT_SUCCESS(status))
    {
        PrintMessage(L"VENUS_MAP escape failed 0x%x\n", status);
        D3DKMT_CLOSEADAPTER close = {open.hAdapter};
        D3DKMTCloseAdapter(&close);
        return 1;
    }

    volatile ULONG *p = (volatile ULONG *)(ULONG_PTR)data.VenusMapping.UserAddress;
    const ULONG dwords = (ULONG)(data.VenusMapping.Size / sizeof(ULONG));

    PrintMessage(L"mapped res=%u size=0x%llx at %p map_info=%u\n",
                 data.VenusMapping.ResourceId,
                 data.VenusMapping.Size,
                 p,
                 data.VenusMapping.MapInfo);

    int rc = 0;
    ULONG bad = 0;

    __try
    {
        for (ULONG i = 0; i < dwords; i++)
        {
            if (p[i] != (data.VenusMapping.Pattern | (i & 0xFFFF)))
            {
                if (bad == 0)
                {
                    PrintMessage(L"read mismatch at %u: expected 0x%08X got 0x%08X\n",
                                 i,
                                 data.VenusMapping.Pattern | (i & 0xFFFF),
                                 p[i]);
                }
                bad++;
            }
        }

        if (bad == 0)
        {
            PrintMessage(L"read back %u dwords seeded by the driver: OK\n", dwords);
        }
        else
        {
            PrintMessage(L"%u/%u dwords wrong\n", bad, dwords);
            rc = 1;
        }

        for (ULONG i = 0; i < dwords; i++)
        {
            p[i] = (data.VenusMapping.Pattern | (i & 0xFFFF)) ^ VIOGPU_VENUS_XOR;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        PrintMessage(L"faulted touching the mapping: 0x%x\n", GetExceptionCode());
        rc = 1;
    }

    VIOGPU_ESCAPE undo{0};
    undo.Type = VIOGPU_VENUS_UNMAP;
    undo.DataLength = sizeof(VIOGPU_VENUS_MAPPING);
    escape.pPrivateDriverData = &undo;
    escape.PrivateDriverDataSize = sizeof(undo);

    status = D3DKMTEscape(&escape);
    if (!NT_SUCCESS(status))
    {
        PrintMessage(L"VENUS_UNMAP escape failed 0x%x\n", status);
        rc = 1;
    }
    else
    {
        PrintMessage(L"unmapped; the driver logs the round-trip verdict on its trace\n");
    }

    D3DKMT_CLOSEADAPTER close = {open.hAdapter};
    D3DKMTCloseAdapter(&close);
    return rc;
}

//
// Fire the driver's submission check. Everything interesting is on the driver's trace: from here we only learn
// whether the escape succeeded.
//
static int VenusSubmitTest(void)
{
    D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME open = {0};
    wcscpy_s(open.DeviceName, L"\\\\.\\DISPLAY1");

    NTSTATUS status = D3DKMTOpenAdapterFromGdiDisplayName(&open);
    if (!NT_SUCCESS(status))
    {
        PrintMessage(L"OpenAdapter failed 0x%x\n", status);
        return 1;
    }

    VIOGPU_ESCAPE data{0};
    data.Type = VIOGPU_VENUS_SUBMIT;
    data.DataLength = sizeof(VIOGPU_VENUS_MAPPING);

    D3DKMT_ESCAPE escape = {0};
    escape.hAdapter = open.hAdapter;
    escape.pPrivateDriverData = &data;
    escape.PrivateDriverDataSize = sizeof(data);

    status = D3DKMTEscape(&escape);
    if (NT_SUCCESS(status))
    {
        PrintMessage(L"submit escape returned OK; see the driver trace for the per-ring fence timings\n");
    }
    else
    {
        PrintMessage(L"VENUS_SUBMIT escape failed 0x%x\n", status);
    }

    D3DKMT_CLOSEADAPTER close = {open.hAdapter};
    D3DKMTCloseAdapter(&close);
    return NT_SUCCESS(status) ? 0 : 1;
}

//
// Talk to the Vulkan ICD directly, without a loader.
//
// vulkaninfo needs the Vulkan SDK, and the runtime loader needs installing too - neither is on a bring-up VM
// with no working display. But a loader does nothing magic: it finds the ICD, negotiates an interface version,
// asks it for vkCreateInstance, and calls it. That is short enough to do here, and it removes the last reason
// to install anything in the guest.
//
// The Vulkan types are declared inline rather than pulled from the SDK headers, for the same reason. Only the
// head of VkPhysicalDeviceProperties is described; the rest is left as padding, since the ICD writes the whole
// structure and we read the fields we name.
//
typedef int VkResult;
typedef void *VkInstance;
typedef void *VkPhysicalDevice;

#define VK_STRUCTURE_TYPE_APPLICATION_INFO     0
#define VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO 1
#define VK_MAKE_API_VERSION(v, ma, mi, pa)     ((((UINT32)(v)) << 29) | (((UINT32)(ma)) << 22) | (((UINT32)(mi)) << 12) | ((UINT32)(pa)))

typedef struct
{
    UINT32 sType;
    const void *pNext;
    const char *pApplicationName;
    UINT32 applicationVersion;
    const char *pEngineName;
    UINT32 engineVersion;
    UINT32 apiVersion;
} VkApplicationInfoLite;

typedef struct
{
    UINT32 sType;
    const void *pNext;
    UINT32 flags;
    const VkApplicationInfoLite *pApplicationInfo;
    UINT32 enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    UINT32 enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
} VkInstanceCreateInfoLite;

typedef struct
{
    UINT32 apiVersion;
    UINT32 driverVersion;
    UINT32 vendorID;
    UINT32 deviceID;
    UINT32 deviceType;
    char deviceName[256];
    // pipelineCacheUUID, limits and sparseProperties follow; the caller's buffer covers them.
} VkPhysicalDevicePropertiesHead;

typedef void *(__stdcall *PFN_vkGetInstanceProcAddr)(VkInstance, const char *);
typedef VkResult(__stdcall *PFN_vkNegotiate)(UINT32 *);
typedef VkResult(__stdcall *PFN_vkCreateInstance)(const VkInstanceCreateInfoLite *, const void *, VkInstance *);
typedef VkResult(__stdcall *PFN_vkEnumeratePhysicalDevices)(VkInstance, UINT32 *, VkPhysicalDevice *);
typedef void(__stdcall *PFN_vkGetPhysicalDeviceProperties)(VkPhysicalDevice, void *);

static int VenusIcdTest(PCWSTR path)
{
    HMODULE icd = LoadLibraryW(path);
    if (icd == NULL)
    {
        PrintMessage(L"LoadLibrary(%ws) failed, error %u\n", path, GetLastError());
        return 1;
    }
    PrintMessage(L"loaded %ws\n", path);

    PFN_vkNegotiate negotiate = (PFN_vkNegotiate)GetProcAddress(icd, "vk_icdNegotiateLoaderICDInterfaceVersion");
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(icd, "vk_icdGetInstanceProcAddr");

    if (gipa == NULL)
    {
        PrintMessage(L"no vk_icdGetInstanceProcAddr export; this is not a Vulkan ICD\n");
        return 1;
    }

    if (negotiate != NULL)
    {
        UINT32 version = 5; // what a current loader asks for; the ICD lowers it if it speaks less
        VkResult r = negotiate(&version);
        PrintMessage(L"ICD interface negotiated: version %u (result %d)\n", version, r);
    }

    PFN_vkCreateInstance createInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    if (createInstance == NULL)
    {
        PrintMessage(L"ICD does not offer vkCreateInstance\n");
        return 1;
    }

    VkApplicationInfoLite app = {0};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "viogpuap";
    app.apiVersion = VK_MAKE_API_VERSION(0, 1, 1, 0);

    VkInstanceCreateInfoLite ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;

    VkInstance instance = NULL;
    VkResult r = createInstance(&ci, NULL, &instance);
    if (r != 0)
    {
        PrintMessage(L"vkCreateInstance failed: %d\n", r);
        return 1;
    }
    PrintMessage(L"vkCreateInstance OK\n");

    PFN_vkEnumeratePhysicalDevices enumerate =
        (PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties getProps =
        (PFN_vkGetPhysicalDeviceProperties)gipa(instance, "vkGetPhysicalDeviceProperties");

    if (enumerate == NULL)
    {
        PrintMessage(L"no vkEnumeratePhysicalDevices\n");
        return 1;
    }

    UINT32 count = 0;
    r = enumerate(instance, &count, NULL);
    PrintMessage(L"vkEnumeratePhysicalDevices: %u device(s), result %d\n", count, r);

    if (r != 0 || count == 0)
    {
        PrintMessage(L"no physical device: the ICD reached the driver but found nothing usable\n");
        return 1;
    }

    VkPhysicalDevice devices[8];
    if (count > 8)
    {
        count = 8;
    }
    r = enumerate(instance, &count, devices);
    if (r != 0)
    {
        PrintMessage(L"second vkEnumeratePhysicalDevices failed: %d\n", r);
        return 1;
    }

    for (UINT32 i = 0; i < count && getProps != NULL; i++)
    {
        // Generous buffer: the ICD fills the whole VkPhysicalDeviceProperties, we only name its head.
        BYTE buffer[2048] = {0};
        getProps(devices[i], buffer);

        VkPhysicalDevicePropertiesHead *p = (VkPhysicalDevicePropertiesHead *)buffer;
        PrintMessage(L"device %u: '%hs'\n", i, p->deviceName);
        PrintMessage(L"  api %u.%u.%u  driver 0x%08X  vendor 0x%04X  device 0x%04X  type %u\n",
                     (p->apiVersion >> 22) & 0x7F,
                     (p->apiVersion >> 12) & 0x3FF,
                     p->apiVersion & 0xFFF,
                     p->driverVersion,
                     p->vendorID,
                     p->deviceID,
                     p->deviceType);
    }

    PrintMessage(L"VENUS ICD OK\n");
    return 0;
}

int _cdecl wmain(__in ULONG argc, __in_ecount(argc) PWCHAR argv[])
{
    PrintMessage(L"viogpuap.exe built on %ws %ws\n", _CRT_WIDE(__DATE__), _CRT_WIDE(__TIME__));

    if (argc >= 2 && _wcsicmp(argv[1], L"venusicd") == 0)
    {
        return VenusIcdTest(argc >= 3 ? argv[2] : L"vulkan_virtio.dll");
    }

    if (argc == 2 && _wcsicmp(argv[1], L"venustest") == 0)
    {
        return VenusMapTest();
    }

    if (argc == 2 && _wcsicmp(argv[1], L"venussubmit") == 0)
    {
        return VenusSubmitTest();
    }

    PipeClient *pClient = NULL;
    if (argc == 2 && iswdigit(argv[1][0]))
    {
        std::wstring pipename = PIPE_NAME;
        pipename.append(argv[1]);
        if (pipename.length())
        {
            pClient = new PipeClient(pipename);
            pClient->Init();
        }
    }

    GpuAdaptersMgr *m_pMgr;
    m_pMgr = new GpuAdaptersMgr();
    if (!m_pMgr || !m_pMgr->Init())
    {
        ErrorHandler("Start GpuAdaptersMgr", GetLastError());
        return 0;
    }

    if (pClient)
    {
        pClient->WaitRunning();
    }
    else
    {
        while (getchar() != 'q')
            ;
    }

    m_pMgr->Close();
    delete m_pMgr;
    m_pMgr = NULL;

    if (pClient)
    {
        pClient->Close();
        delete pClient;
        pClient = NULL;
    }
    return 0;
}
