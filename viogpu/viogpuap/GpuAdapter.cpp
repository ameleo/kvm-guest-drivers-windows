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

GpuAdapter::GpuAdapter(const std::wstring LinkName)
    : m_hThread(NULL), m_hStopEvent(NULL), m_hResolutionEvent(NULL), m_hDC(NULL), m_hAdapter(NULL), m_Index(-1),
      m_DeviceId(0), m_SourceId(0), m_PathArrayElements(0), m_ModeInfoArrayElements(0), m_pDisplayPathInfo(NULL),
      m_pDisplayModeInfo(NULL), m_Flag(None)
{
    m_DeviceName = LinkName;
    PrintMessage(L"%ws %ws\n", __FUNCTIONW__, m_DeviceName.c_str());
    Init();
};

void GpuAdapter::UpdateDisplayConfig(void)
{
    PrintMessage(L"%ws\n", __FUNCTIONW__);

    UINT32 filter = QDC_ALL_PATHS;
    ClearDisplayConfig();

    if (FAILED(HRESULT_FROM_WIN32(::GetDisplayConfigBufferSizes(filter,
                                                                &m_PathArrayElements,
                                                                &m_ModeInfoArrayElements))))
    {
        PrintMessage(L"GetDisplayConfigBufferSizes faled\n");
        return;
    }

    m_pDisplayPathInfo = new DISPLAYCONFIG_PATH_INFO[m_PathArrayElements];
    m_pDisplayModeInfo = new DISPLAYCONFIG_MODE_INFO[m_ModeInfoArrayElements];
    ZeroMemory(m_pDisplayPathInfo, sizeof(DISPLAYCONFIG_PATH_INFO) * m_PathArrayElements);
    ZeroMemory(m_pDisplayModeInfo, sizeof(DISPLAYCONFIG_MODE_INFO) * m_ModeInfoArrayElements);

    if (SUCCEEDED(HRESULT_FROM_WIN32(::QueryDisplayConfig(filter,
                                                          &m_PathArrayElements,
                                                          m_pDisplayPathInfo,
                                                          &m_ModeInfoArrayElements,
                                                          m_pDisplayModeInfo,
                                                          NULL))))
    {

        for (UINT PathIdx = 0; PathIdx < GetNumbersOfPathArrayElements(); ++PathIdx)
        {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME SourceName = {};
            SourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            SourceName.header.size = sizeof(SourceName);
            SourceName.header.adapterId = m_pDisplayPathInfo[PathIdx].sourceInfo.adapterId;
            SourceName.header.id = m_pDisplayPathInfo[PathIdx].sourceInfo.id;

            if (SUCCEEDED(HRESULT_FROM_WIN32(::DisplayConfigGetDeviceInfo(&SourceName.header))))
            {
                if (wcscmp(m_DeviceName.c_str(), SourceName.viewGdiDeviceName) == 0)
                {
                    m_Index = PathIdx;
                    break;
                }
            }
        }
    }
};

DISPLAYCONFIG_MODE_INFO *GpuAdapter::GetDisplayConfig(UINT index)
{
    PrintMessage(L"%ws\n", __FUNCTIONW__);

    if (index < GetNumbersOfPathArrayElements())
    {
        UINT idx = m_pDisplayPathInfo[index].sourceInfo.modeInfoIdx;
        PrintMessage(L"%ws m_Index %d idx %d active %d\n",
                     __FUNCTIONW__,
                     index,
                     idx,
                     m_pDisplayPathInfo[index].flags & DISPLAYCONFIG_PATH_ACTIVE);
        if (idx < GetNumbersOfModeInfoArrayElements())
        {
            return &m_pDisplayModeInfo[idx];
        }
    }
    return NULL;
}

