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

int _cdecl wmain(__in ULONG argc, __in_ecount(argc) PWCHAR argv[])
{
    PrintMessage(L"viogpuap.exe built on %ws %ws\n", _CRT_WIDE(__DATE__), _CRT_WIDE(__TIME__));

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