void GpuAdapter::ClearDisplayConfig(void)
{
    PrintMessage(L"%ws\n", __FUNCTIONW__);

    delete[] m_pDisplayPathInfo;
    delete[] m_pDisplayModeInfo;
    m_pDisplayPathInfo = NULL;
    m_pDisplayModeInfo = NULL;
    m_PathArrayElements = 0;
    m_ModeInfoArrayElements = 0;
}

void GpuAdapter::Init()
{
    PrintMessage(L"%ws\n", __FUNCTIONW__);

    m_hStopEvent = ::CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!m_hStopEvent)
    {
        return;
    }

    m_hDC = ::CreateDC(NULL, m_DeviceName.c_str(), NULL, NULL);

    D3DKMT_OPENADAPTERFROMHDC openAdapter = {0};
    openAdapter.hDc = m_hDC;

    NTSTATUS status = D3DKMTOpenAdapterFromHdc(&openAdapter);
    if (NT_SUCCESS(status))
    {
        UpdateDisplayConfig();
        m_hAdapter = openAdapter.hAdapter;
        // This is the head (VidPnSourceId) this \\.\DISPLAYn maps to. Used to
        // target the right head for the custom resolution and the display path.
        m_SourceId = openAdapter.VidPnSourceId;
        if (QueryAdapterId())
        {
            std::wstring EventName = GLOBAL_OBJECTS;
            EventName += RESOLUTION_EVENT_NAME;
            EventName += std::to_wstring(m_DeviceId);
            m_hResolutionEvent = ::OpenEvent(EVENT_ALL_ACCESS | EVENT_MODIFY_STATE, FALSE, EventName.c_str());
            if (m_hResolutionEvent == NULL)
            {
                PrintMessage(L"Cannot open event %ws Error = %d.\n", EventName.c_str(), GetLastError());
                return;
            }
            m_hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)ServiceThread, (LPVOID)this, 0, NULL);
            if (m_hThread == NULL)
            {
                PrintMessage(L"Cannot create thread Error = %d.\n", GetLastError());
                return;
            }
        }
    }
    SetStatus(Active);
}

bool GpuAdapter::QueryAdapterId()
{
    PrintMessage(L"%ws\n", __FUNCTIONW__);

    if (m_hAdapter)
    {
        VIOGPU_ESCAPE data{0};
        data.DataLength = sizeof(ULONG);
        data.Type = VIOGPU_GET_DEVICE_ID;

        D3DKMT_ESCAPE escape = {0};
        escape.hAdapter = m_hAdapter;
        escape.pPrivateDriverData = &data;
        escape.PrivateDriverDataSize = sizeof(data);

        NTSTATUS status = D3DKMTEscape(&escape);
        if (!NT_SUCCESS(status))
        {
            PrintMessage(L"D3DKMTEscape failed with status = 0x%x\n", status);
        }
        else
        {
            // Device id is shared by all heads of this single adapter; keep it only
            // for the (per-device) resolution event name. Do NOT overwrite m_Index,
            // which UpdateDisplayConfig already set to this display's path index.
            m_DeviceId = data.Id;
            return true;
        }
    }
    return false;
}

bool GpuAdapter::GetCurrentResolution(PVIOGPU_DISP_MODE mode)
{
    PrintMessage(L"%ws\n", __FUNCTIONW__);

    DISPLAYCONFIG_MODE_INFO *pConfig = GetDisplayConfig(m_Index);
    if (pConfig)
    {
        mode->XResolution = (USHORT)pConfig->sourceMode.width;
        mode->YResolution = (USHORT)pConfig->sourceMode.height;
        return true;
    }
    return false;
}

bool GpuAdapter::GetCustomResolution(PVIOGPU_DISP_MODE pmode)
{
    PrintMessage(L"%ws\n", __FUNCTIONW__);

    if (m_hAdapter && pmode)
    {
        VIOGPU_ESCAPE data{0};
        data.DataLength = sizeof(VIOGPU_DISP_MODE);
        data.Type = VIOGPU_GET_CUSTOM_RESOLUTION;
        data.ScanId = (USHORT)m_SourceId;

        D3DKMT_ESCAPE escape = {0};
        escape.hAdapter = m_hAdapter;
        escape.pPrivateDriverData = &data;
        escape.PrivateDriverDataSize = sizeof(data);

        NTSTATUS status = D3DKMTEscape(&escape);
        if (NT_SUCCESS(status))
        {
            pmode->XResolution = data.Resolution.XResolution;
            pmode->YResolution = data.Resolution.YResolution;
            PrintMessage(L"%ws (%dx%d)\n", __FUNCTIONW__, pmode->XResolution, pmode->YResolution);
            return true;
        }
        PrintMessage(L"D3DKMTEscape failed with status = 0x%0X\n", status);
    }
    return false;
}

bool GpuAdapter::SetResolution(PVIOGPU_DISP_MODE mode)
{
    PrintMessage(L"%ws\n", __FUNCTIONW__);

    // Guard the path index. m_Index is -1 until UpdateDisplayConfig FINDS this display by GDI name; if the query
    // failed or the device is transiently absent (mid-hotplug), m_Index stays -1/stale and m_pDisplayPathInfo may
    // be NULL or smaller → out-of-bounds / NULL deref → agent crash.
    // m_Index is ULONG, initialised to (ULONG)-1 = 0xffffffff and only set when UpdateDisplayConfig FINDS this
    // display by GDI name. A single >= bound catches BOTH the never-found/stale sentinel and any out-of-range
    // index (query failed / device transiently absent mid-hotplug) → avoids the OOB / NULL-deref crash.
    if (!m_pDisplayPathInfo || m_Index >= m_PathArrayElements)
    {
        PrintMessage(L"%ws: invalid path index %u (paths=%u) - skip\n", __FUNCTIONW__, m_Index, m_PathArrayElements);
        return false;
    }

    // Only resize a path Windows has ACTIVATED. An inactive path (single/transient) must not be forced here.
    if (!(m_pDisplayPathInfo[m_Index].flags & DISPLAYCONFIG_PATH_ACTIVE))
    {
        PrintMessage(L"%ws: path %u not active - skip\n", __FUNCTIONW__, m_Index);
        return false;
    }

    // Change ONLY this display's mode, via ChangeDisplaySettingsEx — do NOT re-apply the full CCD config
    // (SDC_USE_SUPPLIED_DISPLAY_CONFIG). Re-applying the whole topology re-imposes a possibly-STALE snapshot of the
    // OTHER heads' active/inactive state: e.g. head 0's instance, having queried during the transient single
    // phase, would deactivate head 1 the moment after Windows extended it → the "revert to single" on re-extend.
    // CDSEx touches only m_DeviceName's resolution and leaves the topology (which displays are active, positions)
    // untouched — so persisting it is SAFE (unlike a full-topology SetDisplayConfig+SDC_SAVE_TO_DATABASE, which
    // restored a stale FULL snapshot and bounced sizes). CDS_UPDATEREGISTRY writes this head's mode to the CCD-backed
    // store so that on the next topology change (dual->mono) Windows restores THIS size directly, instead of a stale
    // nominal one it then has to be corrected from → the residual dual->mono flash self-heals.
    DEVMODE dm = {0};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettings(m_DeviceName.c_str(), ENUM_CURRENT_SETTINGS, &dm))
    {
        PrintMessage(L"%ws: EnumDisplaySettings failed for %ws\n", __FUNCTIONW__, m_DeviceName.c_str());
        return false;
    }
    if (dm.dmPelsWidth == mode->XResolution && dm.dmPelsHeight == mode->YResolution)
    {
        return true;   // already at this size (belt-and-suspenders on top of SyncResolution's compare)
    }
    dm.dmPelsWidth = mode->XResolution;
    dm.dmPelsHeight = mode->YResolution;
    dm.dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT;
    LONG r = ChangeDisplaySettingsEx(m_DeviceName.c_str(), &dm, NULL, CDS_UPDATEREGISTRY, NULL);
    return (r == DISP_CHANGE_SUCCESSFUL);
}

void GpuAdapter::SyncResolution(void)
{
    PrintMessage(L"%ws\n", __FUNCTIONW__);

    // Serialize across the per-display agent instances. SetDisplayConfig applies
    // the FULL topology, so two instances resizing concurrently would each push a
    // stale snapshot and clobber the other head. Holding one lock while we
    // re-query the current config and change only our own path keeps the other
    // head stable when both instances update concurrently.
    HANDLE hLock = CreateMutex(NULL, FALSE, L"VioGpuResizeLock");
    if (hLock)
    {
        WaitForSingleObject(hLock, 5000);
    }

    VIOGPU_DISP_MODE custom = {0};
    UpdateDisplayConfig();
    if (GetCustomResolution(&custom) && custom.XResolution && custom.YResolution)
    {
        VIOGPU_DISP_MODE current = {0};
        GetCurrentResolution(&current);
        // Apply ONLY when the requested size actually differs from what is already applied: SetResolution
        // re-applies the FULL topology via SetDisplayConfig, which itself fires another resolution event, so a
        // blind re-apply on every event would feed back into itself and fight Windows' own topology handling.
        // Skipping the no-op re-apply breaks that loop while still following genuine client resizes.
        if (custom.XResolution != current.XResolution || custom.YResolution != current.YResolution)
        {
            SetResolution(&custom);
        }
    }

    if (hLock)
    {
        ReleaseMutex(hLock);
        CloseHandle(hLock);
    }
}

void GpuAdapter::Run()
{
    PrintMessage(L"%ws\n", __FUNCTIONW__);

    if (m_hThread != NULL && m_hStopEvent != NULL && m_hResolutionEvent != NULL)
    {
        SyncResolution();

        const HANDLE handles[] = {m_hStopEvent, m_hResolutionEvent};
        while (1)
        {
            // The resolution event is a driver-side NOTIFICATION event (IoCreateNotificationEvent), signalled
            // via KeSetEvent, which releases ALL waiters at once, so both heads' threads wake on every signal
            // with no miss. Wait INFINITE, no polling needed.
            if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0)
            {
                break;   // stop event
            }
            SyncResolution();
        }
    }
}

DWORD WINAPI GpuAdapter::ServiceThread(GpuAdapter *ptr)
{
    PrintMessage(L"%ws\n", __FUNCTIONW__);

    ptr->Run();
    return 0;
}

void GpuAdapter::Close()
{
    PrintMessage(L"%ws\n", __FUNCTIONW__);

    if (m_hThread != NULL)
    {
        if (m_hStopEvent != NULL)
        {
            SetEvent(m_hStopEvent);
            if (WAIT_TIMEOUT == WaitForSingleObject(m_hThread, 1000))
            {
                PrintMessage(L"Cannot close thread after 1 sec\n");
                TerminateThread(m_hThread, 0);
            }
        }
        m_hThread = NULL;
    }

    if (m_hStopEvent)
    {
        CloseHandle(m_hStopEvent);
        m_hStopEvent = NULL;
    }

    if (m_hResolutionEvent)
    {
        CloseHandle(m_hResolutionEvent);
        m_hResolutionEvent = NULL;
    }

    if (m_hAdapter)
    {
        D3DKMT_CLOSEADAPTER close = {m_hAdapter};
        NTSTATUS status = D3DKMTCloseAdapter(&close);
        if (!NT_SUCCESS(status))
        {
            PrintMessage(L"D3DKMTCloseAdapter failed with status = 0x%x.\n", status);
        }
        m_hAdapter = NULL;
    }

    if (m_hDC != NULL)
    {
        ReleaseDC(NULL, m_hDC);
        m_hDC = NULL;
    }
}
