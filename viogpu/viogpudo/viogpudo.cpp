/*
 * Copyright (C) 2019-2022 Red Hat, Inc.
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

#include "helper.h"
#include "driver.h"
#include "viogpudo.h"
#include "baseobj.h"
#include "bitops.h"
#include "viogpum.h"
#include "edid.h"

#if !DBG
#include "viogpudo.tmh"
#endif

static UINT g_InstanceId = 0;

PAGED_CODE_SEG_BEGIN
VioGpuDod::VioGpuDod(_In_ DEVICE_OBJECT *pPhysicalDeviceObject)
    : m_pPhysicalDevice(pPhysicalDeviceObject), m_MonitorPowerState(PowerDeviceD0), m_AdapterPowerState(PowerDeviceD0),
      m_pHWDevice(NULL)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    *((UINT *)&m_Flags) = 0;
    RtlZeroMemory(&m_DxgkInterface, sizeof(m_DxgkInterface));
    RtlZeroMemory(&m_DeviceInfo, sizeof(m_DeviceInfo));
    RtlZeroMemory(&m_CurrentMode, sizeof(m_CurrentMode));
    RtlZeroMemory(&m_SystemDisplayInfo, sizeof(m_SystemDisplayInfo));
    RtlZeroMemory(&m_PointerShape, sizeof(m_PointerShape));
    m_PersistentDispMode0Width = 0;
    m_PersistentDispMode0Height = 0;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

VioGpuDod::~VioGpuDod(void)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s 0x%p\n", __FUNCTION__, m_pHWDevice));
    delete m_pHWDevice;
    m_pHWDevice = NULL;
}

BOOLEAN VioGpuDod::CheckHardware()
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_GRAPHICS_DRIVER_MISMATCH;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PCI_COMMON_HEADER Header = {0};
    ULONG BytesRead;

    Status = m_DxgkInterface.DxgkCbReadDeviceSpace(m_DxgkInterface.DeviceHandle,
                                                   DXGK_WHICHSPACE_CONFIG,
                                                   &Header,
                                                   0,
                                                   sizeof(Header),
                                                   &BytesRead);

    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("DxgkCbReadDeviceSpace failed with status 0x%X\n", Status));
        return FALSE;
    }
    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("<--- %s VendorId = 0x%04X DeviceId = 0x%04X\n", __FUNCTION__, Header.VendorID, Header.DeviceID));
    if (Header.VendorID == REDHAT_PCI_VENDOR_ID && Header.DeviceID == 0x1050)
    {
        SetVgaDevice(Header.SubClass == PCI_SUBCLASS_VID_VGA_CTLR);
        return TRUE;
    }

    return FALSE;
}

NTSTATUS VioGpuDod::StartDevice(_In_ DXGK_START_INFO *pDxgkStartInfo,
                                _In_ DXGKRNL_INTERFACE *pDxgkInterface,
                                _Out_ ULONG *pNumberOfViews,
                                _Out_ ULONG *pNumberOfChildren)
{
    PAGED_CODE();

    NTSTATUS Status;

    VIOGPU_ASSERT(pDxgkStartInfo != NULL);
    VIOGPU_ASSERT(pDxgkInterface != NULL);
    VIOGPU_ASSERT(pNumberOfViews != NULL);
    VIOGPU_ASSERT(pNumberOfChildren != NULL);

    if (pDxgkInterface->Size > sizeof(m_DxgkInterface))
    {
        RtlCopyMemory(&m_DxgkInterface, pDxgkInterface, sizeof(m_DxgkInterface));
        m_DxgkInterface.Version = DXGKDDI_INTERFACE_VERSION;
        m_DxgkInterface.Size = sizeof(m_DxgkInterface);
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("VIOGPU: Provided interface version cannot be used by Viogpudo (version %u, size %u), degrading to "
                  "version %u, size %u)\n",
                  pDxgkInterface->Version,
                  pDxgkInterface->Size,
                  m_DxgkInterface.Version,
                  m_DxgkInterface.Size));
    }
    else
    {
        RtlCopyMemory(&m_DxgkInterface, pDxgkInterface, pDxgkInterface->Size);
    }

    RtlZeroMemory(m_CurrentMode, sizeof(m_CurrentMode));
    for (UINT i = 0; i < MAX_VIEWS; i++)
    {
        m_CurrentMode[i].DispInfo.TargetId = D3DDDI_ID_UNINITIALIZED;
    }

    Status = m_DxgkInterface.DxgkCbGetDeviceInformation(m_DxgkInterface.DeviceHandle, &m_DeviceInfo);
    if (!NT_SUCCESS(Status))
    {
        VIOGPU_LOG_ASSERTION1("DxgkCbGetDeviceInformation failed with status 0x%X\n", Status);
        return Status;
    }

    if (CheckHardware())
    {
        m_pHWDevice = new (NonPagedPoolNx) VioGpuAdapter(this);
    }
    if (!m_pHWDevice)
    {
        Status = STATUS_NO_MEMORY;
        DbgPrint(TRACE_LEVEL_ERROR, ("StartDevice failed to allocate memory\n"));
        return Status;
    }

    Status = GetRegisterInfo();
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_WARNING, ("GetRegisterInfo failed with status 0x%X\n", Status));
    }

    Status = m_pHWDevice->HWInit(m_DeviceInfo.TranslatedResourceList, &m_CurrentMode[0].DispInfo);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("HWInit failed with status 0x%X\n", Status));
        return Status;
    }

    m_CurrentMode[0].RamFrameBuffer = m_pHWDevice->GetPciResources()[0].GetMappedAddress(0, 0);
    if (!m_CurrentMode[0].RamFrameBuffer)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("Failed to map RamFrameBuffer for VGA mode"));
    }

    Status = SetRegisterInfo(m_pHWDevice->GetInstanceId(), 0);
    if (!NT_SUCCESS(Status))
    {
        VIOGPU_LOG_ASSERTION1("RegisterHWInfo failed with status 0x%X\n", Status);
        return Status;
    }

    if (IsVgaDevice() && m_DxgkInterface.DxgkCbAcquirePostDisplayOwnership)
    {
        Status = m_DxgkInterface.DxgkCbAcquirePostDisplayOwnership(m_DxgkInterface.DeviceHandle, &m_SystemDisplayInfo);
    }

    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("DxgkCbAcquirePostDisplayOwnership failed with status 0x%X Width = %d\n",
                  Status,
                  m_SystemDisplayInfo.Width));
        VioGpuDbgBreak();
        return STATUS_UNSUCCESSFUL;
    }
    DbgPrint(TRACE_LEVEL_FATAL,
             ("DxgkCbAcquirePostDisplayOwnership Width = %d Height = %d Pitch = %d ColorFormat = %d\n",
              m_SystemDisplayInfo.Width,
              m_SystemDisplayInfo.Height,
              m_SystemDisplayInfo.Pitch,
              m_SystemDisplayInfo.ColorFormat));

    if (m_SystemDisplayInfo.Width == 0)
    {
        m_SystemDisplayInfo.Width = NOM_WIDTH_SIZE;
        m_SystemDisplayInfo.Height = NOM_HEIGHT_SIZE;
        m_SystemDisplayInfo.ColorFormat = D3DDDIFMT_X8R8G8B8;
        m_SystemDisplayInfo.Pitch = (BPPFromPixelFormat(m_SystemDisplayInfo.ColorFormat) / BITS_PER_BYTE) *
                                    m_SystemDisplayInfo.Width;
        m_SystemDisplayInfo.TargetId = 0;
        if (m_SystemDisplayInfo.PhysicAddress.QuadPart == 0LL)
        {
            m_SystemDisplayInfo.PhysicAddress = m_pHWDevice->GetFrameBufferPA();
        }
    }

    ULONG numScanouts = m_pHWDevice->GetNumScanouts();
    // Seed only the active scanouts. Leave phantom sources' TargetId at
    // D3DDDI_ID_UNINITIALIZED (set above) so we never advertise sources Windows
    // does not know about.
    for (UINT i = 0; i < numScanouts; i++)
    {
        m_CurrentMode[i].DispInfo.Width = max(MIN_WIDTH_SIZE, m_SystemDisplayInfo.Width);
        m_CurrentMode[i].DispInfo.Height = max(MIN_HEIGHT_SIZE, m_SystemDisplayInfo.Height);
        m_CurrentMode[i].DispInfo.ColorFormat = D3DDDIFMT_X8R8G8B8;
        m_CurrentMode[i].DispInfo.Pitch = (BPPFromPixelFormat(m_CurrentMode[i].DispInfo.ColorFormat) / BITS_PER_BYTE) *
                                          m_CurrentMode[i].DispInfo.Width;
        m_CurrentMode[i].DispInfo.TargetId = i;
        if (i == 0 && m_CurrentMode[i].DispInfo.PhysicAddress.QuadPart == 0LL &&
            m_SystemDisplayInfo.PhysicAddress.QuadPart != 0LL)
        {
            m_CurrentMode[i].DispInfo.PhysicAddress = m_SystemDisplayInfo.PhysicAddress;
        }
    }

    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("<--- %s ColorFormat = %d\n", __FUNCTION__, m_CurrentMode[0].DispInfo.ColorFormat));

    *pNumberOfViews = numScanouts;
    *pNumberOfChildren = numScanouts;
    m_Flags.DriverStarted = TRUE;
    // The one-shot post-start scan is triggered from the FIRST CommitVidPn (see TriggerInitialScan), not here:
    // DxgkCbIndicateChildStatus is illegal during StartDevice, and CommitVidPn is the point where Windows has
    // actually composed its boot topology -- the right moment to indicate the secondary arrival so it extends.
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuDod::StopDevice(VOID)
{
    PAGED_CODE();

    m_Flags.DriverStarted = FALSE;
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuDod::DispatchIoRequest(_In_ ULONG VidPnSourceId, _In_ VIDEO_REQUEST_PACKET *pVideoRequestPacket)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(VidPnSourceId);
    UNREFERENCED_PARAMETER(pVideoRequestPacket);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

PCHAR
DbgDevicePowerString(__in DEVICE_POWER_STATE Type)
{
    PAGED_CODE();

    switch (Type)
    {
        case PowerDeviceUnspecified:
            return "PowerDeviceUnspecified";
        case PowerDeviceD0:
            return "PowerDeviceD0";
        case PowerDeviceD1:
            return "PowerDeviceD1";
        case PowerDeviceD2:
            return "PowerDeviceD2";
        case PowerDeviceD3:
            return "PowerDeviceD3";
        case PowerDeviceMaximum:
            return "PowerDeviceMaximum";
        default:
            return "UnKnown Device Power State";
    }
}

PCHAR
DbgPowerActionString(__in POWER_ACTION Type)
{
    PAGED_CODE();

    switch (Type)
    {
        case PowerActionNone:
            return "PowerActionNone";
        case PowerActionReserved:
            return "PowerActionReserved";
        case PowerActionSleep:
            return "PowerActionSleep";
        case PowerActionHibernate:
            return "PowerActionHibernate";
        case PowerActionShutdown:
            return "PowerActionShutdown";
        case PowerActionShutdownReset:
            return "PowerActionShutdownReset";
        case PowerActionShutdownOff:
            return "PowerActionShutdownOff";
        case PowerActionWarmEject:
            return "PowerActionWarmEject";
        default:
            return "UnKnown Device Power State";
    }
}

NTSTATUS VioGpuDod::SetPowerState(_In_ ULONG HardwareUid,
                                  _In_ DEVICE_POWER_STATE DevicePowerState,
                                  _In_ POWER_ACTION ActionType)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(ActionType);
    NTSTATUS Status = STATUS_SUCCESS;

    DbgPrint(TRACE_LEVEL_FATAL,
             ("---> %s HardwareUid = 0x%x ActionType = %s DevicePowerState = %s AdapterPowerState = %s\n",
              __FUNCTION__,
              HardwareUid,
              DbgPowerActionString(ActionType),
              DbgDevicePowerString(DevicePowerState),
              DbgDevicePowerString(m_AdapterPowerState)));

    if (HardwareUid == DISPLAY_ADAPTER_HW_ID)
    {
        if (DevicePowerState == PowerDeviceD0)
        {
            if (m_DxgkInterface.DxgkCbAcquirePostDisplayOwnership)
            {
                Status = m_DxgkInterface.DxgkCbAcquirePostDisplayOwnership(m_DxgkInterface.DeviceHandle,
                                                                           &m_SystemDisplayInfo);
            }

            if (!NT_SUCCESS(Status))
            {
                DbgPrint(TRACE_LEVEL_FATAL,
                         ("DxgkCbAcquirePostDisplayOwnership failed with status 0x%X Width = %d\n",
                          Status,
                          m_SystemDisplayInfo.Width));
                VioGpuDbgBreak();
            }

            if (m_AdapterPowerState == PowerDeviceD3)
            {
                DXGKARG_SETVIDPNSOURCEVISIBILITY Visibility;
                Visibility.VidPnSourceId = D3DDDI_ID_ALL;
                Visibility.Visible = FALSE;
                SetVidPnSourceVisibility(&Visibility);
            }
            m_AdapterPowerState = DevicePowerState;
        }

        Status = m_pHWDevice->SetPowerState(&m_DeviceInfo, DevicePowerState, &m_CurrentMode[0]);
        return Status;
    }
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuDod::QueryChildRelations(_Out_writes_bytes_(ChildRelationsSize) DXGK_CHILD_DESCRIPTOR *pChildRelations,
                                        _In_ ULONG ChildRelationsSize)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    VIOGPU_ASSERT(pChildRelations != NULL);

    ULONG ChildRelationsCount = (ChildRelationsSize / sizeof(DXGK_CHILD_DESCRIPTOR)) - 1;
    VIOGPU_ASSERT(ChildRelationsCount <= MAX_CHILDREN);

    for (UINT ChildIndex = 0; ChildIndex < ChildRelationsCount; ++ChildIndex)
    {
        // Only the PRIMARY (scanout 0) is the VGA post-display output → always-connected / internal panel. The
        // SECONDARY heads are HOTPLUGGABLE: report them Interruptible + external (HD15) so Windows HONORS their
        // QueryChildStatus (connect/disconnect from m_bConnected). Reporting AlwaysConnected/INTERNAL for a
        // secondary made Windows treat it as a soldered panel and IGNORE QueryChildStatus entirely — so it stayed
        // connected regardless of GET_DISPLAY_INFO enabled=0 and a refused EDID → phantom 2nd monitor at boot.
        BOOLEAN primaryVga = (ChildIndex == 0) && IsVgaDevice();
        pChildRelations[ChildIndex].ChildDeviceType = TypeVideoOutput;
        pChildRelations[ChildIndex].ChildCapabilities.HpdAwareness = primaryVga ? HpdAwarenessAlwaysConnected
                                                                                : HpdAwarenessInterruptible;
        pChildRelations[ChildIndex].ChildCapabilities.Type.VideoOutput.InterfaceTechnology = primaryVga ? D3DKMDT_VOT_INTERNAL
                                                                                                        : D3DKMDT_VOT_HD15;
        pChildRelations[ChildIndex].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness = D3DKMDT_MOA_NONE;
        pChildRelations[ChildIndex].ChildCapabilities.Type.VideoOutput.SupportsSdtvModes = FALSE;
        pChildRelations[ChildIndex].AcpiUid = 0;
        pChildRelations[ChildIndex].ChildUid = ChildIndex;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuDod::QueryChildStatus(_Inout_ DXGK_CHILD_STATUS *pChildStatus, _In_ BOOLEAN NonDestructiveOnly)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    UNREFERENCED_PARAMETER(NonDestructiveOnly);
    VIOGPU_ASSERT(pChildStatus != NULL);
    VIOGPU_ASSERT(pChildStatus->ChildUid < MAX_CHILDREN);

    switch (pChildStatus->Type)
    {
        case StatusConnection:
            {
                // Per-child connected state (hotplug). m_pHWDevice tracks each
                // scanout's state; a child is connected only while the driver is
                // active AND its scanout is marked connected.
                pChildStatus->HotPlug.Connected =
                    IsDriverActive() && m_pHWDevice && m_pHWDevice->IsChildConnected(pChildStatus->ChildUid);
                return STATUS_SUCCESS;
            }

        case StatusRotation:
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("Child status being queried for StatusRotation even though D3DKMDT_MOA_NONE was reported"));
                return STATUS_INVALID_PARAMETER;
            }

        default:
            {
                DbgPrint(TRACE_LEVEL_WARNING, ("Unknown pChildStatus->Type (0x%I64x) requested.", pChildStatus->Type));
                return STATUS_NOT_SUPPORTED;
            }
    }
}

NTSTATUS VioGpuDod::QueryDeviceDescriptor(_In_ ULONG ChildUid, _Inout_ DXGK_DEVICE_DESCRIPTOR *pDeviceDescriptor)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    VIOGPU_ASSERT(pDeviceDescriptor != NULL);
    VIOGPU_ASSERT(ChildUid < MAX_CHILDREN);
    PBYTE edid = NULL;

    edid = m_pHWDevice->GetEdidData(ChildUid);

    if (!edid)
    {
        return STATUS_GRAPHICS_CHILD_DESCRIPTOR_NOT_SUPPORTED;
    }
    else if (pDeviceDescriptor->DescriptorOffset < EDID_RAW_BLOCK_SIZE)
    {
        ULONG len = min(pDeviceDescriptor->DescriptorLength,
                        (EDID_RAW_BLOCK_SIZE - pDeviceDescriptor->DescriptorOffset));
        RtlCopyMemory(pDeviceDescriptor->DescriptorBuffer, (edid + pDeviceDescriptor->DescriptorOffset), len);
        pDeviceDescriptor->DescriptorLength = len;
        return STATUS_SUCCESS;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA;
}

NTSTATUS VioGpuDod::QueryAdapterInfo(_In_ CONST DXGKARG_QUERYADAPTERINFO *pQueryAdapterInfo)
{
    PAGED_CODE();

    VIOGPU_ASSERT(pQueryAdapterInfo != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    switch (pQueryAdapterInfo->Type)
    {
        case DXGKQAITYPE_DRIVERCAPS:
            {
                if (!pQueryAdapterInfo->OutputDataSize)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pQueryAdapterInfo->OutputDataSize (0x%u) is smaller than sizeof(DXGK_DRIVERCAPS) "
                              "(0x%u)\n",
                              pQueryAdapterInfo->OutputDataSize,
                              sizeof(DXGK_DRIVERCAPS)));
                    return STATUS_BUFFER_TOO_SMALL;
                }

                DXGK_DRIVERCAPS *pDriverCaps = (DXGK_DRIVERCAPS *)pQueryAdapterInfo->pOutputData;
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("InterruptMessageNumber = %d, WDDMVersion = %d\n",
                          pDriverCaps->InterruptMessageNumber,
                          pDriverCaps->WDDMVersion));
                RtlZeroMemory(pDriverCaps, pQueryAdapterInfo->OutputDataSize /*sizeof(DXGK_DRIVERCAPS)*/);
                pDriverCaps->WDDMVersion = DXGKDDI_WDDMv1_2;
                pDriverCaps->HighestAcceptableAddress.QuadPart = (ULONG64)-1;

                if (IsPointerEnabled())
                {
                    pDriverCaps->MaxPointerWidth = POINTER_SIZE;
                    pDriverCaps->MaxPointerHeight = POINTER_SIZE;
                    pDriverCaps->PointerCaps.Color = 1;
                    pDriverCaps->PointerCaps.MaskedColor = 1;
                    // Advertise monochrome HW cursors too (e.g. the text I-beam); otherwise
                    // Windows renders them as a software cursor composited into the framebuffer
                    // — captured into the video/remote stream and laggy (issue #977 follow-up).
                    pDriverCaps->PointerCaps.Monochrome = 1;
                }
                pDriverCaps->SupportNonVGA = IsVgaDevice();
                pDriverCaps->SupportSmoothRotation = TRUE;
                DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s 1\n", __FUNCTION__));
                return STATUS_SUCCESS;
            }

        default:
            {
                DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
                return STATUS_NOT_SUPPORTED;
            }
    }
}

NTSTATUS VioGpuDod::SetPointerPosition(_In_ CONST DXGKARG_SETPOINTERPOSITION *pSetPointerPosition)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT(pSetPointerPosition != NULL);
    VIOGPU_ASSERT(pSetPointerPosition->VidPnSourceId < MAX_VIEWS);
    if (IsPointerEnabled() && pSetPointerPosition->VidPnSourceId < m_pHWDevice->GetNumScanouts())
    {
        return m_pHWDevice->SetPointerPosition(pSetPointerPosition, &m_CurrentMode[pSetPointerPosition->VidPnSourceId]);
    }
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS VioGpuDod::SetPointerShape(_In_ CONST DXGKARG_SETPOINTERSHAPE *pSetPointerShape)
{
    PAGED_CODE();

    VIOGPU_ASSERT(pSetPointerShape != NULL);

    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("<---> %s Height = %d, Width = %d, XHot= %d, YHot = %d SourceId = %d\n",
              __FUNCTION__,
              pSetPointerShape->Height,
              pSetPointerShape->Width,
              pSetPointerShape->XHot,
              pSetPointerShape->YHot,
              pSetPointerShape->VidPnSourceId));
    if (IsPointerEnabled() && pSetPointerShape->VidPnSourceId < m_pHWDevice->GetNumScanouts())
    {
        return m_pHWDevice->SetPointerShape(pSetPointerShape, &m_CurrentMode[pSetPointerShape->VidPnSourceId]);
    }
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS VioGpuDod::Escape(_In_ CONST DXGKARG_ESCAPE *pEscape)
{
    PAGED_CODE();

    VIOGPU_ASSERT(pEscape != NULL);

    DbgPrint(TRACE_LEVEL_INFORMATION, ("<---> %s Flags = %d\n", __FUNCTION__, pEscape->Flags.Value));

    return m_pHWDevice->Escape(pEscape);
}

NTSTATUS VioGpuDod::PresentDisplayOnly(_In_ CONST DXGKARG_PRESENT_DISPLAYONLY *pPresentDisplayOnly)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    VIOGPU_ASSERT(pPresentDisplayOnly != NULL);
    VIOGPU_ASSERT(pPresentDisplayOnly->VidPnSourceId < MAX_VIEWS);

    UINT srcId = pPresentDisplayOnly->VidPnSourceId;
    if (pPresentDisplayOnly->BytesPerPixel < 4 || srcId >= m_pHWDevice->GetNumScanouts())
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pPresentDisplayOnly->BytesPerPixel is 0x%d, which is lower than the allowed.\n",
                  pPresentDisplayOnly->BytesPerPixel));
        return STATUS_INVALID_PARAMETER;
    }

    CURRENT_MODE *pMode = &m_CurrentMode[srcId];

    if ((m_MonitorPowerState > PowerDeviceD0) || (pMode->Flags.SourceNotVisible))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("<--- %s Source is not visiable\n", __FUNCTION__));
        return STATUS_SUCCESS;
    }

    if (!pMode->Flags.FrameBufferIsActive)
    {
        DbgPrint(TRACE_LEVEL_WARNING, ("<--- %s Frame Buffer is Not active\n", __FUNCTION__));
        return STATUS_UNSUCCESSFUL;
    }

    D3DKMDT_VIDPN_PRESENT_PATH_ROTATION RotationNeededByFb = pPresentDisplayOnly->Flags.Rotate ? pMode->Rotation
                                                                                               : D3DKMDT_VPPR_IDENTITY;
    BYTE *pDst = (BYTE *)pMode->FrameBuffer;
    UINT DstBitPerPixel = BPPFromPixelFormat(pMode->DispInfo.ColorFormat);
    if (pMode->Scaling == D3DKMDT_VPPS_CENTERED)
    {
        UINT CenterShift = (pMode->DispInfo.Height - pMode->SrcModeHeight) * pMode->DispInfo.Pitch;
        CenterShift += (pMode->DispInfo.Width - pMode->SrcModeWidth) * DstBitPerPixel / 8;
        pDst += (int)CenterShift / 2;
    }
    Status = m_pHWDevice->ExecutePresentDisplayOnly(pDst,
                                                    DstBitPerPixel,
                                                    (BYTE *)pPresentDisplayOnly->pSource,
                                                    pPresentDisplayOnly->BytesPerPixel,
                                                    pPresentDisplayOnly->Pitch,
                                                    pPresentDisplayOnly->NumMoves,
                                                    pPresentDisplayOnly->pMoves,
                                                    pPresentDisplayOnly->NumDirtyRects,
                                                    pPresentDisplayOnly->pDirtyRect,
                                                    RotationNeededByFb,
                                                    pMode);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuDod::QueryInterface(_In_ CONST PQUERY_INTERFACE pQueryInterface)
{
    PAGED_CODE();

    VIOGPU_ASSERT(pQueryInterface != NULL);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s Version = %d\n", __FUNCTION__, pQueryInterface->Version));

    return STATUS_NOT_SUPPORTED;
}

NTSTATUS VioGpuDod::StopDeviceAndReleasePostDisplayOwnership(_In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                             _Out_ DXGK_DISPLAY_INFORMATION *pDisplayInfo)
{
    PAGED_CODE();

    VIOGPU_ASSERT(TargetId < MAX_CHILDREN);

    // FIXME!!!
    if (m_MonitorPowerState > PowerDeviceD0)
    {
        SetPowerState(TargetId, PowerDeviceD0, PowerActionNone);
    }

    UINT scanId = (TargetId < MAX_VIEWS) ? (UINT)TargetId : 0;
    m_pHWDevice->BlackOutScreen(&m_CurrentMode[scanId]);
    DbgPrint(TRACE_LEVEL_FATAL,
             ("StopDeviceAndReleasePostDisplayOwnership Width = %d Height = %d Pitch = %d ColorFormat = %dn",
              m_SystemDisplayInfo.Width,
              m_SystemDisplayInfo.Height,
              m_SystemDisplayInfo.Pitch,
              m_SystemDisplayInfo.ColorFormat));

    *pDisplayInfo = m_SystemDisplayInfo;
    pDisplayInfo->TargetId = TargetId;
    pDisplayInfo->AcpiId = m_CurrentMode[scanId].DispInfo.AcpiId;
    return StopDevice();
}

NTSTATUS VioGpuDod::QueryVidPnHWCapability(_Inout_ DXGKARG_QUERYVIDPNHWCAPABILITY *pVidPnHWCaps)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT(pVidPnHWCaps != NULL);
    VIOGPU_ASSERT(pVidPnHWCaps->SourceId < MAX_VIEWS);
    VIOGPU_ASSERT(pVidPnHWCaps->TargetId < MAX_CHILDREN);

    pVidPnHWCaps->VidPnHWCaps.DriverRotation = 1;
    pVidPnHWCaps->VidPnHWCaps.DriverScaling = 0;
    pVidPnHWCaps->VidPnHWCaps.DriverCloning = 0;
    pVidPnHWCaps->VidPnHWCaps.DriverColorConvert = 1;
    pVidPnHWCaps->VidPnHWCaps.DriverLinkedAdapaterOutput = 0;
    pVidPnHWCaps->VidPnHWCaps.DriverRemoteDisplay = 0;
    pVidPnHWCaps->VidPnHWCaps.Reserved = 0;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuDod::IsSupportedVidPn(_Inout_ DXGKARG_ISSUPPORTEDVIDPN *pIsSupportedVidPn)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT(pIsSupportedVidPn != NULL);

    if (pIsSupportedVidPn->hDesiredVidPn == 0)
    {
        pIsSupportedVidPn->IsVidPnSupported = TRUE;
        return STATUS_SUCCESS;
    }

    pIsSupportedVidPn->IsVidPnSupported = FALSE;

    CONST DXGK_VIDPN_INTERFACE *pVidPnInterface;
    NTSTATUS Status = m_DxgkInterface.DxgkCbQueryVidPnInterface(pIsSupportedVidPn->hDesiredVidPn,
                                                                DXGK_VIDPN_INTERFACE_VERSION_V1,
                                                                &pVidPnInterface);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("DxgkCbQueryVidPnInterface failed with Status = 0x%X, hDesiredVidPn = %llu\n",
                  Status,
                  LONG_PTR(pIsSupportedVidPn->hDesiredVidPn)));
        return Status;
    }

    D3DKMDT_HVIDPNTOPOLOGY hVidPnTopology;
    CONST DXGK_VIDPNTOPOLOGY_INTERFACE *pVidPnTopologyInterface;
    Status = pVidPnInterface->pfnGetTopology(pIsSupportedVidPn->hDesiredVidPn,
                                             &hVidPnTopology,
                                             &pVidPnTopologyInterface);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pfnGetTopology failed with Status = 0x%X, hDesiredVidPn = %llu\n",
                  Status,
                  LONG_PTR(pIsSupportedVidPn->hDesiredVidPn)));
        return Status;
    }

    // Only enumerate the sources Windows actually knows about (the number of
    // active scanouts we reported at StartDevice). Iterating up to the array
    // capacity MAX_VIEWS would query phantom sources that are not in the VidPN
    // topology; dxgkrnl returns an error (not SOURCE_NOT_IN_TOPOLOGY) for those,
    // which would make IsSupportedVidPn fail and the VidPN never commit.
    ULONG numScanouts = m_pHWDevice->GetNumScanouts();
    for (D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId = 0; SourceId < numScanouts; ++SourceId)
    {
        SIZE_T NumPathsFromSource = 0;
        Status = pVidPnTopologyInterface->pfnGetNumPathsFromSource(hVidPnTopology, SourceId, &NumPathsFromSource);
        if (Status == STATUS_GRAPHICS_SOURCE_NOT_IN_TOPOLOGY)
        {
            continue;
        }
        else if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnGetNumPathsFromSource failed with Status = 0x%X hVidPnTopology = %llu, SourceId = %llu",
                      Status,
                      LONG_PTR(hVidPnTopology),
                      LONG_PTR(SourceId)));
            return Status;
        }
        else if (NumPathsFromSource > MAX_CHILDREN)
        {
            return STATUS_SUCCESS;
        }

        // Check if resolution exceeds framebuffer segment capacity
        if (NumPathsFromSource == 0)
        {
            continue;
        }

        D3DKMDT_HVIDPNSOURCEMODESET hVidPnSourceModeSet;
        CONST DXGK_VIDPNSOURCEMODESET_INTERFACE *pVidPnSourceModeSetInterface;
        Status = pVidPnInterface->pfnAcquireSourceModeSet(pIsSupportedVidPn->hDesiredVidPn,
                                                          SourceId,
                                                          &hVidPnSourceModeSet,
                                                          &pVidPnSourceModeSetInterface);
        if (!NT_SUCCESS(Status))
        {
            if (Status == STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE)
            {
                continue;
            }
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("<--- %s pfnAcquireSourceModeSet failed with Status = 0x%X, SourceId = %llu\n",
                      __FUNCTION__,
                      Status,
                      LONG_PTR(SourceId)));
            return Status;
        }

        CONST D3DKMDT_VIDPN_SOURCE_MODE *pPinnedVidPnSourceModeInfo = NULL;
        Status = pVidPnSourceModeSetInterface->pfnAcquirePinnedModeInfo(hVidPnSourceModeSet,
                                                                        &pPinnedVidPnSourceModeInfo);
        if (!NT_SUCCESS(Status))
        {
            pVidPnInterface->pfnReleaseSourceModeSet(pIsSupportedVidPn->hDesiredVidPn, hVidPnSourceModeSet);
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("<--- %s pfnAcquirePinnedModeInfo failed with Status = 0x%X\n", __FUNCTION__, Status));
            return Status;
        }

        BOOLEAN bReject = FALSE;
        if (pPinnedVidPnSourceModeInfo != NULL)
        {
            SIZE_T RequiredSize = (SIZE_T)pPinnedVidPnSourceModeInfo->Format.Graphics.PrimSurfSize.cx *
                                  pPinnedVidPnSourceModeInfo->Format.Graphics.PrimSurfSize.cy *
                                  (BPPFromPixelFormat(pPinnedVidPnSourceModeInfo->Format.Graphics.PixelFormat) /
                                   BITS_PER_BYTE);
            SIZE_T SegmentSize = m_pHWDevice->GetFrameSegmentSize();
            if (SegmentSize > 0 && RequiredSize > SegmentSize)
            {
                DbgPrint(TRACE_LEVEL_WARNING,
                         ("<--- %s Resolution requires %llu bytes, segment size is %llu\n",
                          __FUNCTION__,
                          (ULONGLONG)RequiredSize,
                          (ULONGLONG)SegmentSize));
                bReject = TRUE;
            }
            pVidPnSourceModeSetInterface->pfnReleaseModeInfo(hVidPnSourceModeSet, pPinnedVidPnSourceModeInfo);
        }
        pVidPnInterface->pfnReleaseSourceModeSet(pIsSupportedVidPn->hDesiredVidPn, hVidPnSourceModeSet);

        if (bReject)
        {
            return STATUS_NO_MEMORY;
        }
    }

    pIsSupportedVidPn->IsVidPnSupported = TRUE;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS
VioGpuDod::RecommendFunctionalVidPn(_In_ CONST DXGKARG_RECOMMENDFUNCTIONALVIDPN *CONST pRecommendFunctionalVidPn)
{
    PAGED_CODE();


    VIOGPU_ASSERT(pRecommendFunctionalVidPn == NULL);

    return STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;
}

NTSTATUS VioGpuDod::RecommendVidPnTopology(_In_ CONST DXGKARG_RECOMMENDVIDPNTOPOLOGY *CONST pRecommendVidPnTopology)
{
    PAGED_CODE();


    VIOGPU_ASSERT(pRecommendVidPnTopology == NULL);

    return STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;
}

NTSTATUS VioGpuDod::RecommendMonitorModes(_In_ CONST DXGKARG_RECOMMENDMONITORMODES *CONST pRecommendMonitorModes)
{
    PAGED_CODE();


    return AddSingleMonitorMode(pRecommendMonitorModes);
}

NTSTATUS VioGpuDod::AddSingleSourceMode(_In_ CONST DXGK_VIDPNSOURCEMODESET_INTERFACE *pVidPnSourceModeSetInterface,
                                        D3DKMDT_HVIDPNSOURCEMODESET hVidPnSourceModeSet,
                                        D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    UNREFERENCED_PARAMETER(SourceId);

    for (ULONG idx = 0; idx < m_pHWDevice->GetModeCount(); ++idx)
    {
        D3DKMDT_VIDPN_SOURCE_MODE *pVidPnSourceModeInfo = NULL;
        PVIDEO_MODE_INFORMATION pModeInfo = m_pHWDevice->GetModeInfo(idx);
        NTSTATUS Status = pVidPnSourceModeSetInterface->pfnCreateNewModeInfo(hVidPnSourceModeSet,
                                                                             &pVidPnSourceModeInfo);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnCreateNewModeInfo failed with Status = 0x%X, hVidPnSourceModeSet = %llu",
                      Status,
                      LONG_PTR(hVidPnSourceModeSet)));
            return Status;
        }

        pVidPnSourceModeInfo->Type = D3DKMDT_RMT_GRAPHICS;
        pVidPnSourceModeInfo->Format.Graphics.PrimSurfSize.cx = pModeInfo->VisScreenWidth;
        pVidPnSourceModeInfo->Format.Graphics.PrimSurfSize.cy = pModeInfo->VisScreenHeight;
        pVidPnSourceModeInfo->Format.Graphics.VisibleRegionSize = pVidPnSourceModeInfo->Format.Graphics.PrimSurfSize;
        pVidPnSourceModeInfo->Format.Graphics.Stride = pModeInfo->ScreenStride;
        pVidPnSourceModeInfo->Format.Graphics.PixelFormat = D3DDDIFMT_A8R8G8B8;
        pVidPnSourceModeInfo->Format.Graphics.ColorBasis = D3DKMDT_CB_SCRGB;
        pVidPnSourceModeInfo->Format.Graphics.PixelValueAccessMode = D3DKMDT_PVAM_DIRECT;

        Status = pVidPnSourceModeSetInterface->pfnAddMode(hVidPnSourceModeSet, pVidPnSourceModeInfo);
        if (!NT_SUCCESS(Status))
        {
            NTSTATUS TempStatus = pVidPnSourceModeSetInterface->pfnReleaseModeInfo(hVidPnSourceModeSet,
                                                                                   pVidPnSourceModeInfo);
            UNREFERENCED_PARAMETER(TempStatus);
            NT_ASSERT(NT_SUCCESS(TempStatus));

            if (Status != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnAddMode failed with Status = 0x%X, hVidPnSourceModeSet = %llu, pVidPnSourceModeInfo = %p",
                          Status,
                          LONG_PTR(hVidPnSourceModeSet),
                          pVidPnSourceModeInfo));
                return Status;
            }
        }
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

VOID VioGpuDod::BuildVideoSignalInfo(D3DKMDT_VIDEO_SIGNAL_INFO *pVideoSignalInfo, PVIDEO_MODE_INFORMATION pModeInfo)
{
    PAGED_CODE();

    pVideoSignalInfo->VideoStandard = D3DKMDT_VSS_OTHER;
    pVideoSignalInfo->TotalSize.cx = pModeInfo->VisScreenWidth;
    pVideoSignalInfo->TotalSize.cy = pModeInfo->VisScreenHeight;

    pVideoSignalInfo->VSyncFreq.Numerator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pVideoSignalInfo->VSyncFreq.Denominator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pVideoSignalInfo->HSyncFreq.Numerator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pVideoSignalInfo->HSyncFreq.Denominator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pVideoSignalInfo->PixelRate = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pVideoSignalInfo->ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;
}

NTSTATUS VioGpuDod::AddSingleTargetMode(_In_ CONST DXGK_VIDPNTARGETMODESET_INTERFACE *pVidPnTargetModeSetInterface,
                                        D3DKMDT_HVIDPNTARGETMODESET hVidPnTargetModeSet,
                                        _In_opt_ CONST D3DKMDT_VIDPN_SOURCE_MODE *pVidPnPinnedSourceModeInfo,
                                        D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    UNREFERENCED_PARAMETER(pVidPnPinnedSourceModeInfo);

    D3DKMDT_VIDPN_TARGET_MODE *pVidPnTargetModeInfo = NULL;
    NTSTATUS Status = STATUS_SUCCESS;

    for (UINT ModeIndex = 0; ModeIndex < m_pHWDevice->GetModeCount(); ++ModeIndex)
    {
        PVIDEO_MODE_INFORMATION pModeInfo = m_pHWDevice->GetModeInfo(SourceId);
        pVidPnTargetModeInfo = NULL;
        Status = pVidPnTargetModeSetInterface->pfnCreateNewModeInfo(hVidPnTargetModeSet, &pVidPnTargetModeInfo);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnCreateNewModeInfo failed with Status = 0x%X, hVidPnTargetModeSet = %llu",
                      Status,
                      LONG_PTR(hVidPnTargetModeSet)));
            return Status;
        }
        pVidPnTargetModeInfo->VideoSignalInfo.ActiveSize = pVidPnTargetModeInfo->VideoSignalInfo.TotalSize;
        BuildVideoSignalInfo(&pVidPnTargetModeInfo->VideoSignalInfo, pModeInfo);

        if (pModeInfo->VisScreenWidth == NOM_WIDTH_SIZE && pModeInfo->VisScreenHeight == NOM_HEIGHT_SIZE)
        {
            pVidPnTargetModeInfo->Preference = D3DKMDT_MP_PREFERRED;
        }
        else
        {
            pVidPnTargetModeInfo->Preference = D3DKMDT_MP_NOTPREFERRED;
        }

        Status = pVidPnTargetModeSetInterface->pfnAddMode(hVidPnTargetModeSet, pVidPnTargetModeInfo);
        if (!NT_SUCCESS(Status))
        {
            if (Status != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnAddMode failed with Status = 0x%X, hVidPnTargetModeSet = 0x%llu, pVidPnTargetModeInfo = "
                          "%p\n",
                          Status,
                          LONG_PTR(hVidPnTargetModeSet),
                          pVidPnTargetModeInfo));
            }

            Status = pVidPnTargetModeSetInterface->pfnReleaseModeInfo(hVidPnTargetModeSet, pVidPnTargetModeInfo);
            NT_ASSERT(NT_SUCCESS(Status));
        }
    }
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuDod::AddSingleMonitorMode(_In_ CONST DXGKARG_RECOMMENDMONITORMODES *CONST pRecommendMonitorModes)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    D3DKMDT_MONITOR_SOURCE_MODE *pMonitorSourceMode = NULL;
    PVIDEO_MODE_INFORMATION pVbeModeInfo = NULL;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    Status = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnCreateNewModeInfo(pRecommendMonitorModes->hMonitorSourceModeSet,
                                                                                          &pMonitorSourceMode);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pfnCreateNewModeInfo failed with Status = 0x%X, hMonitorSourceModeSet = 0x%llu\n",
                  Status,
                  LONG_PTR(pRecommendMonitorModes->hMonitorSourceModeSet)));
        return Status;
    }

    UINT monScanId = (pRecommendMonitorModes->VideoPresentTargetId < MAX_SCANOUTS)
                         ? (UINT)pRecommendMonitorModes->VideoPresentTargetId
                         : 0;
    pVbeModeInfo = m_pHWDevice->GetModeInfo(m_pHWDevice->GetCurrentModeIndex(monScanId));

    BuildVideoSignalInfo(&pMonitorSourceMode->VideoSignalInfo, pVbeModeInfo);

    pMonitorSourceMode->Origin = D3DKMDT_MCO_DRIVER;
    pMonitorSourceMode->Preference = D3DKMDT_MP_PREFERRED;
    pMonitorSourceMode->ColorBasis = D3DKMDT_CB_SRGB;
    pMonitorSourceMode->ColorCoeffDynamicRanges.FirstChannel = 8;
    pMonitorSourceMode->ColorCoeffDynamicRanges.SecondChannel = 8;
    pMonitorSourceMode->ColorCoeffDynamicRanges.ThirdChannel = 8;
    pMonitorSourceMode->ColorCoeffDynamicRanges.FourthChannel = 8;

    Status = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnAddMode(pRecommendMonitorModes->hMonitorSourceModeSet,
                                                                                pMonitorSourceMode);
    if (!NT_SUCCESS(Status))
    {
        if (Status != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnAddMode failed with Status = 0x%X, hMonitorSourceModeSet = 0x%llu, pMonitorSourceMode = "
                      "0x%p\n",
                      Status,
                      LONG_PTR(pRecommendMonitorModes->hMonitorSourceModeSet),
                      pMonitorSourceMode));
        }
        else
        {
            Status = STATUS_SUCCESS;
        }

        NTSTATUS TempStatus = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnReleaseModeInfo(pRecommendMonitorModes->hMonitorSourceModeSet,
                                                                                                         pMonitorSourceMode);
        UNREFERENCED_PARAMETER(TempStatus);
        NT_ASSERT(NT_SUCCESS(TempStatus));
        return Status;
    }

    for (UINT Idx = 0; Idx < m_pHWDevice->GetModeCount(); ++Idx)
    {
        pVbeModeInfo = m_pHWDevice->GetModeInfo(Idx);

        Status = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnCreateNewModeInfo(pRecommendMonitorModes->hMonitorSourceModeSet,
                                                                                              &pMonitorSourceMode);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnCreateNewModeInfo failed with Status = 0x%X, hMonitorSourceModeSet = 0x%llu\n",
                      Status,
                      LONG_PTR(pRecommendMonitorModes->hMonitorSourceModeSet)));
            return Status;
        }

        DbgPrint(TRACE_LEVEL_INFORMATION,
                 ("%s: add pref mode, dimensions %ux%u, taken from DxgkCbAcquirePostDisplayOwnership at StartDevice\n",
                  __FUNCTION__,
                  pVbeModeInfo->VisScreenWidth,
                  pVbeModeInfo->VisScreenHeight));

        BuildVideoSignalInfo(&pMonitorSourceMode->VideoSignalInfo, pVbeModeInfo);

        pMonitorSourceMode->Origin = D3DKMDT_MCO_DRIVER;
        pMonitorSourceMode->ColorBasis = D3DKMDT_CB_SRGB;
        pMonitorSourceMode->ColorCoeffDynamicRanges.FirstChannel = 8;
        pMonitorSourceMode->ColorCoeffDynamicRanges.SecondChannel = 8;
        pMonitorSourceMode->ColorCoeffDynamicRanges.ThirdChannel = 8;
        pMonitorSourceMode->ColorCoeffDynamicRanges.FourthChannel = 8;
        if (pVbeModeInfo->VisScreenWidth == NOM_WIDTH_SIZE && pVbeModeInfo->VisScreenHeight == NOM_HEIGHT_SIZE)
        {
            pMonitorSourceMode->Preference = D3DKMDT_MP_PREFERRED;
        }
        else
        {
            pMonitorSourceMode->Preference = D3DKMDT_MP_NOTPREFERRED;
        }

        Status = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnAddMode(pRecommendMonitorModes->hMonitorSourceModeSet,
                                                                                    pMonitorSourceMode);
        if (!NT_SUCCESS(Status))
        {
            if (Status != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnAddMode failed with Status = 0x%X, hMonitorSourceModeSet = 0x%llu, pMonitorSourceMode = "
                          "0x%p\n",
                          Status,
                          LONG_PTR(pRecommendMonitorModes->hMonitorSourceModeSet),
                          pMonitorSourceMode));
            }

            Status = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnReleaseModeInfo(pRecommendMonitorModes->hMonitorSourceModeSet,
                                                                                                pMonitorSourceMode);
            NT_ASSERT(NT_SUCCESS(Status));
        }
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuDod::EnumVidPnCofuncModality(_In_ CONST DXGKARG_ENUMVIDPNCOFUNCMODALITY *CONST pEnumCofuncModality)
{
    PAGED_CODE();

    VIOGPU_ASSERT(pEnumCofuncModality != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    D3DKMDT_HVIDPNTOPOLOGY hVidPnTopology = 0;
    D3DKMDT_HVIDPNSOURCEMODESET hVidPnSourceModeSet = 0;
    D3DKMDT_HVIDPNTARGETMODESET hVidPnTargetModeSet = 0;
    CONST DXGK_VIDPN_INTERFACE *pVidPnInterface = NULL;
    CONST DXGK_VIDPNTOPOLOGY_INTERFACE *pVidPnTopologyInterface = NULL;
    CONST DXGK_VIDPNSOURCEMODESET_INTERFACE *pVidPnSourceModeSetInterface = NULL;
    CONST DXGK_VIDPNTARGETMODESET_INTERFACE *pVidPnTargetModeSetInterface = NULL;
    CONST D3DKMDT_VIDPN_PRESENT_PATH *pVidPnPresentPath = NULL;
    CONST D3DKMDT_VIDPN_PRESENT_PATH *pVidPnPresentPathTemp = NULL;
    CONST D3DKMDT_VIDPN_SOURCE_MODE *pVidPnPinnedSourceModeInfo = NULL;
    CONST D3DKMDT_VIDPN_TARGET_MODE *pVidPnPinnedTargetModeInfo = NULL;

    NTSTATUS Status = m_DxgkInterface.DxgkCbQueryVidPnInterface(pEnumCofuncModality->hConstrainingVidPn,
                                                                DXGK_VIDPN_INTERFACE_VERSION_V1,
                                                                &pVidPnInterface);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("DxgkCbQueryVidPnInterface failed with Status = 0x%X, hFunctionalVidPn = 0x%llu\n",
                  Status,
                  LONG_PTR(pEnumCofuncModality->hConstrainingVidPn)));
        return Status;
    }

    Status = pVidPnInterface->pfnGetTopology(pEnumCofuncModality->hConstrainingVidPn,
                                             &hVidPnTopology,
                                             &pVidPnTopologyInterface);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pfnGetTopology failed with Status = 0x%X, hFunctionalVidPn = 0x%llu\n",
                  Status,
                  LONG_PTR(pEnumCofuncModality->hConstrainingVidPn)));
        return Status;
    }

    Status = pVidPnTopologyInterface->pfnAcquireFirstPathInfo(hVidPnTopology, &pVidPnPresentPath);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pfnAcquireFirstPathInfo failed with Status = 0x%X, hVidPnTopology = 0x%llu\n",
                  Status,
                  LONG_PTR(hVidPnTopology)));
        return Status;
    }

    while (Status != STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET)
    {
        Status = pVidPnInterface->pfnAcquireSourceModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                          pVidPnPresentPath->VidPnSourceId,
                                                          &hVidPnSourceModeSet,
                                                          &pVidPnSourceModeSetInterface);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnAcquireSourceModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, SourceId = "
                      "0x%llu\n",
                      Status,
                      LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                      LONG_PTR(pVidPnPresentPath->VidPnSourceId)));
            break;
        }

        Status = pVidPnSourceModeSetInterface->pfnAcquirePinnedModeInfo(hVidPnSourceModeSet,
                                                                        &pVidPnPinnedSourceModeInfo);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnAcquirePinnedModeInfo failed with Status = 0x%X, hVidPnSourceModeSet = 0x%llu\n",
                      Status,
                      LONG_PTR(hVidPnSourceModeSet)));
            break;
        }

        if (!((pEnumCofuncModality->EnumPivotType == D3DKMDT_EPT_VIDPNSOURCE) &&
              (pEnumCofuncModality->EnumPivot.VidPnSourceId == pVidPnPresentPath->VidPnSourceId)))
        {
            if (pVidPnPinnedSourceModeInfo == NULL)
            {
                Status = pVidPnInterface->pfnReleaseSourceModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                  hVidPnSourceModeSet);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnReleaseSourceModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, "
                              "hVidPnSourceModeSet = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                              LONG_PTR(hVidPnSourceModeSet)));
                    break;
                }
                hVidPnSourceModeSet = 0;

                Status = pVidPnInterface->pfnCreateNewSourceModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                    pVidPnPresentPath->VidPnSourceId,
                                                                    &hVidPnSourceModeSet,
                                                                    &pVidPnSourceModeSetInterface);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnCreateNewSourceModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, "
                              "SourceId = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                              LONG_PTR(pVidPnPresentPath->VidPnSourceId)));
                    break;
                }

                {
                    Status = AddSingleSourceMode(pVidPnSourceModeSetInterface,
                                                 hVidPnSourceModeSet,
                                                 pVidPnPresentPath->VidPnSourceId);
                }

                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("AddSingleSourceMode failed with Status = 0x%X, hFunctionalVidPn = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn)));
                    break;
                }

                Status = pVidPnInterface->pfnAssignSourceModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                 pVidPnPresentPath->VidPnSourceId,
                                                                 hVidPnSourceModeSet);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnAssignSourceModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, SourceId "
                              "= 0x%llu, hVidPnSourceModeSet = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                              LONG_PTR(pVidPnPresentPath->VidPnSourceId),
                              LONG_PTR(hVidPnSourceModeSet)));
                    break;
                }
                hVidPnSourceModeSet = 0;
            }
        }

        if (!((pEnumCofuncModality->EnumPivotType == D3DKMDT_EPT_VIDPNTARGET) &&
              (pEnumCofuncModality->EnumPivot.VidPnTargetId == pVidPnPresentPath->VidPnTargetId)))
        {
            Status = pVidPnInterface->pfnAcquireTargetModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                              pVidPnPresentPath->VidPnTargetId,
                                                              &hVidPnTargetModeSet,
                                                              &pVidPnTargetModeSetInterface);
            if (!NT_SUCCESS(Status))
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnAcquireTargetModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, TargetId = "
                          "0x%llu\n",
                          Status,
                          LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                          LONG_PTR(pVidPnPresentPath->VidPnTargetId)));
                break;
            }

            Status = pVidPnTargetModeSetInterface->pfnAcquirePinnedModeInfo(hVidPnTargetModeSet,
                                                                            &pVidPnPinnedTargetModeInfo);
            if (!NT_SUCCESS(Status))
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnAcquirePinnedModeInfo failed with Status = 0x%X, hVidPnTargetModeSet = 0x%llu\n",
                          Status,
                          LONG_PTR(hVidPnTargetModeSet)));
                break;
            }

            if (pVidPnPinnedTargetModeInfo == NULL)
            {
                Status = pVidPnInterface->pfnReleaseTargetModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                  hVidPnTargetModeSet);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnReleaseTargetModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, "
                              "hVidPnTargetModeSet = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                              LONG_PTR(hVidPnTargetModeSet)));
                    break;
                }
                hVidPnTargetModeSet = 0;

                Status = pVidPnInterface->pfnCreateNewTargetModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                    pVidPnPresentPath->VidPnTargetId,
                                                                    &hVidPnTargetModeSet,
                                                                    &pVidPnTargetModeSetInterface);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnCreateNewTargetModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, "
                              "TargetId = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                              LONG_PTR(pVidPnPresentPath->VidPnTargetId)));
                    break;
                }

                Status = AddSingleTargetMode(pVidPnTargetModeSetInterface,
                                             hVidPnTargetModeSet,
                                             pVidPnPinnedSourceModeInfo,
                                             pVidPnPresentPath->VidPnSourceId);

                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("AddSingleTargetMode failed with Status = 0x%X, hFunctionalVidPn = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn)));
                    break;
                }

                Status = pVidPnInterface->pfnAssignTargetModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                 pVidPnPresentPath->VidPnTargetId,
                                                                 hVidPnTargetModeSet);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnAssignTargetModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, TargetId "
                              "= 0x%llu, hVidPnTargetModeSet = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                              LONG_PTR(pVidPnPresentPath->VidPnTargetId),
                              LONG_PTR(hVidPnTargetModeSet)));
                    break;
                }
                hVidPnTargetModeSet = 0;
            }
            else
            {
                Status = pVidPnTargetModeSetInterface->pfnReleaseModeInfo(hVidPnTargetModeSet,
                                                                          pVidPnPinnedTargetModeInfo);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnReleaseModeInfo failed with Status = 0x%X, hVidPnTargetModeSet = 0x%llu, "
                              "pVidPnPinnedTargetModeInfo = %p\n",
                              Status,
                              LONG_PTR(hVidPnTargetModeSet),
                              pVidPnPinnedTargetModeInfo));
                    break;
                }
                pVidPnPinnedTargetModeInfo = NULL;

                Status = pVidPnInterface->pfnReleaseTargetModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                  hVidPnTargetModeSet);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnReleaseTargetModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, "
                              "hVidPnTargetModeSet = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                              LONG_PTR(hVidPnTargetModeSet)));
                    break;
                }
                hVidPnTargetModeSet = 0;
            }
        }

        if (pVidPnPinnedSourceModeInfo != NULL)
        {
            Status = pVidPnSourceModeSetInterface->pfnReleaseModeInfo(hVidPnSourceModeSet, pVidPnPinnedSourceModeInfo);
            if (!NT_SUCCESS(Status))
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnReleaseModeInfo failed with Status = 0x%X, hVidPnSourceModeSet = 0x%llu, "
                          "pVidPnPinnedSourceModeInfo = %p\n",
                          Status,
                          LONG_PTR(hVidPnSourceModeSet),
                          pVidPnPinnedSourceModeInfo));
                break;
            }
            pVidPnPinnedSourceModeInfo = NULL;
        }

        if (hVidPnSourceModeSet != 0)
        {
            Status = pVidPnInterface->pfnReleaseSourceModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                              hVidPnSourceModeSet);
            if (!NT_SUCCESS(Status))
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnReleaseSourceModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, "
                          "hVidPnSourceModeSet = 0x%llu\n",
                          Status,
                          LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                          LONG_PTR(hVidPnSourceModeSet)));
                break;
            }
            hVidPnSourceModeSet = 0;
        }

        D3DKMDT_VIDPN_PRESENT_PATH LocalVidPnPresentPath = *pVidPnPresentPath;
        BOOLEAN SupportFieldsModified = FALSE;

        if (!((pEnumCofuncModality->EnumPivotType == D3DKMDT_EPT_SCALING) &&
              (pEnumCofuncModality->EnumPivot.VidPnSourceId == pVidPnPresentPath->VidPnSourceId) &&
              (pEnumCofuncModality->EnumPivot.VidPnTargetId == pVidPnPresentPath->VidPnTargetId)))
        {
            if (pVidPnPresentPath->ContentTransformation.Scaling == D3DKMDT_VPPS_UNPINNED)
            {
                RtlZeroMemory(&(LocalVidPnPresentPath.ContentTransformation.ScalingSupport),
                              sizeof(D3DKMDT_VIDPN_PRESENT_PATH_SCALING_SUPPORT));
                LocalVidPnPresentPath.ContentTransformation.ScalingSupport.Identity = 1;
                LocalVidPnPresentPath.ContentTransformation.ScalingSupport.Centered = 1;
                SupportFieldsModified = TRUE;
            }
        }

        if (!((pEnumCofuncModality->EnumPivotType != D3DKMDT_EPT_ROTATION) &&
              (pEnumCofuncModality->EnumPivot.VidPnSourceId == pVidPnPresentPath->VidPnSourceId) &&
              (pEnumCofuncModality->EnumPivot.VidPnTargetId == pVidPnPresentPath->VidPnTargetId)))
        {
            if (pVidPnPresentPath->ContentTransformation.Rotation == D3DKMDT_VPPR_UNPINNED)
            {
                LocalVidPnPresentPath.ContentTransformation.RotationSupport.Identity = 1;
                LocalVidPnPresentPath.ContentTransformation.RotationSupport.Rotate90 = 1;
                LocalVidPnPresentPath.ContentTransformation.RotationSupport.Rotate180 = 0;
                LocalVidPnPresentPath.ContentTransformation.RotationSupport.Rotate270 = 0;
                SupportFieldsModified = TRUE;
            }
        }

        if (SupportFieldsModified)
        {
            Status = pVidPnTopologyInterface->pfnUpdatePathSupportInfo(hVidPnTopology, &LocalVidPnPresentPath);
            if (!NT_SUCCESS(Status))
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnUpdatePathSupportInfo failed with Status = 0x%X, hVidPnTopology = 0x%llu\n",
                          Status,
                          LONG_PTR(hVidPnTopology)));
                break;
            }
        }

        pVidPnPresentPathTemp = pVidPnPresentPath;
        Status = pVidPnTopologyInterface->pfnAcquireNextPathInfo(hVidPnTopology,
                                                                 pVidPnPresentPathTemp,
                                                                 &pVidPnPresentPath);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnAcquireNextPathInfo failed with Status = 0x%X, hVidPnTopology = 0x%llu, "
                      "pVidPnPresentPathTemp = %p\n",
                      Status,
                      LONG_PTR(hVidPnTopology),
                      pVidPnPresentPathTemp));
            break;
        }

        NTSTATUS TempStatus = pVidPnTopologyInterface->pfnReleasePathInfo(hVidPnTopology, pVidPnPresentPathTemp);
        if (!NT_SUCCESS(TempStatus))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnReleasePathInfo failed with Status = 0x%X, hVidPnTopology = 0x%llu, pVidPnPresentPathTemp = "
                      "%p\n",
                      TempStatus,
                      LONG_PTR(hVidPnTopology),
                      pVidPnPresentPathTemp));
            Status = TempStatus;
            break;
        }
        pVidPnPresentPathTemp = NULL;
    }

    if (Status == STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET)
    {
        Status = STATUS_SUCCESS;
    }

    NTSTATUS TempStatus = STATUS_NOT_FOUND;

    if ((pVidPnSourceModeSetInterface != NULL) && (pVidPnPinnedSourceModeInfo != NULL))
    {
        TempStatus = pVidPnSourceModeSetInterface->pfnReleaseModeInfo(hVidPnSourceModeSet, pVidPnPinnedSourceModeInfo);
        VIOGPU_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    if ((pVidPnTargetModeSetInterface != NULL) && (pVidPnPinnedTargetModeInfo != NULL))
    {
        TempStatus = pVidPnTargetModeSetInterface->pfnReleaseModeInfo(hVidPnTargetModeSet, pVidPnPinnedTargetModeInfo);
        VIOGPU_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    if (pVidPnPresentPath != NULL)
    {
        TempStatus = pVidPnTopologyInterface->pfnReleasePathInfo(hVidPnTopology, pVidPnPresentPath);
        VIOGPU_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    if (pVidPnPresentPathTemp != NULL)
    {
        TempStatus = pVidPnTopologyInterface->pfnReleasePathInfo(hVidPnTopology, pVidPnPresentPathTemp);
        VIOGPU_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    if (hVidPnSourceModeSet != 0)
    {
        TempStatus = pVidPnInterface->pfnReleaseSourceModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                              hVidPnSourceModeSet);
        VIOGPU_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    if (hVidPnTargetModeSet != 0)
    {
        TempStatus = pVidPnInterface->pfnReleaseTargetModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                              hVidPnTargetModeSet);
        VIOGPU_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    VIOGPU_ASSERT_CHK(TempStatus == STATUS_NOT_FOUND || Status != STATUS_SUCCESS);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuDod::SetVidPnSourceVisibility(_In_ CONST DXGKARG_SETVIDPNSOURCEVISIBILITY *pSetVidPnSourceVisibility)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT(pSetVidPnSourceVisibility != NULL);
    VIOGPU_ASSERT((pSetVidPnSourceVisibility->VidPnSourceId < MAX_VIEWS) ||
                  (pSetVidPnSourceVisibility->VidPnSourceId == D3DDDI_ID_ALL));

    ULONG numScanouts = m_pHWDevice->GetNumScanouts();
    UINT first = 0, last = 0;
    if (pSetVidPnSourceVisibility->VidPnSourceId == D3DDDI_ID_ALL)
    {
        first = 0;
        last = numScanouts - 1;
    }
    else
    {
        if (pSetVidPnSourceVisibility->VidPnSourceId >= numScanouts)
        {
            return STATUS_SUCCESS;
        }
        first = last = pSetVidPnSourceVisibility->VidPnSourceId;
    }

    for (UINT src = first; src <= last; src++)
    {
        if (pSetVidPnSourceVisibility->Visible)
        {
            m_CurrentMode[src].Flags.FullscreenPresent = TRUE;
        }
        else
        {
            m_pHWDevice->BlackOutScreen(&m_CurrentMode[src]);
        }
        m_CurrentMode[src].Flags.SourceNotVisible = !(pSetVidPnSourceVisibility->Visible);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return STATUS_SUCCESS;
}

NTSTATUS VioGpuDod::CommitVidPn(_In_ CONST DXGKARG_COMMITVIDPN *CONST pCommitVidPn)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT(pCommitVidPn != NULL);
    VIOGPU_ASSERT(pCommitVidPn->AffectedVidPnSourceId < MAX_VIEWS);

    NTSTATUS Status;
    SIZE_T NumPaths = 0;
    D3DKMDT_HVIDPNTOPOLOGY hVidPnTopology = 0;
    D3DKMDT_HVIDPNSOURCEMODESET hVidPnSourceModeSet = 0;
    CONST DXGK_VIDPN_INTERFACE *pVidPnInterface = NULL;
    CONST DXGK_VIDPNTOPOLOGY_INTERFACE *pVidPnTopologyInterface = NULL;
    CONST DXGK_VIDPNSOURCEMODESET_INTERFACE *pVidPnSourceModeSetInterface = NULL;
    CONST D3DKMDT_VIDPN_PRESENT_PATH *pVidPnPresentPath = NULL;
    CONST D3DKMDT_VIDPN_SOURCE_MODE *pPinnedVidPnSourceModeInfo = NULL;

    if (pCommitVidPn->Flags.PathPoweredOff)
    {
        Status = STATUS_SUCCESS;
        goto CommitVidPnExit;
    }

    Status = m_DxgkInterface.DxgkCbQueryVidPnInterface(pCommitVidPn->hFunctionalVidPn,
                                                       DXGK_VIDPN_INTERFACE_VERSION_V1,
                                                       &pVidPnInterface);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("DxgkCbQueryVidPnInterface failed with Status = 0x%X, hFunctionalVidPn = 0x%llu\n",
                  Status,
                  LONG_PTR(pCommitVidPn->hFunctionalVidPn)));
        goto CommitVidPnExit;
    }

    Status = pVidPnInterface->pfnGetTopology(pCommitVidPn->hFunctionalVidPn, &hVidPnTopology, &pVidPnTopologyInterface);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pfnGetTopology failed with Status = 0x%X, hFunctionalVidPn = 0x%llu\n",
                  Status,
                  LONG_PTR(pCommitVidPn->hFunctionalVidPn)));
        goto CommitVidPnExit;
    }

    Status = pVidPnTopologyInterface->pfnGetNumPaths(hVidPnTopology, &NumPaths);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pfnGetNumPaths failed with Status = 0x%X, hVidPnTopology = 0x%llu\n",
                  Status,
                  LONG_PTR(hVidPnTopology)));
        goto CommitVidPnExit;
    }


    // Windows has composed a real topology (the boot commit is single). Trigger the ONE-SHOT post-start scan so a
    // secondary the host enabled at boot -- kept at the HWInit seed (disconnected) until now -- is indicated as a
    // hotplug ARRIVAL, which makes Windows re-compose and EXTEND. This replaces a magic timer: it is tied to the
    // real "Windows finished its boot topology" event, like the Linux driver drives reconfiguration from the
    // display event. The trigger only SIGNALS the worker; the actual GetDisplayInfo / DxgkCbIndicateChildStatus
    // runs there (PASSIVE), never re-entrantly from inside this DDI. One-shot guard lives in TriggerInitialScan.
    if (NumPaths != 0 && m_pHWDevice)
    {
        m_pHWDevice->TriggerInitialScan();
    }

    if (NumPaths != 0)
    {
        Status = pVidPnInterface->pfnAcquireSourceModeSet(pCommitVidPn->hFunctionalVidPn,
                                                          pCommitVidPn->AffectedVidPnSourceId,
                                                          &hVidPnSourceModeSet,
                                                          &pVidPnSourceModeSetInterface);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnAcquireSourceModeSet failed with Status = 0x%X, hFunctionalVidPn = 0x%llu, SourceId = "
                      "0x%I64x\n",
                      Status,
                      LONG_PTR(pCommitVidPn->hFunctionalVidPn),
                      pCommitVidPn->AffectedVidPnSourceId));
            goto CommitVidPnExit;
        }

        Status = pVidPnSourceModeSetInterface->pfnAcquirePinnedModeInfo(hVidPnSourceModeSet,
                                                                        &pPinnedVidPnSourceModeInfo);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnAcquirePinnedModeInfo failed with Status = 0x%X, hFunctionalVidPn = 0x%llu\n",
                      Status,
                      LONG_PTR(pCommitVidPn->hFunctionalVidPn)));
            goto CommitVidPnExit;
        }
    }
    else
    {
        pPinnedVidPnSourceModeInfo = NULL;
    }

    if (pPinnedVidPnSourceModeInfo == NULL)
    {
        Status = STATUS_SUCCESS;
        goto CommitVidPnExit;
    }

    Status = IsVidPnSourceModeFieldsValid(pPinnedVidPnSourceModeInfo);
    if (!NT_SUCCESS(Status))
    {
        goto CommitVidPnExit;
    }

    SIZE_T NumPathsFromSource = 0;
    Status = pVidPnTopologyInterface->pfnGetNumPathsFromSource(hVidPnTopology,
                                                               pCommitVidPn->AffectedVidPnSourceId,
                                                               &NumPathsFromSource);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pfnGetNumPathsFromSource failed with Status = 0x%X, hVidPnTopology = 0x%llu\n",
                  Status,
                  LONG_PTR(hVidPnTopology)));
        goto CommitVidPnExit;
    }

    for (SIZE_T PathIndex = 0; PathIndex < NumPathsFromSource; ++PathIndex)
    {
        D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId = D3DDDI_ID_UNINITIALIZED;
        Status = pVidPnTopologyInterface->pfnEnumPathTargetsFromSource(hVidPnTopology,
                                                                       pCommitVidPn->AffectedVidPnSourceId,
                                                                       PathIndex,
                                                                       &TargetId);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnEnumPathTargetsFromSource failed with Status = 0x%X, hVidPnTopology = 0x%llu, SourceId = "
                      "0x%I64x, PathIndex = 0x%I64x\n",
                      Status,
                      LONG_PTR(hVidPnTopology),
                      pCommitVidPn->AffectedVidPnSourceId,
                      PathIndex));
            goto CommitVidPnExit;
        }

        Status = pVidPnTopologyInterface->pfnAcquirePathInfo(hVidPnTopology,
                                                             pCommitVidPn->AffectedVidPnSourceId,
                                                             TargetId,
                                                             &pVidPnPresentPath);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnAcquirePathInfo failed with Status = 0x%X, hVidPnTopology = 0x%llu, SourceId = 0x%I64x, "
                      "TargetId = 0x%I64x\n",
                      Status,
                      LONG_PTR(hVidPnTopology),
                      pCommitVidPn->AffectedVidPnSourceId,
                      TargetId));
            goto CommitVidPnExit;
        }

        Status = IsVidPnPathFieldsValid(pVidPnPresentPath);
        if (!NT_SUCCESS(Status))
        {
            goto CommitVidPnExit;
        }

        Status = SetSourceModeAndPath(pPinnedVidPnSourceModeInfo, pVidPnPresentPath);
        if (!NT_SUCCESS(Status))
        {
            goto CommitVidPnExit;
        }

        Status = pVidPnTopologyInterface->pfnReleasePathInfo(hVidPnTopology, pVidPnPresentPath);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnReleasePathInfo failed with Status = 0x%X, hVidPnTopoogy = 0x%llu, pVidPnPresentPath = %p\n",
                      Status,
                      LONG_PTR(hVidPnTopology),
                      pVidPnPresentPath));
            goto CommitVidPnExit;
        }
        pVidPnPresentPath = NULL;
    }

CommitVidPnExit:

    NTSTATUS TempStatus = STATUS_SUCCESS;

    if ((pVidPnSourceModeSetInterface != NULL) && (hVidPnSourceModeSet != 0) && (pPinnedVidPnSourceModeInfo != NULL))
    {
        TempStatus = pVidPnSourceModeSetInterface->pfnReleaseModeInfo(hVidPnSourceModeSet, pPinnedVidPnSourceModeInfo);
        NT_ASSERT(NT_SUCCESS(TempStatus));
    }

    if ((pVidPnInterface != NULL) && (pCommitVidPn->hFunctionalVidPn != 0) && (hVidPnSourceModeSet != 0))
    {
        TempStatus = pVidPnInterface->pfnReleaseSourceModeSet(pCommitVidPn->hFunctionalVidPn, hVidPnSourceModeSet);
        NT_ASSERT(NT_SUCCESS(TempStatus));
    }

    if ((pVidPnTopologyInterface != NULL) && (hVidPnTopology != 0) && (pVidPnPresentPath != NULL))
    {
        TempStatus = pVidPnTopologyInterface->pfnReleasePathInfo(hVidPnTopology, pVidPnPresentPath);
        NT_ASSERT(NT_SUCCESS(TempStatus));
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return Status;
}

NTSTATUS VioGpuDod::SetSourceModeAndPath(CONST D3DKMDT_VIDPN_SOURCE_MODE *pSourceMode,
                                         CONST D3DKMDT_VIDPN_PRESENT_PATH *pPath)
{
    PAGED_CODE();
    VIOGPU_ASSERT(pPath->VidPnSourceId < MAX_VIEWS);

    NTSTATUS Status = STATUS_SUCCESS;

    UINT srcId = (UINT)pPath->VidPnSourceId;
    UINT scanId = (pPath->VidPnTargetId < MAX_SCANOUTS) ? (UINT)pPath->VidPnTargetId : 0;
    CURRENT_MODE *pCurrentMode = &m_CurrentMode[srcId];
    pCurrentMode->DispInfo.TargetId = scanId;
    DbgPrint(TRACE_LEVEL_FATAL,
             ("---> %s src %d scan %d (%dx%d)\n",
              __FUNCTION__,
              srcId,
              scanId,
              pSourceMode->Format.Graphics.VisibleRegionSize.cx,
              pSourceMode->Format.Graphics.VisibleRegionSize.cy));
    pCurrentMode->Scaling = pPath->ContentTransformation.Scaling;
    pCurrentMode->SrcModeWidth = pSourceMode->Format.Graphics.VisibleRegionSize.cx;
    pCurrentMode->SrcModeHeight = pSourceMode->Format.Graphics.VisibleRegionSize.cy;
    pCurrentMode->Rotation = pPath->ContentTransformation.Rotation;

    pCurrentMode->DispInfo.Width = pSourceMode->Format.Graphics.PrimSurfSize.cx;
    pCurrentMode->DispInfo.Height = pSourceMode->Format.Graphics.PrimSurfSize.cy;
    pCurrentMode->DispInfo.Pitch = pSourceMode->Format.Graphics.PrimSurfSize.cx *
                                   BPPFromPixelFormat(pCurrentMode->DispInfo.ColorFormat) / BITS_PER_BYTE;

    if (NT_SUCCESS(Status))
    {
        pCurrentMode->Flags.FullscreenPresent = TRUE;
        for (USHORT ModeIndex = 0; ModeIndex < m_pHWDevice->GetModeCount(); ++ModeIndex)
        {
            PVIDEO_MODE_INFORMATION pModeInfo = m_pHWDevice->GetModeInfo(ModeIndex);
            if (pCurrentMode->DispInfo.Width == pModeInfo->VisScreenWidth &&
                pCurrentMode->DispInfo.Height == pModeInfo->VisScreenHeight)
            {
                Status = m_pHWDevice->SetCurrentMode(m_pHWDevice->GetModeNumber(ModeIndex), pCurrentMode, scanId);
                if (NT_SUCCESS(Status))
                {
                    m_pHWDevice->SetCurrentModeIndex(scanId, ModeIndex);
                }
                break;
            }
        }
    }

    return Status;
}

NTSTATUS VioGpuDod::IsVidPnPathFieldsValid(CONST D3DKMDT_VIDPN_PRESENT_PATH *pPath) const
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    if (pPath->VidPnSourceId >= MAX_VIEWS)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("VidPnSourceId is 0x%I64x is too high (MAX_VIEWS is 0x%I64x)", pPath->VidPnSourceId, MAX_VIEWS));
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE;
    }
    else if (pPath->VidPnTargetId >= MAX_CHILDREN)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("VidPnTargetId is 0x%I64x is too high (MAX_CHILDREN is 0x%I64x)",
                  pPath->VidPnTargetId,
                  MAX_CHILDREN));
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET;
    }
    else if (pPath->GammaRamp.Type != D3DDDI_GAMMARAMP_DEFAULT)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("pPath contains a gamma ramp (0x%I64x)", pPath->GammaRamp.Type));
        return STATUS_GRAPHICS_GAMMA_RAMP_NOT_SUPPORTED;
    }
    else if ((pPath->ContentTransformation.Scaling != D3DKMDT_VPPS_IDENTITY) &&
             (pPath->ContentTransformation.Scaling != D3DKMDT_VPPS_CENTERED) &&
             (pPath->ContentTransformation.Scaling != D3DKMDT_VPPS_NOTSPECIFIED) &&
             (pPath->ContentTransformation.Scaling != D3DKMDT_VPPS_UNINITIALIZED))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pPath contains a non-identity scaling (0x%I64x)", pPath->ContentTransformation.Scaling));
        return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    }
    else if ((pPath->ContentTransformation.Rotation != D3DKMDT_VPPR_IDENTITY) &&
             (pPath->ContentTransformation.Rotation != D3DKMDT_VPPR_ROTATE90) &&
             (pPath->ContentTransformation.Rotation != D3DKMDT_VPPR_NOTSPECIFIED) &&
             (pPath->ContentTransformation.Rotation != D3DKMDT_VPPR_UNINITIALIZED))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pPath contains a not-supported rotation (0x%I64x)", pPath->ContentTransformation.Rotation));
        return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    }
    else if ((pPath->VidPnTargetColorBasis != D3DKMDT_CB_SCRGB) &&
             (pPath->VidPnTargetColorBasis != D3DKMDT_CB_UNINITIALIZED))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("pPath has a non-linear RGB color basis (0x%I64x)", pPath->VidPnTargetColorBasis));
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return STATUS_SUCCESS;
}

NTSTATUS VioGpuDod::IsVidPnSourceModeFieldsValid(CONST D3DKMDT_VIDPN_SOURCE_MODE *pSourceMode) const
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    if (pSourceMode->Type != D3DKMDT_RMT_GRAPHICS)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("pSourceMode is a non-graphics mode (0x%I64x)", pSourceMode->Type));
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    }
    else if ((pSourceMode->Format.Graphics.ColorBasis != D3DKMDT_CB_SCRGB) &&
             (pSourceMode->Format.Graphics.ColorBasis != D3DKMDT_CB_UNINITIALIZED))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pSourceMode has a non-linear RGB color basis (0x%I64x)", pSourceMode->Format.Graphics.ColorBasis));
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    }
    else if (pSourceMode->Format.Graphics.PixelValueAccessMode != D3DKMDT_PVAM_DIRECT)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pSourceMode has a palettized access mode (0x%I64x)",
                  pSourceMode->Format.Graphics.PixelValueAccessMode));
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    }
    else
    {
        if (pSourceMode->Format.Graphics.PixelFormat == D3DDDIFMT_A8R8G8B8)
        {
            return STATUS_SUCCESS;
        }
    }

    DbgPrint(TRACE_LEVEL_ERROR,
             ("pSourceMode has an unknown pixel format (0x%I64x)", pSourceMode->Format.Graphics.PixelFormat));

    return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
}

NTSTATUS
VioGpuDod::UpdateActiveVidPnPresentPath(_In_ CONST DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *CONST pUpdateActiveVidPnPresentPath)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT(pUpdateActiveVidPnPresentPath != NULL);
    VIOGPU_ASSERT(pUpdateActiveVidPnPresentPath->VidPnPresentPathInfo.VidPnSourceId < MAX_VIEWS);

    NTSTATUS Status = IsVidPnPathFieldsValid(&(pUpdateActiveVidPnPresentPath->VidPnPresentPathInfo));
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    UINT srcId = (UINT)pUpdateActiveVidPnPresentPath->VidPnPresentPathInfo.VidPnSourceId;
    if (srcId >= m_pHWDevice->GetNumScanouts())
    {
        return STATUS_SUCCESS;
    }

    m_CurrentMode[srcId].Flags.FullscreenPresent = TRUE;

    m_CurrentMode[srcId].Rotation = pUpdateActiveVidPnPresentPath->VidPnPresentPathInfo.ContentTransformation.Rotation;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return STATUS_SUCCESS;
}

PAGED_CODE_SEG_END

//
// Non-Paged Code
//
#pragma code_seg(push)
#pragma code_seg()

VOID VioGpuDod::DpcRoutine(VOID)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    m_pHWDevice->DpcRoutine(&m_DxgkInterface);
    m_DxgkInterface.DxgkCbNotifyDpc((HANDLE)m_DxgkInterface.DeviceHandle);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

BOOLEAN VioGpuDod::InterruptRoutine(_In_ ULONG MessageNumber)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s\n", __FUNCTION__));
    if (IsHardwareInit())
    {
        return m_pHWDevice ? m_pHWDevice->InterruptRoutine(&m_DxgkInterface, MessageNumber) : FALSE;
    }
    return FALSE;
}

VOID VioGpuDod::ResetDevice(VOID)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));
    m_pHWDevice->ResetDevice();
}

NTSTATUS VioGpuDod::SystemDisplayEnable(_In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                        _In_ PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags,
                                        _Out_ UINT *pWidth,
                                        _Out_ UINT *pHeight,
                                        _Out_ D3DDDIFORMAT *pColorFormat)
{
    UNREFERENCED_PARAMETER(Flags);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT((TargetId < MAX_CHILDREN) || (TargetId == D3DDDI_ID_UNINITIALIZED));

    if (!IsVgaDevice())
    {
        return STATUS_UNSUCCESSFUL;
    }

    m_CurrentMode[0].Flags.FrameBufferIsActive = FALSE;
    m_pHWDevice->ResetToVgaMode();

    if (m_CurrentMode[0].RamFrameBuffer == nullptr)
    {
        return STATUS_UNSUCCESSFUL;
    }

    m_CurrentMode[0].FrameBuffer = m_CurrentMode[0].RamFrameBuffer;
    m_CurrentMode[0].Flags.FrameBufferIsActive = TRUE;
    m_CurrentMode[0].Rotation = D3DKMDT_VPPR_IDENTITY;
    *pWidth = m_CurrentMode[0].DispInfo.Width = m_SystemDisplayInfo.Width;
    *pHeight = m_CurrentMode[0].DispInfo.Height = m_SystemDisplayInfo.Height;
    *pColorFormat = m_CurrentMode[0].DispInfo.ColorFormat = m_SystemDisplayInfo.ColorFormat;
    m_CurrentMode[0].DispInfo.Pitch = m_SystemDisplayInfo.Pitch = (BPPFromPixelFormat(m_SystemDisplayInfo.ColorFormat) /
                                                                BITS_PER_BYTE) *
                                                               m_SystemDisplayInfo.Width;

    DbgPrint(TRACE_LEVEL_INFORMATION, ("<--- %s (%dx%dx%d)\n", __FUNCTION__, *pWidth, *pHeight, *pColorFormat));

    return STATUS_SUCCESS;
}

VOID VioGpuDod::SystemDisplayWrite(_In_reads_bytes_(SourceHeight *SourceStride) VOID *pSource,
                                   _In_ UINT SourceWidth,
                                   _In_ UINT SourceHeight,
                                   _In_ UINT SourceStride,
                                   _In_ INT PositionX,
                                   _In_ INT PositionY)
{
    if (m_CurrentMode[0].Flags.FrameBufferIsActive)
    {
        RECT Rect = {0};
        BLT_INFO SrcBltInfo = {0};
        BLT_INFO DstBltInfo = {0};

        Rect.left = PositionX;
        Rect.top = PositionY;
        Rect.right = Rect.left + SourceWidth;
        Rect.bottom = Rect.top + SourceHeight;

        DstBltInfo.pBits = m_CurrentMode[0].FrameBuffer;
        DstBltInfo.Pitch = m_CurrentMode[0].DispInfo.Pitch;
        DstBltInfo.BitsPerPel = BPPFromPixelFormat(m_CurrentMode[0].DispInfo.ColorFormat);
        DstBltInfo.Offset.x = 0;
        DstBltInfo.Offset.y = 0;
        DstBltInfo.Rotation = m_CurrentMode[0].Rotation;
        DstBltInfo.Width = m_CurrentMode[0].DispInfo.Width;
        DstBltInfo.Height = m_CurrentMode[0].DispInfo.Height;

        SrcBltInfo.pBits = pSource;
        SrcBltInfo.Pitch = SourceStride;
        SrcBltInfo.BitsPerPel = BPPFromPixelFormat(D3DDDIFMT_A8R8G8B8);
        SrcBltInfo.Offset.x = -PositionX;
        SrcBltInfo.Offset.y = -PositionY;
        SrcBltInfo.Rotation = D3DKMDT_VPPR_IDENTITY;
        SrcBltInfo.Width = SourceWidth;
        SrcBltInfo.Height = SourceHeight;

        BltBits(&DstBltInfo, &SrcBltInfo, &Rect);
    }
}

#pragma code_seg(pop) // End Non-Paged Code

PAGED_CODE_SEG_BEGIN
NTSTATUS VioGpuDod::WriteRegistryString(_In_ HANDLE DevInstRegKeyHandle, _In_ PCWSTR pszwValueName, _In_ PCSTR pszValue)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    ANSI_STRING AnsiStrValue;
    UNICODE_STRING UnicodeStrValue;
    UNICODE_STRING UnicodeStrValueName;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    RtlInitUnicodeString(&UnicodeStrValueName, pszwValueName);

    RtlInitAnsiString(&AnsiStrValue, pszValue);
    Status = RtlAnsiStringToUnicodeString(&UnicodeStrValue, &AnsiStrValue, TRUE);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("RtlAnsiStringToUnicodeString failed with Status: 0x%X\n", Status));
        return Status;
    }

    Status = ZwSetValueKey(DevInstRegKeyHandle,
                           &UnicodeStrValueName,
                           0,
                           REG_SZ,
                           UnicodeStrValue.Buffer,
                           UnicodeStrValue.MaximumLength);

    RtlFreeUnicodeString(&UnicodeStrValue);

    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("ZwSetValueKey failed with Status: 0x%X\n", Status));
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuDod::WriteRegistryDWORD(_In_ HANDLE DevInstRegKeyHandle, _In_ PCWSTR pszwValueName, _In_ PDWORD pdwValue)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    UNICODE_STRING UnicodeStrValueName;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    RtlInitUnicodeString(&UnicodeStrValueName, pszwValueName);

    Status = ZwSetValueKey(DevInstRegKeyHandle, &UnicodeStrValueName, 0, REG_DWORD, pdwValue, sizeof(DWORD));

    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("ZwSetValueKey failed with Status: 0x%X\n", Status));
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuDod::ReadRegistryDWORD(_In_ HANDLE DevInstRegKeyHandle,
                                      _In_ PCWSTR pszwValueName,
                                      _Inout_ PDWORD pdwValue)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    UNICODE_STRING UnicodeStrValueName;
    ULONG ulRes;
    UCHAR Buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(DWORD)];
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    RtlInitUnicodeString(&UnicodeStrValueName, pszwValueName);

    Status = ZwQueryValueKey(DevInstRegKeyHandle,
                             &UnicodeStrValueName,
                             KeyValuePartialInformation,
                             Buf,
                             sizeof(Buf),
                             &ulRes);

    if (Status == STATUS_SUCCESS)
    {
        if (((PKEY_VALUE_PARTIAL_INFORMATION)Buf)->Type == REG_DWORD &&
            (((PKEY_VALUE_PARTIAL_INFORMATION)Buf)->DataLength == sizeof(DWORD)))
        {
            *pdwValue = *((PDWORD) & (((PKEY_VALUE_PARTIAL_INFORMATION)Buf)->Data));
        }
        else
        {
            Status = STATUS_INVALID_PARAMETER;
            VioGpuDbgBreak();
        }
    }

    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("ZwQueryValueKey failed with Status: 0x%X\n", Status));
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuDod::SetRegisterInfo(_In_ ULONG Id, _In_ DWORD MemSize)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PCSTR StrHWInfoChipType = "QEMU VIRTIO GPU";
    PCSTR StrHWInfoDacType = "VIRTIO GPU";
    PCSTR StrHWInfoAdapterString = "VIRTIO GPU";
    PCSTR StrHWInfoBiosString = "SEABIOS VIRTIO GPU";

    HANDLE DevInstRegKeyHandle;
    Status = IoOpenDeviceRegistryKey(m_pPhysicalDevice, PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &DevInstRegKeyHandle);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("IoOpenDeviceRegistryKey failed for PDO: 0x%p, Status: 0x%X", m_pPhysicalDevice, Status));
        return Status;
    }

    do
    {
        Status = WriteRegistryString(DevInstRegKeyHandle, L"HardwareInformation.ChipType", StrHWInfoChipType);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("WriteRegistryString failed for ChipType with Status: 0x%X", Status));
            break;
        }

        Status = WriteRegistryString(DevInstRegKeyHandle, L"HardwareInformation.DacType", StrHWInfoDacType);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("WriteRegistryString failed DacType with Status: 0x%X", Status));
            break;
        }

        Status = WriteRegistryString(DevInstRegKeyHandle, L"HardwareInformation.AdapterString", StrHWInfoAdapterString);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("WriteRegistryString failed for AdapterString with Status: 0x%X", Status));
            break;
        }

        Status = WriteRegistryString(DevInstRegKeyHandle, L"HardwareInformation.BiosString", StrHWInfoBiosString);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("WriteRegistryString failed for BiosString with Status: 0x%X", Status));
            break;
        }

        DWORD MemorySize = MemSize;
        Status = WriteRegistryDWORD(DevInstRegKeyHandle, L"HardwareInformation.MemorySize", &MemorySize);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("WriteRegistryDWORD failed for MemorySize with Status: 0x%X", Status));
            break;
        }

        DWORD DeviceId = Id;
        Status = WriteRegistryDWORD(DevInstRegKeyHandle, L"VioGpuAdapterID", &DeviceId);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("WriteRegistryDWORD failed for VioGpuAdapterID with Status: 0x%X", Status));
        }
    } while (0);

    ZwClose(DevInstRegKeyHandle);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuDod::SetRegisterConfigInfo()
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    DWORD value = 0;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    HANDLE DevInstRegKeyHandle;
    Status = IoOpenDeviceRegistryKey(m_pPhysicalDevice, PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &DevInstRegKeyHandle);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("IoOpenDeviceRegistryKey failed for PDO: 0x%p, Status: 0x%X", m_pPhysicalDevice, Status));
        return Status;
    }

    do
    {
        if (IsPersistentDispMode0Set())
        {
            value = GetPersistentDispMode0Width();
            Status = WriteRegistryDWORD(DevInstRegKeyHandle, L"PersistentDispMode0Width", &value);
            if (!NT_SUCCESS(Status))
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("WriteRegistryDWORD failed for PersistentDispMode0Width with Status: 0x%X", Status));
                break;
            }

            value = GetPersistentDispMode0Height();
            Status = WriteRegistryDWORD(DevInstRegKeyHandle, L"PersistentDispMode0Height", &value);
            if (!NT_SUCCESS(Status))
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("WriteRegistryDWORD failed for PersistentDispMode0Height with Status: 0x%X", Status));
                break;
            }
        }
    } while (0);

    ZwClose(DevInstRegKeyHandle);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuDod::GetRegisterInfo(void)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    NTSTATUS StatusOptional = STATUS_SUCCESS;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    HANDLE DevInstRegKeyHandle;
    Status = IoOpenDeviceRegistryKey(m_pPhysicalDevice, PLUGPLAY_REGKEY_DRIVER, KEY_READ, &DevInstRegKeyHandle);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("IoOpenDeviceRegistryKey failed for PDO: 0x%p, Status: 0x%X", m_pPhysicalDevice, Status));
        return Status;
    }

    DWORD value = 0;
    Status = ReadRegistryDWORD(DevInstRegKeyHandle, L"HWCursor", &value);
    if (NT_SUCCESS(Status))
    {
        SetPointerEnabled(!!value);
    }

    value = 0;
    Status = ReadRegistryDWORD(DevInstRegKeyHandle, L"FlexResolution", &value);
    if (NT_SUCCESS(Status))
    {
        SetFlexResolution(!!value);
    }

    value = 0;
    Status = ReadRegistryDWORD(DevInstRegKeyHandle, L"UsePhysicalMemory", &value);
    if (NT_SUCCESS(Status))
    {
        SetUsePhysicalMemory(!!value);
    }

    value = 0;
    Status = ReadRegistryDWORD(DevInstRegKeyHandle, L"UsePresentProgress", &value);
    if (NT_SUCCESS(Status))
    {
        SetUsePresentProgress(!!value);
    }

    // The following keys are optional and no need to report error if them are missing
    value = 0;
    StatusOptional = ReadRegistryDWORD(DevInstRegKeyHandle, L"PersistentDispMode0Width", &value);
    if (NT_SUCCESS(StatusOptional))
    {
        SetPersistentDispMode0Width((USHORT)value);
    }

    value = 0;
    StatusOptional = ReadRegistryDWORD(DevInstRegKeyHandle, L"PersistentDispMode0Height", &value);
    if (NT_SUCCESS(StatusOptional))
    {
        SetPersistentDispMode0Height((USHORT)value);
    }

    ZwClose(DevInstRegKeyHandle);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

VioGpuAdapter::VioGpuAdapter(_In_ VioGpuDod *pVioGpuDod)
{
    PAGED_CODE();
    RtlZeroMemory(&m_VioDev, sizeof(m_VioDev));
    m_pVioGpuDod = pVioGpuDod;
    RtlZeroMemory(m_CurrentModeIndex, sizeof(m_CurrentModeIndex));
    RtlZeroMemory(m_CustomModeIndex, sizeof(m_CustomModeIndex));
    RtlZeroMemory(m_EDIDs, sizeof(m_EDIDs));
    RtlZeroMemory(m_bEDID, sizeof(m_bEDID));
    RtlZeroMemory(m_bConnected, sizeof(m_bConnected));
    m_ModeInfo = NULL;
    m_ModeCount = 0;
    m_Id = g_InstanceId++;
    RtlZeroMemory(m_pFrameBuf, sizeof(m_pFrameBuf));
    m_pCursorBuf = NULL;
    m_PendingWorks = 0;
    m_bStopWorkThread = FALSE;
    m_pWorkThread = NULL;
    m_ResolutionEvent = NULL;
    m_ResolutionEventHandle = NULL;
    m_u64HostFeatures = 0;
    m_u64GuestFeatures = 0;
    m_u32NumCapsets = 0;
    m_u32NumScanouts = 0;

    KeInitializeEvent(&m_ConfigUpdateEvent, SynchronizationEvent, FALSE);
    m_InitialScanPending = 0;
    m_bInitialScanArmed = FALSE;
}

VioGpuAdapter::~VioGpuAdapter(void)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_FATAL, ("---> %s 0x%p\n", __FUNCTION__, this));
    CloseResolutionEvent();
    DestroyCursor();
    for (UINT scan = 0; scan < MAX_SCANOUTS; scan++)
    {
        DestroyFrameBufferObj(TRUE, FALSE, scan);
    }
    VioGpuAdapterClose();
    HWClose();
    delete[] m_ModeInfo;
    m_ModeInfo = NULL;
    RtlZeroMemory(m_CurrentModeIndex, sizeof(m_CurrentModeIndex));
    m_ModeCount = 0;
    m_Id = 0;
    DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s\n", __FUNCTION__));
}

NTSTATUS VioGpuAdapter::SetCurrentMode(ULONG Mode, CURRENT_MODE *pCurrentMode, UINT scanId)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_ERROR, ("---> %s - %d: scan %d Mode = %d\n", __FUNCTION__, m_Id, scanId, Mode));
    for (ULONG idx = 0; idx < GetModeCount(); idx++)
    {
        if (Mode == m_ModeInfo[idx].ModeIndex /*m_ModeNumbers[idx]*/)
        {
            if (pCurrentMode->Flags.FrameBufferIsActive)
            {
                DestroyFrameBufferObj(FALSE, FALSE, scanId);
                pCurrentMode->Flags.FrameBufferIsActive = FALSE;
            }
            if (CreateFrameBufferObj(&m_ModeInfo[idx], pCurrentMode, scanId))
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("%s device %d: setting current mode %d (%d x %d)\n",
                          __FUNCTION__,
                          m_Id,
                          Mode,
                          m_ModeInfo[idx].VisScreenWidth,
                          m_ModeInfo[idx].VisScreenHeight));
                return STATUS_SUCCESS;
            }
        }
    }
    DbgPrint(TRACE_LEVEL_ERROR, ("<--- %s failed\n", __FUNCTION__));
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS VioGpuAdapter::VioGpuAdapterInit(DXGK_DISPLAY_INFORMATION *pDispInfo)
{
    PAGED_CODE();
    NTSTATUS status = STATUS_SUCCESS;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    UNREFERENCED_PARAMETER(pDispInfo);
    if (m_pVioGpuDod->IsHardwareInit())
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("Already Initialized\n"));
        VioGpuDbgBreak();
        return status;
    }
    status = VirtIoDeviceInit();
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("Failed to initialize virtio device, error %x\n", status));
        VioGpuDbgBreak();
        return status;
    }

    m_u64HostFeatures = virtio_get_features(&m_VioDev);
    m_u64GuestFeatures = 0;
    do
    {
        struct virtqueue *vqs[2];
        if (!AckFeature(VIRTIO_F_VERSION_1))
        {
            status = STATUS_UNSUCCESSFUL;
            break;
        }

        AckFeature(VIRTIO_F_ACCESS_PLATFORM);

        status = virtio_set_features(&m_VioDev, m_u64GuestFeatures);
        if (!NT_SUCCESS(status))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("%s virtio_set_features failed with %x\n", __FUNCTION__, status));
            VioGpuDbgBreak();
            break;
        }

        status = virtio_find_queues(&m_VioDev, 2, vqs);
        if (!NT_SUCCESS(status))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("virtio_find_queues failed with error %x\n", status));
            VioGpuDbgBreak();
            break;
        }

        if (!m_CtrlQueue.Init(&m_VioDev, vqs[0], 0) || !m_CursorQueue.Init(&m_VioDev, vqs[1], 1))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("Failed to initialize virtio queues\n"));
            status = STATUS_INSUFFICIENT_RESOURCES;
            VioGpuDbgBreak();
            break;
        }

        virtio_get_config(&m_VioDev,
                          FIELD_OFFSET(GPU_CONFIG, num_scanouts),
                          &m_u32NumScanouts,
                          sizeof(m_u32NumScanouts));

        virtio_get_config(&m_VioDev, FIELD_OFFSET(GPU_CONFIG, num_capsets), &m_u32NumCapsets, sizeof(m_u32NumCapsets));
    } while (0);
    if (status == STATUS_SUCCESS)
    {
        virtio_device_ready(&m_VioDev);
        m_pVioGpuDod->SetHardwareInit(TRUE);
    }
    else
    {
        virtio_add_status(&m_VioDev, VIRTIO_CONFIG_S_FAILED);
        VioGpuDbgBreak();
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return status;
}

NTSTATUS VioGpuAdapter::SetPowerState(DXGK_DEVICE_INFO *pDeviceInfo,
                                      DEVICE_POWER_STATE DevicePowerState,
                                      CURRENT_MODE *pCurrentMode)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_FATAL, ("---> %s DevicePowerState = %d\n", __FUNCTION__, DevicePowerState));
    UNREFERENCED_PARAMETER(pDeviceInfo);

    switch (DevicePowerState)
    {
        case PowerDeviceUnspecified:
        case PowerDeviceD0:
            {
                VioGpuAdapterInit(&pCurrentMode->DispInfo);
            }
            break;
        case PowerDeviceD1:
        case PowerDeviceD2:
        case PowerDeviceD3:
            {
                DestroyFrameBufferObj(TRUE, FALSE, 0);
                VioGpuAdapterClose();
                pCurrentMode->Flags.FrameBufferIsActive = FALSE;
                pCurrentMode->FrameBuffer = NULL;
            }
            break;
    }
    DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

BOOLEAN VioGpuAdapter::AckFeature(UINT64 Feature)
{
    PAGED_CODE();

    if (virtio_is_feature_enabled(m_u64HostFeatures, Feature))
    {
        virtio_feature_enable(m_u64GuestFeatures, Feature);
        return TRUE;
    }
    return FALSE;
}

NTSTATUS VioGpuAdapter::VirtIoDeviceInit()
{
    PAGED_CODE();

    return virtio_device_initialize(&m_VioDev,
                                    &VioGpuSystemOps,
                                    reinterpret_cast<IVioGpuPCI *>(this),
                                    m_PciResources.IsMSIEnabled());
}

PBYTE VioGpuAdapter::GetEdidData(UINT scanId)
{
    PAGED_CODE();

    if (scanId >= MAX_SCANOUTS)
    {
        scanId = 0;
    }
    return m_bEDID[scanId] ? m_EDIDs[scanId] : (PBYTE)(g_gpu_edid);
}

PBYTE VioGpuAdapter::GetCTA861Data(void)
{
    PAGED_CODE();

    if (m_bEDID[0])
    {
        PEDID_DATA_V1 edid_data = (PEDID_DATA_V1)m_EDIDs[0];
        if (edid_data->ExtensionFlag[0])
        {
            PEDID_CTA_861 cta_data = (PEDID_CTA_861)(m_EDIDs[0] + EDID_V1_BLOCK_SIZE);
            if (cta_data->ExtentionTag[0] >= 2 && cta_data->Revision[0] >= 3)
            {
                return (PBYTE)cta_data;
            }
        }
    }
    return NULL;
}

VOID VioGpuAdapter::CreateResolutionEvent(VOID)
{
    PAGED_CODE();

    if (m_ResolutionEvent != NULL && m_ResolutionEventHandle != NULL)
    {
        return;
    }
    DECLARE_UNICODE_STRING_SIZE(DeviceNumber, 10);
    DECLARE_UNICODE_STRING_SIZE(EventName, 256);

    RtlIntegerToUnicodeString(m_Id, 10, &DeviceNumber);
    NTSTATUS status = RtlUnicodeStringPrintf(&EventName,
                                             L"%ws%ws%ws",
                                             BASE_NAMED_OBJECTS,
                                             RESOLUTION_EVENT_NAME,
                                             DeviceNumber.Buffer);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("RtlUnicodeStringPrintf failed 0x%x\n", status));
        return;
    }
    m_ResolutionEvent = IoCreateNotificationEvent(&EventName, &m_ResolutionEventHandle);
    if (m_ResolutionEvent == NULL)
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("<--> %s\n", __FUNCTION__));
        return;
    }
    KeClearEvent(m_ResolutionEvent);
    ObReferenceObject(m_ResolutionEvent);
}

VOID VioGpuAdapter::NotifyResolutionEvent(VOID)
{
    PAGED_CODE();

    if (m_ResolutionEvent != NULL)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("NotifyResolutionEvent\n"));
        KeSetEvent(m_ResolutionEvent, IO_NO_INCREMENT, FALSE);
        KeClearEvent(m_ResolutionEvent);
    }
}

VOID VioGpuAdapter::CloseResolutionEvent(VOID)
{
    PAGED_CODE();

    if (m_ResolutionEventHandle != NULL)
    {
        ZwClose(m_ResolutionEventHandle);
        m_ResolutionEventHandle = NULL;
    }

    if (m_ResolutionEvent != NULL)
    {
        ObDereferenceObject(m_ResolutionEvent);
        m_ResolutionEvent = NULL;
    }
}

NTSTATUS VioGpuAdapter::HWInit(PCM_RESOURCE_LIST pResList, DXGK_DISPLAY_INFORMATION *pDispInfo)
{
    PAGED_CODE();

    NTSTATUS status = STATUS_SUCCESS;
    HANDLE threadHandle = 0;
    DbgPrint(TRACE_LEVEL_INFORMATION, ("---> %s\n", __FUNCTION__));
    UINT size = 0;
    do
    {
        if (!m_PciResources.Init(GetVioGpu()->GetDxgkInterface(), pResList))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("Incomplete resources\n"));
            status = STATUS_INSUFFICIENT_RESOURCES;
            VioGpuDbgBreak();
            break;
        }

        status = VioGpuAdapterInit(pDispInfo);
        if (!NT_SUCCESS(status))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("%s Failed initialize adapter %x\n", __FUNCTION__, status));
            VioGpuDbgBreak();
            break;
        }

        size = m_CtrlQueue.QueryAllocation() + m_CursorQueue.QueryAllocation();
        DbgPrint(TRACE_LEVEL_FATAL, ("%s size %d\n", __FUNCTION__, size));
        ASSERT(size);

        if (!m_GpuBuf.Init(size))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("Failed to initialize buffers\n"));
            status = STATUS_INSUFFICIENT_RESOURCES;
            VioGpuDbgBreak();
            break;
        }

        m_CtrlQueue.SetGpuBuf(&m_GpuBuf);
        m_CursorQueue.SetGpuBuf(&m_GpuBuf);

        if (!m_Idr.Init(1))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("Failed to initialize id generator\n"));
            status = STATUS_INSUFFICIENT_RESOURCES;
            VioGpuDbgBreak();
            break;
        }

    } while (0);
    // Exit if the block above failed
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    status = PsCreateSystemThread(&threadHandle,
                                  (ACCESS_MASK)0,
                                  NULL,
                                  (HANDLE)0,
                                  NULL,
                                  VioGpuAdapter::ThreadWork,
                                  this);

    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("%s failed to create system thread, status %x\n", __FUNCTION__, status));
        VioGpuDbgBreak();
        return status;
    }
    ObReferenceObjectByHandle(threadHandle, THREAD_ALL_ACCESS, NULL, KernelMode, (PVOID *)(&m_pWorkThread), NULL);

    ZwClose(threadHandle);

    status = BuildModeList(pDispInfo);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("%s GetModeList failed with %x\n", __FUNCTION__, status));
        VioGpuDbgBreak();
    }

    PHYSICAL_ADDRESS fb_pa = m_PciResources.GetPciBar(0)->GetPA();
    UINT fb_size = (UINT)m_PciResources.GetPciBar(0)->GetSize();
    UINT req_size = pDispInfo->Pitch * pDispInfo->Height;
    req_size = max(req_size, 0x1000000);

    UINT max_res_size = MIN_WIDTH_SIZE * MIN_HEIGHT_SIZE;
    for (UINT idx = 0; idx < m_ModeCount; idx++)
    {
        UINT res_size = m_ModeInfo[idx].VisScreenWidth * m_ModeInfo[idx].VisScreenHeight;
        max_res_size = max(max_res_size, res_size);
    }

    req_size = max(req_size, (max_res_size * 4));

    if (fb_pa.QuadPart != 0LL)
    {
        pDispInfo->PhysicAddress = fb_pa;
    }

    if (fb_size < req_size)
    {
        m_pVioGpuDod->SetUsePhysicalMemory(FALSE);
    }

    if (!m_pVioGpuDod->IsUsePhysicalMemory() || fb_pa.QuadPart == 0 || fb_size < req_size)
    {
        fb_pa.QuadPart = 0LL;
        fb_size = max(req_size, fb_size);
    }

    if (!m_FrameSegment[0].Init(fb_size, &fb_pa))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("%s failed to allocate FB memory segment\n", __FUNCTION__));
        status = STATUS_INSUFFICIENT_RESOURCES;
        VioGpuDbgBreak();
        return status;
    }

    // Each active scanout needs its OWN framebuffer segment: VioGpuObj::Init does
    // not sub-allocate, it just references the segment base, so two scanouts
    // sharing one segment would overlap. Scanout 0 above may use the PCI BAR (VGA
    // post-display); the secondary scanouts always use system memory (pass a zero
    // PA so VioGpuMemSegment::Init allocates a backing buffer).
    ULONG numScanouts = GetNumScanouts();

    // Hotplug initial state: the PRIMARY (scanout 0) is always connected (VGA /
    // post-display, never removed). Secondary heads start DISCONNECTED and
    // hotplug in when the host enables them (pmodes[i].enabled, driven from
    // GetDisplayInfo). The initial GetDisplayInfo below refines this before the
    // driver is "started" (so QueryChildStatus is already correct at Start).
    for (UINT scan = 0; scan < MAX_SCANOUTS; scan++)
    {
        m_bConnected[scan] = (scan == 0);
    }

    for (UINT scan = 1; scan < numScanouts; scan++)
    {
        PHYSICAL_ADDRESS sys_pa = {0};
        if (!m_FrameSegment[scan].Init(req_size, &sys_pa))
        {
            DbgPrint(TRACE_LEVEL_FATAL,
                     ("%s failed to allocate FB memory segment for scanout %u\n", __FUNCTION__, scan));
            status = STATUS_INSUFFICIENT_RESOURCES;
            VioGpuDbgBreak();
            return status;
        }
    }

    if (!m_CursorSegment.Init(POINTER_SIZE * POINTER_SIZE * 4, NULL))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("%s failed to allocate Cursor memory segment\n", __FUNCTION__));
        status = STATUS_INSUFFICIENT_RESOURCES;
        VioGpuDbgBreak();
        return status;
    }

    return status;
}

NTSTATUS VioGpuAdapter::HWClose(void)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_INFORMATION, ("---> %s\n", __FUNCTION__));
    m_pVioGpuDod->SetHardwareInit(FALSE);

    LARGE_INTEGER timeout = {0};
    timeout.QuadPart = Int32x32To64(1000, -10000);

    m_bStopWorkThread = TRUE;
    KeSetEvent(&m_ConfigUpdateEvent, IO_NO_INCREMENT, FALSE);

    if (KeWaitForSingleObject(m_pWorkThread, Executive, KernelMode, FALSE, &timeout) == STATUS_TIMEOUT)
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("---> Failed to exit the worker thread\n"));
        VioGpuDbgBreak();
    }

    ObDereferenceObject(m_pWorkThread);

    for (UINT scan = 0; scan < MAX_SCANOUTS; scan++)
    {
        m_FrameSegment[scan].Close();
    }
    m_CursorSegment.Close();

    DbgPrint(TRACE_LEVEL_INFORMATION, ("<--- %s\n", __FUNCTION__));

    return STATUS_SUCCESS;
}

BOOLEAN FindUpdateRect(_In_ ULONG NumMoves,
                       _In_ D3DKMT_MOVE_RECT *pMoves,
                       _In_ ULONG NumDirtyRects,
                       _In_ PRECT pDirtyRect,
                       _In_ D3DKMDT_VIDPN_PRESENT_PATH_ROTATION Rotation,
                       _Out_ PRECT pUpdateRect)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Rotation);
    BOOLEAN updated = FALSE;

    if (pUpdateRect == NULL)
    {
        return FALSE;
    }

    if (NumMoves == 0 && NumDirtyRects == 0)
    {
        pUpdateRect->bottom = 0;
        pUpdateRect->left = 0;
        pUpdateRect->right = 0;
        pUpdateRect->top = 0;
    }

    for (ULONG i = 0; i < NumMoves; i++)
    {
        PRECT pRect = &pMoves[i].DestRect;
        if (!updated)
        {
            *pUpdateRect = *pRect;
            updated = TRUE;
        }
        else
        {
            pUpdateRect->bottom = max(pRect->bottom, pUpdateRect->bottom);
            pUpdateRect->left = min(pRect->left, pUpdateRect->left);
            pUpdateRect->right = max(pRect->right, pUpdateRect->right);
            pUpdateRect->top = min(pRect->top, pUpdateRect->top);
        }
    }
    for (ULONG i = 0; i < NumDirtyRects; i++)
    {
        PRECT pRect = &pDirtyRect[i];
        if (!updated)
        {
            *pUpdateRect = *pRect;
            updated = TRUE;
        }
        else
        {
            pUpdateRect->bottom = max(pRect->bottom, pUpdateRect->bottom);
            pUpdateRect->left = min(pRect->left, pUpdateRect->left);
            pUpdateRect->right = max(pRect->right, pUpdateRect->right);
            pUpdateRect->top = min(pRect->top, pUpdateRect->top);
        }
    }
    if (Rotation == D3DKMDT_VPPR_ROTATE90 || Rotation == D3DKMDT_VPPR_ROTATE270)
    {
    }
    return updated;
}

NTSTATUS VioGpuAdapter::ExecutePresentDisplayOnly(_In_ BYTE *DstAddr,
                                                  _In_ UINT DstBitPerPixel,
                                                  _In_ BYTE *SrcAddr,
                                                  _In_ UINT SrcBytesPerPixel,
                                                  _In_ LONG SrcPitch,
                                                  _In_ ULONG NumMoves,
                                                  _In_ D3DKMT_MOVE_RECT *pMoves,
                                                  _In_ ULONG NumDirtyRects,
                                                  _In_ RECT *pDirtyRect,
                                                  _In_ D3DKMDT_VIDPN_PRESENT_PATH_ROTATION Rotation,
                                                  _In_ const CURRENT_MODE *pModeCur)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    BLT_INFO SrcBltInfo = {0};
    BLT_INFO DstBltInfo = {0};
    UINT resid = 0;
    RECT updrect = {0};
    ULONG offset = 0UL;

    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("SrcBytesPerPixel = %d DstBitPerPixel = %d (%dx%d)\n",
              SrcBytesPerPixel,
              DstBitPerPixel,
              pModeCur->SrcModeWidth,
              pModeCur->SrcModeHeight));

    DstBltInfo.pBits = DstAddr;
    DstBltInfo.Pitch = pModeCur->DispInfo.Pitch;
    DstBltInfo.BitsPerPel = DstBitPerPixel;
    DstBltInfo.Offset.x = 0;
    DstBltInfo.Offset.y = 0;
    DstBltInfo.Rotation = Rotation;
    DstBltInfo.Width = pModeCur->SrcModeWidth;
    DstBltInfo.Height = pModeCur->SrcModeHeight;

    SrcBltInfo.pBits = SrcAddr;
    SrcBltInfo.Pitch = SrcPitch;
    SrcBltInfo.BitsPerPel = SrcBytesPerPixel * BITS_PER_BYTE;
    SrcBltInfo.Offset.x = 0;
    SrcBltInfo.Offset.y = 0;
    SrcBltInfo.Rotation = D3DKMDT_VPPR_IDENTITY;
    if (Rotation == D3DKMDT_VPPR_ROTATE90 || Rotation == D3DKMDT_VPPR_ROTATE270)
    {
        SrcBltInfo.Width = DstBltInfo.Height;
        SrcBltInfo.Height = DstBltInfo.Width;
    }
    else
    {
        SrcBltInfo.Width = DstBltInfo.Width;
        SrcBltInfo.Height = DstBltInfo.Height;
    }

    for (UINT i = 0; i < NumMoves; i++)
    {
        RECT *pDestRect = &pMoves[i].DestRect;
        BltBits(&DstBltInfo, &SrcBltInfo, pDestRect);
    }

    for (UINT i = 0; i < NumDirtyRects; i++)
    {
        RECT *pRect = &pDirtyRect[i];
        BltBits(&DstBltInfo, &SrcBltInfo, pRect);
    }
    if (!FindUpdateRect(NumMoves, pMoves, NumDirtyRects, pDirtyRect, Rotation, &updrect))
    {
        updrect.top = 0;
        updrect.left = 0;
        updrect.bottom = pModeCur->SrcModeHeight;
        updrect.right = pModeCur->SrcModeWidth;
    }
    // FIXME!!! rotation
    offset = (updrect.top * pModeCur->DispInfo.Pitch) +
             (updrect.left * ((DstBitPerPixel + BITS_PER_BYTE - 1) / BITS_PER_BYTE));

    // Route the transfer/flush to the framebuffer of the scanout this source
    // maps to (DispInfo.TargetId was set to scanId in SetSourceModeAndPath).
    // Without this, presenting to a secondary head would flush scanout 0's
    // resource -> the secondary head stays black.
    UINT scanId = (pModeCur->DispInfo.TargetId < MAX_SCANOUTS) ? (UINT)pModeCur->DispInfo.TargetId : 0;
    if (m_pFrameBuf[scanId] == NULL)
    {
        DbgPrint(TRACE_LEVEL_WARNING, ("present scanout %u has no framebuffer\n", scanId));
        return STATUS_UNSUCCESSFUL;
    }
    resid = m_pFrameBuf[scanId]->GetId();
    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("offset = %lu (XxYxWxH) (%dx%dx%dx%d) vs (%dx%dx%dx%d)\n",
              offset,
              updrect.left,
              updrect.top,
              updrect.right - updrect.left,
              updrect.bottom - updrect.top,
              0,
              0,
              pModeCur->SrcModeWidth,
              pModeCur->SrcModeHeight));

    m_CtrlQueue.TransferToHost2D(resid,
                                 offset,
                                 updrect.right - updrect.left,
                                 updrect.bottom - updrect.top,
                                 updrect.left,
                                 updrect.top);
    m_CtrlQueue.ResFlush(resid, updrect.right - updrect.left, updrect.bottom - updrect.top, updrect.left, updrect.top);

    return STATUS_SUCCESS;
}

VOID VioGpuAdapter::BlackOutScreen(CURRENT_MODE *pCurrentMod)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_INFORMATION, ("---> %s\n", __FUNCTION__));

    if (pCurrentMod->Flags.FrameBufferIsActive)
    {
        UINT ScreenHeight = pCurrentMod->DispInfo.Height;
        UINT ScreenPitch = pCurrentMod->DispInfo.Pitch;
        BYTE *pDst = (BYTE *)pCurrentMod->FrameBuffer;

        UINT resid = 0;

        if (pDst)
        {
            RtlZeroMemory(pDst, (ULONGLONG)ScreenHeight * ScreenPitch);
        }

        // FIXME!!! rotation

        UINT scanId = (pCurrentMod->DispInfo.TargetId < MAX_SCANOUTS) ? (UINT)pCurrentMod->DispInfo.TargetId : 0;
        if (m_pFrameBuf[scanId] == NULL)
        {
            DbgPrint(TRACE_LEVEL_WARNING, ("blackout scanout %u has no framebuffer\n", scanId));
            return;
        }
        resid = m_pFrameBuf[scanId]->GetId();

        m_CtrlQueue.TransferToHost2D(resid, 0UL, pCurrentMod->DispInfo.Width, pCurrentMod->DispInfo.Height, 0, 0);
        m_CtrlQueue.ResFlush(resid, pCurrentMod->DispInfo.Width, pCurrentMod->DispInfo.Height, 0, 0);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

NTSTATUS VioGpuAdapter::SetPointerShape(_In_ CONST DXGKARG_SETPOINTERSHAPE *pSetPointerShape,
                                        _In_ CONST CURRENT_MODE *pModeCur)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("<--> %s flag = %d pitch = %d, pixels = %p, id = %d, w = %d, h = %d, x = %d, y = %d\n",
              __FUNCTION__,
              pSetPointerShape->Flags.Value,
              pSetPointerShape->Pitch,
              pSetPointerShape->pPixels,
              pSetPointerShape->VidPnSourceId,
              pSetPointerShape->Width,
              pSetPointerShape->Height,
              pSetPointerShape->XHot,
              pSetPointerShape->YHot));

    // Monochrome (and masked-color) cursors are now converted to A8R8G8B8 inside UpdateCursor,
    // so we must NOT reject them here — otherwise Windows falls back to a SOFTWARE cursor
    // composited into the framebuffer (captured into the video/remote stream, laggy + double
    // with the client's own cursor). This gate was what kept the I-beam in software. (#977 follow-up)
    if (UpdateCursor(pSetPointerShape, pModeCur))
    {
        PGPU_UPDATE_CURSOR crsr;
        PGPU_VBUFFER vbuf;
        UINT ret = 0;
        crsr = (PGPU_UPDATE_CURSOR)m_CursorQueue.AllocCursor(&vbuf);
        RtlZeroMemory(crsr, sizeof(*crsr));

        crsr->hdr.type = VIRTIO_GPU_CMD_UPDATE_CURSOR;
        crsr->resource_id = m_pCursorBuf->GetId();
        // Target the head DXGK named (VidPnSourceId), NOT pModeCur->DispInfo.TargetId: in the 1:1:1 extend
        // mapping source id == scanout id, and the dispatcher already bounded VidPnSourceId < GetNumScanouts().
        // DispInfo.TargetId stays D3DDDI_ID_UNINITIALIZED (0xFFFFFFFF) for a head until a commit/resize sets it
        // (never for the primary in some paths) → the cursor landed on scanout 0xFFFFFFFF and vanished.
        crsr->pos.scanout_id = (ULONG)pSetPointerShape->VidPnSourceId;
        crsr->pos.x = 0;
        crsr->pos.y = 0;
        crsr->hot_x = pSetPointerShape->XHot;
        crsr->hot_y = pSetPointerShape->YHot;
        ret = m_CursorQueue.QueueCursor(vbuf);
        DbgPrint(TRACE_LEVEL_INFORMATION, ("<--- %s vbuf = %p, ret = %d\n", __FUNCTION__, vbuf, ret));
        if (ret == 0)
        {
            return STATUS_SUCCESS;
        }
        VioGpuDbgBreak();
    }
    DbgPrint(TRACE_LEVEL_ERROR, ("<--- %s Failed to create cursor\n", __FUNCTION__));
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS VioGpuAdapter::SetPointerPosition(_In_ CONST DXGKARG_SETPOINTERPOSITION *pSetPointerPosition,
                                           _In_ CONST CURRENT_MODE *pModeCur)
{
    PAGED_CODE();
    if (m_pCursorBuf != NULL)
    {
        PGPU_UPDATE_CURSOR crsr;
        PGPU_VBUFFER vbuf;
        UINT ret = 0;
        crsr = (PGPU_UPDATE_CURSOR)m_CursorQueue.AllocCursor(&vbuf);
        RtlZeroMemory(crsr, sizeof(*crsr));

        crsr->hdr.type = VIRTIO_GPU_CMD_MOVE_CURSOR;
        crsr->resource_id = m_pCursorBuf->GetId();
        // Head index from DXGK (bounded < GetNumScanouts() by the dispatcher), not the possibly-uninitialized
        // pModeCur->DispInfo.TargetId — see SetPointerShape for the rationale.
        crsr->pos.scanout_id = (ULONG)pSetPointerPosition->VidPnSourceId;

        if (!pSetPointerPosition->Flags.Visible || (UINT)pSetPointerPosition->X > pModeCur->SrcModeWidth ||
            (UINT)pSetPointerPosition->Y > pModeCur->SrcModeHeight || pSetPointerPosition->X < 0 ||
            pSetPointerPosition->Y < 0)
        {
            DbgPrint(TRACE_LEVEL_VERBOSE,
                     ("---> %s (%d - %d) Visiable = %d Value = %x VidPnSourceId = %d\n",
                      __FUNCTION__,
                      pSetPointerPosition->X,
                      pSetPointerPosition->Y,
                      pSetPointerPosition->Flags.Visible,
                      pSetPointerPosition->Flags.Value,
                      pSetPointerPosition->VidPnSourceId));
            crsr->pos.x = 0;
            crsr->pos.y = 0;
        }
        else
        {
            DbgPrint(TRACE_LEVEL_VERBOSE,
                     ("---> %s (%d - %d) Visiable = %d Value = %x VidPnSourceId = %d posX = %d, psY = %d\n",
                      __FUNCTION__,
                      pSetPointerPosition->X,
                      pSetPointerPosition->Y,
                      pSetPointerPosition->Flags.Visible,
                      pSetPointerPosition->Flags.Value,
                      pSetPointerPosition->VidPnSourceId,
                      pSetPointerPosition->X,
                      pSetPointerPosition->Y));
            crsr->pos.x = pSetPointerPosition->X;
            crsr->pos.y = pSetPointerPosition->Y;
        }
        ret = m_CursorQueue.QueueCursor(vbuf);
        DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s vbuf = %p, ret = %d\n", __FUNCTION__, vbuf, ret));
        if (ret == 0)
        {
            return STATUS_SUCCESS;
        }
        VioGpuDbgBreak();
    }
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS VioGpuAdapter::Escape(_In_ CONST DXGKARG_ESCAPE *pEscape)
{
    PAGED_CODE();
    PVIOGPU_ESCAPE pVioGpuEscape = (PVIOGPU_ESCAPE)pEscape->pPrivateDriverData;
    NTSTATUS status = STATUS_SUCCESS;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    UINT size = pEscape->PrivateDriverDataSize;
    if (size < sizeof(PVIOGPU_ESCAPE))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s buffer too small %d, should be at least %d\n",
                  __FUNCTION__,
                  pEscape->PrivateDriverDataSize,
                  size));
        return STATUS_INVALID_BUFFER_SIZE;
    }

    switch (pVioGpuEscape->Type)
    {
        case VIOGPU_GET_DEVICE_ID:
            {
                CreateResolutionEvent();
                size = sizeof(ULONG);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }
                pVioGpuEscape->Id = m_Id;
                break;
            }
        case VIOGPU_GET_CUSTOM_RESOLUTION:
            {
                size = sizeof(VIOGPU_DISP_MODE);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }
                {
                    // Return the custom resolution of the head the caller asks for
                    // (ScanId == VidPnSourceId, which equals the scanout in practice).
                    UINT getScan = (pVioGpuEscape->ScanId < GetNumScanouts()) ? (UINT)pVioGpuEscape->ScanId : 0;
                    pVioGpuEscape->Resolution.XResolution = (USHORT)m_ModeInfo[m_CustomModeIndex[getScan]].VisScreenWidth;
                    pVioGpuEscape->Resolution.YResolution = (USHORT)m_ModeInfo[m_CustomModeIndex[getScan]].VisScreenHeight;
                }
                break;
            }
        case VIOGPU_SET_CUSTOM_RESOLUTION:
            {
                size = sizeof(VIOGPU_DISP_MODE);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }
                if (pVioGpuEscape->Resolution.XResolution <= 0 || pVioGpuEscape->Resolution.YResolution <= 0)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s PersistentDispMode0 width %d and height %d should be > 0\n",
                              __FUNCTION__,
                              pVioGpuEscape->Resolution.XResolution,
                              pVioGpuEscape->Resolution.YResolution));
                    return STATUS_INVALID_PARAMETER;
                }

                DbgPrint(TRACE_LEVEL_INFORMATION,
                         ("%s PersistentDispMode0 width %d, height %d\n",
                          __FUNCTION__,
                          pVioGpuEscape->Resolution.XResolution,
                          pVioGpuEscape->Resolution.YResolution));

                {
                    UINT setScan = (pVioGpuEscape->ScanId < GetNumScanouts()) ? (UINT)pVioGpuEscape->ScanId : 0;
                    // Only the primary head's custom resolution is persisted across boots.
                    if (setScan == 0)
                    {
                        m_pVioGpuDod->SetPersistentDispMode0Width(pVioGpuEscape->Resolution.XResolution);
                        m_pVioGpuDod->SetPersistentDispMode0Height(pVioGpuEscape->Resolution.YResolution);
                        m_pVioGpuDod->SetRegisterConfigInfo();
                    }
                    SetCustomDisplay(setScan, pVioGpuEscape->Resolution.XResolution, pVioGpuEscape->Resolution.YResolution);
                    SetCurrentModeIndex(0, GetCurrentModeIndex(0));
                }
                break;
            }
        default:
            DbgPrint(TRACE_LEVEL_ERROR, ("%s: invalid Escape type 0x%x\n", __FUNCTION__, pVioGpuEscape->Type));
            status = STATUS_INVALID_PARAMETER;
    }

    return status;
}

BOOLEAN VioGpuAdapter::GetDisplayInfo(void)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    ULONG numScanouts = GetNumScanouts();
    BOOLEAN any = FALSE;

    // Read EACH scanout's host-preferred resolution into ITS OWN custom slot.
    // Only overwrite a slot when the host actually reports a size for that
    // scanout: clobbering a head that momentarily reports "disabled" with a
    // nominal size would knock it back to 1024x768 on every config change.
    for (UINT32 i = 0; i < numScanouts; i++)
    {
        PGPU_VBUFFER vbuf = NULL;
        ULONG xres = 0;
        ULONG yres = 0;
        BOOLEAN ok = FALSE;

        if (m_CtrlQueue.AskDisplayInfo(&vbuf))
        {
            ok = m_CtrlQueue.GetDisplayInfo(vbuf, i, &xres, &yres);
            m_CtrlQueue.ReleaseBuffer(vbuf);
        }


        if (ok && xres && yres)
        {
            SetCustomDisplay(i, (USHORT)xres, (USHORT)yres);
            // Point this head's CURRENT mode index at its CUSTOM slot (the host-preferred size) as soon as we
            // learn the size — do NOT wait for a VidPN commit. Otherwise m_CurrentModeIndex[i] stays 0 for an
            // uncommitted secondary, so RecommendMonitorModes/AddSingleMonitorMode advertises mode[0] (a small
            // default) as this monitor's PREFERRED mode → Windows composes the extended layout wrong and falls
            // back to "second screen only". That never commits head i → the index stays 0 → self-reinforcing
            // random single/extend on re-extend. Seeding the correct preferred here breaks the cycle.
            SetCurrentModeIndex(i, m_CustomModeIndex[i]);
            any = TRUE;
        }

        // Hotplug (Option A): a head follows the host's pmodes[i].enabled. The
        // primary (scanout 0) stays connected (VGA/post-display, never removed);
        // secondaries connect when the host gives them a size and disconnect when
        // it drops them. Once the driver is started, tell dxgkrnl (which re-queries
        // and adds/removes the monitor); during StartDevice just latch the state so
        // the first QueryChildStatus is already correct (indicating mid-Start is
        // illegal).
        BOOLEAN wantConnected = (i == 0) ? TRUE : (ok && xres && yres);
        if (wantConnected != m_bConnected[i])
        {
            // The connection state changes ONLY through DxgkCbIndicateChildStatus, which is legal only once the
            // driver is active. So pre-active (during boot) we do NOTHING here: the state stays at its HWInit seed
            // (m_bConnected[scan] = (scan == 0)) -- primary connected, secondaries disconnected -- exactly the
            // DRM/virtio-gpu init model (index 0 enabled, index > 0 starts disabled). Sizes were already read into
            // the custom slots above. A secondary the host reports enabled is therefore delivered as a proper
            // hotplug ARRIVAL once active (TriggerInitialScan re-runs this with IsDriverActive() -> UpdateChildStatus
            // connect), which is what makes Windows EXTEND at boot instead of composing single. QEMU never
            // re-notifies (static size), so the one-shot post-start scan self-triggers that arrival.
            if (m_pVioGpuDod->IsDriverActive())
            {
                if (wantConnected && i != 0)
                {
                    // Arrival: pick up this head's REAL host EDID now that it is enabled (the host may have had only
                    // a blank block at boot -> GetEdids fell back to a copy of the primary's). Do it BEFORE the
                    // indicate so Windows reads the fresh descriptor on QueryDeviceDescriptor. No-op if still blank.
                    RefreshEdid(i);
                }
                UpdateChildStatus(i, wantConnected);   // updates m_bConnected + indicates the hotplug arrival/departure
                if (!wantConnected)
                {
                    // On disconnect, release the scanout on the host (DestroyFrameBufferObj with bReset=TRUE issues
                    // SetScanout(i,0) and drops the resource). QEMU can then destroy the scanout's DisplayChannel
                    // surface -> the server's capture skips it -> RTP goes silent (no frozen heartbeat for a head
                    // that is gone). On reconnect, CreateFrameBufferObj sets a fresh scanout. Present to this head
                    // is guarded (m_pFrameBuf[i] == NULL -> no-op).
                    DestroyFrameBufferObj(TRUE, FALSE, i);
                }
            }
            // else: pre-active boot -> keep the HWInit seed (see above); the arrival comes post-start.
        }
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return any;
}

int VioGpuAdapter::ProcessEdid(void)
{
    PAGED_CODE();

    if (virtio_is_feature_enabled(m_u64HostFeatures, VIRTIO_GPU_F_EDID))
    {
        GetEdids();
    }
    else
    {
        FixEdid();
    }

    return AddEdidModes();
}

void VioGpuAdapter::FixEdid(void)
{
    PAGED_CODE();

    UCHAR Sum = 0;
    PUCHAR buf = GetEdidData();
    ;
    PEDID_DATA_V1 pdata = (PEDID_DATA_V1)buf;
    pdata->MaximumHorizontalImageSize[0] = 0;
    pdata->MaximumVerticallImageSize[0] = 0;
    pdata->ExtensionFlag[0] = 0;
    pdata->Checksum[0] = 0;
    for (ULONG i = 0; i < EDID_V1_BLOCK_SIZE; i++)
    {
        Sum += buf[i];
    }
    pdata->Checksum[0] = -Sum;
}

BOOLEAN VioGpuAdapter::GetEdids(void)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_VBUFFER vbuf = NULL;
    ULONG numScanouts = GetNumScanouts();
    static const BYTE edid_magic[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};

    for (UINT32 i = 0; i < numScanouts; i++)
    {
        // Reset per iteration: AskEdidInfo leaves *buf untouched when it fails (e.g. response
        // allocation), so without this the ReleaseBuffer below would free the previous
        // iteration's already-freed buffer (double-free), or NULL on the first scanout.
        vbuf = NULL;
        // Store each scanout's EDID in its OWN slot: a single shared buffer would let
        // a blank secondary EDID clobber the primary's, leaving both monitors
        // identical / without modes.
        if (m_CtrlQueue.AskEdidInfo(&vbuf, i) && m_CtrlQueue.GetEdidInfo(vbuf, i, m_EDIDs[i]))
        {
            // Only accept it if the EDID header magic is present; QEMU hands a
            // real EDID to some scanouts and a blank block to the others.
            if (RtlCompareMemory(m_EDIDs[i], edid_magic, sizeof(edid_magic)) == sizeof(edid_magic))
            {
                m_bEDID[i] = TRUE;
            }
            else
            {
            }
        }
        else
        {
        }
        if (vbuf != NULL)
        {
            m_CtrlQueue.ReleaseBuffer(vbuf);
        }
    }

    // Any head that has no real EDID gets a COPY of the primary's (so it exposes
    // real native modes) but with a DISTINCT MONITOR IDENTITY, so Windows treats the
    // heads as different monitors and can compose a multi-source VidPN.
    // Nudge BOTH the product code (bytes 10-11) AND the serial (byte 12): with only the
    // serial byte differing, the two heads were near-twins and Windows restored the
    // extended arrangement NON-DETERMINISTICALLY (random "show only on X" on re-extend,
    // because it conflated the two near-identical monitors when persisting/restoring the
    // CCD config). A distinct product code makes head i a different model → stable identity.
    if (m_bEDID[0])
    {
        for (UINT32 i = 1; i < numScanouts; i++)
        {
            if (!m_bEDID[i])
            {
                RtlCopyMemory(m_EDIDs[i], m_EDIDs[0], EDID_RAW_BLOCK_SIZE);
                m_EDIDs[i][10] = (BYTE)(m_EDIDs[i][10] + i);       // product code low byte (distinct model)
                m_EDIDs[i][12] = (BYTE)(m_EDIDs[i][12] + i);       // serial byte
                m_EDIDs[i][127] = (BYTE)(m_EDIDs[i][127] - 2 * i); // keep block-0 checksum == 0 (two bytes +i)
                m_bEDID[i] = TRUE;
            }
        }
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return TRUE;
}

BOOLEAN VioGpuAdapter::RefreshEdid(UINT32 scanId)
{
    PAGED_CODE();
    // Re-read a secondary head's REAL host EDID when it is (re)enabled. At boot a not-yet-configured scanout hands
    // back a blank block, so GetEdids fell back to a copy of the primary's EDID. Once the host enables the head it
    // can provide a real EDID for that scanout -- carrying the head's own native modes and, when the server pushes
    // physical dimensions, the correct DPI. Prefer that over the copy-of-primary. Purely additive: if the host is
    // still blank we keep whatever we already have (never clobber the boot EDID with a blank).
    if (scanId == 0 || scanId >= GetNumScanouts())
    {
        return FALSE;
    }
    static const BYTE edid_magic[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    BYTE tmp[EDID_RAW_BLOCK_SIZE] = {0};
    PGPU_VBUFFER vbuf = NULL;
    BOOLEAN got = FALSE;

    BOOLEAN haveTmp = (m_CtrlQueue.AskEdidInfo(&vbuf, scanId) && m_CtrlQueue.GetEdidInfo(vbuf, scanId, tmp));
    BOOLEAN validTmp = haveTmp && RtlCompareMemory(tmp, edid_magic, sizeof(edid_magic)) == sizeof(edid_magic);
    if (!validTmp)
    {
    }
    if (validTmp)
    {
        // EDID bytes 21/22 = max horizontal/vertical image size in CM (the physical size that drives DPI). Logging
        // them tells us whether the host's per-scanout width_mm/height_mm actually reached the generated EDID.
        RtlCopyMemory(m_EDIDs[scanId], tmp, EDID_RAW_BLOCK_SIZE);
        // Distinctness safety: if a generic host hands the SAME identity block to every scanout (manufacturer +
        // product + serial, bytes 8..15, identical to the primary), Windows would conflate the two monitors
        // (the non-deterministic "show only on X"). Nudge product code + serial only in that collision case;
        // a host that already gives distinct/real EDIDs is used verbatim.
        if (m_bEDID[0] && RtlCompareMemory(&m_EDIDs[scanId][8], &m_EDIDs[0][8], 8) == 8)
        {
            m_EDIDs[scanId][10] = (BYTE)(m_EDIDs[scanId][10] + scanId);       // product code low byte
            m_EDIDs[scanId][12] = (BYTE)(m_EDIDs[scanId][12] + scanId);       // serial byte
            m_EDIDs[scanId][127] = (BYTE)(m_EDIDs[scanId][127] - 2 * scanId); // keep block-0 checksum == 0
        }
        m_bEDID[scanId] = TRUE;
        got = TRUE;
    }
    if (vbuf != NULL)
    {
        m_CtrlQueue.ReleaseBuffer(vbuf);
    }
    return got;
}

BOOLEAN VioGpuAdapter::UpdateModes(USHORT xres, USHORT yres, int &cnt)
{
    int idx = 0;

    DbgPrint(TRACE_LEVEL_INFORMATION, (" x_res: %d, y_res: %d\n", xres, yres));
    if ((xres < MIN_WIDTH_SIZE) || (yres < MIN_HEIGHT_SIZE))
    {
        return FALSE;
    }

    for (; idx < cnt; idx++)
    {
        if ((gpu_disp_modes[idx].XResolution == xres) && (gpu_disp_modes[idx].YResolution == yres))
        {
            return FALSE;
        }
    }
    gpu_disp_modes[idx].XResolution = xres;
    gpu_disp_modes[idx].YResolution = yres;
    cnt++;
    return TRUE;
}

int VioGpuAdapter::AddEdidModes(void)
{
    PAGED_CODE();
    PEDID_DATA_V1 edid_data = (PEDID_DATA_V1)(GetEdidData());
    ESTABLISHED_TIMINGS_1_2 est_timing_1_2 = edid_data->EstablishedTimings;
    MANUFACTURER_TIMINGS manufact_timing = edid_data->ManufacturerTimings;
    int modecount = 0;

    DbgPrint(TRACE_LEVEL_INFORMATION, (" Default resolutions\n"));
    UpdateModes(MIN_WIDTH_SIZE, MIN_HEIGHT_SIZE, modecount);
    UpdateModes(NOM_WIDTH_SIZE, NOM_HEIGHT_SIZE, modecount);

    DbgPrint(TRACE_LEVEL_INFORMATION, (" Processing EDID's Established timings I and II\n"));
    if (est_timing_1_2.Timing_640x480_75 || est_timing_1_2.Timing_640x480_72 || est_timing_1_2.Timing_640x480_67 ||
        est_timing_1_2.Timing_640x480_60)
    {
        UpdateModes(640, 480, modecount);
    }

    if (est_timing_1_2.Timing_800x600_60 || est_timing_1_2.Timing_800x600_56 || est_timing_1_2.Timing_800x600_75 ||
        est_timing_1_2.Timing_800x600_72)
    {
        UpdateModes(800, 600, modecount);
    }

    if (est_timing_1_2.Timing_720x400_88 || est_timing_1_2.Timing_720x400_70)
    {
        UpdateModes(720, 400, modecount);
    }

    if (est_timing_1_2.Timing_832x624_75)
    {
        UpdateModes(832, 624, modecount);
    }

    if (est_timing_1_2.Timing_1024x768_75 || est_timing_1_2.Timing_1024x768_70 || est_timing_1_2.Timing_1024x768_60 ||
        est_timing_1_2.Timing_1024x768_87)
    {
        UpdateModes(1024, 768, modecount);
    }

    if (est_timing_1_2.Timing_1280x1024_75)
    {
        UpdateModes(1280, 1024, modecount);
    }

    if (manufact_timing.Timing_1152x870_75)
    {
        UpdateModes(1152, 870, modecount);
    }

    PSTANDARD_TIMING_DESCRIPTOR standard_timing = edid_data->StandardTimings;
    DbgPrint(TRACE_LEVEL_INFORMATION, (" Processing EDID's Standard timings\n"));
    for (int i = 0; i < 8; i++, standard_timing++)
    {
        VIOGPU_DISP_MODE mode{0};
        if (GetStandardTimingResolution(standard_timing, &mode))
        {
            UpdateModes(mode.XResolution, mode.YResolution, modecount);
        }
    }

    DbgPrint(TRACE_LEVEL_INFORMATION, (" Processing EDID's detailed timings (4 18-byte blocks)\n"));
    if (edid_data->Revision[0] == 4)
    {
        PEDID_DETAILED_DESCRIPTOR detailed_desc = edid_data->EDIDDetailedTimings;
        for (int i = 0; i < 4; i++, detailed_desc++)
        {
            if (detailed_desc->PixelClock == 0)
            {
                PEDID_DISPLAY_DESCRIPTOR disp = (PEDID_DISPLAY_DESCRIPTOR)detailed_desc;
                if (disp->Tag[3] == 0xF7 && disp->Revision == 0xA)
                {
                    PESTABLISHED_TIMINGS_3 est_timing_3 = (PESTABLISHED_TIMINGS_3)disp->Data;
                    if (est_timing_3->Timing_640x350_85)
                    {
                        UpdateModes(640, 350, modecount);
                    }

                    if (est_timing_3->Timing_640x400_85)
                    {
                        UpdateModes(640, 400, modecount);
                    }

                    if (est_timing_3->Timing_640x480_85)
                    {
                        UpdateModes(640, 480, modecount);
                    }

                    if (est_timing_3->Timing_720x400_85)
                    {
                        UpdateModes(720, 400, modecount);
                    }

                    if (est_timing_3->Timing_800x600_85)
                    {
                        UpdateModes(800, 600, modecount);
                    }

                    if (est_timing_3->Timing_848x480_60)
                    {
                        UpdateModes(848, 480, modecount);
                    }

                    if (est_timing_3->Timing_1024x768_85)
                    {
                        UpdateModes(1024, 768, modecount);
                    }

                    if (est_timing_3->Timing_1152x864_75)
                    {
                        UpdateModes(1152, 864, modecount);
                    }

                    if (est_timing_3->Timing_1280x768_60 || est_timing_3->Timing_1280x768_60_RB ||
                        est_timing_3->Timing_1280x768_75 || est_timing_3->Timing_1280x768_85)
                    {
                        UpdateModes(1280, 768, modecount);
                    }

                    if (est_timing_3->Timing_1280x960_60 || est_timing_3->Timing_1280x960_85)
                    {
                        UpdateModes(1280, 960, modecount);
                    }

                    if (est_timing_3->Timing_1280x1024_60 || est_timing_3->Timing_1280x1024_85)
                    {
                        UpdateModes(1280, 1024, modecount);
                    }

                    if (est_timing_3->Timing_1360x768_60)
                    {
                        UpdateModes(1360, 768, modecount);
                    }

                    if (est_timing_3->Timing_1400x1050_60 || est_timing_3->Timing_1400x1050_60_RB ||
                        est_timing_3->Timing_1400x1050_75 || est_timing_3->Timing_1400x1050_85)
                    {
                        UpdateModes(1400, 1050, modecount);
                    }

                    if (est_timing_3->Timing_1440x900_60 || est_timing_3->Timing_1440x900_60_RB ||
                        est_timing_3->Timing_1440x900_75 || est_timing_3->Timing_1440x900_85)
                    {
                        UpdateModes(1440, 900, modecount);
                    }

                    if (est_timing_3->Timing_1600x1200_60 || est_timing_3->Timing_1600x1200_65 ||
                        est_timing_3->Timing_1600x1200_70 || est_timing_3->Timing_1600x1200_75 ||
                        est_timing_3->Timing_1600x1200_85)
                    {
                        UpdateModes(1600, 1200, modecount);
                    }

                    if (est_timing_3->Timing_1680x1050_60 || est_timing_3->Timing_1680x1050_60_RB ||
                        est_timing_3->Timing_1680x1050_75 || est_timing_3->Timing_1680x1050_85)
                    {
                        UpdateModes(1680, 1050, modecount);
                    }

                    if (est_timing_3->Timing_1792x1344_60 || est_timing_3->Timing_1792x1344_75)
                    {
                        UpdateModes(1792, 1344, modecount);
                    }

                    if (est_timing_3->Timing_1856x1392_60 || est_timing_3->Timing_1856x1392_75)
                    {
                        UpdateModes(1856, 1392, modecount);
                    }

                    if (est_timing_3->Timing_1920x1200_60 || est_timing_3->Timing_1920x1200_60_RB ||
                        est_timing_3->Timing_1920x1200_75 || est_timing_3->Timing_1920x1200_85)
                    {
                        UpdateModes(1920, 1200, modecount);
                    }

                    if (est_timing_3->Timing_1920x1440_60 || est_timing_3->Timing_1920x1440_75)
                    {
                        UpdateModes(1920, 1440, modecount);
                    }
                }
            }
        }
    }

    DbgPrint(TRACE_LEVEL_INFORMATION, (" Processing CTA861 data\n"));
    PEDID_CTA_861 cta_data = (PEDID_CTA_861)GetCTA861Data();
    if (cta_data && cta_data->DTDBegin[0] > 4)
    {
        int vics = (cta_data->DTDBegin[0] - 1) - 4;
        for (int idx = 0; idx < vics; idx++)
        {
            VIOGPU_DISP_MODE mode{0};
            USHORT vic_num = cta_data->Data[idx];
            if (GetVICResolution(vic_num, &mode))
            {
                UpdateModes(mode.XResolution, mode.YResolution, modecount);
            }
        }
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return modecount;
}

void VioGpuAdapter::SetVideoModeInfo(UINT Idx, PVIOGPU_DISP_MODE pModeInfo)
{
    PAGED_CODE();

    PVIDEO_MODE_INFORMATION pMode = NULL;

    pMode = &m_ModeInfo[Idx];
    pMode->Length = sizeof(VIDEO_MODE_INFORMATION);
    pMode->ModeIndex = Idx;
    pMode->VisScreenWidth = pModeInfo->XResolution;
    pMode->VisScreenHeight = pModeInfo->YResolution;
    pMode->ScreenStride = (pModeInfo->XResolution * 4 + 3) & ~0x3;
}

NTSTATUS VioGpuAdapter::UpdateChildStatus(UINT childUid, BOOLEAN connect)
{
    PAGED_CODE();
    NTSTATUS Status(STATUS_SUCCESS);
    DXGK_CHILD_STATUS ChildStatus;
    PDXGKRNL_INTERFACE pDXGKInterface(m_pVioGpuDod->GetDxgkInterface());

    if (childUid >= MAX_SCANOUTS)
    {
        return STATUS_INVALID_PARAMETER;
    }

    // Remember the state QueryChildStatus will report for this child, then tell
    // dxgkrnl the monitor on this target connected/disconnected.
    m_bConnected[childUid] = connect;

    RtlZeroMemory(&ChildStatus, sizeof(ChildStatus));
    ChildStatus.Type = StatusConnection;
    ChildStatus.ChildUid = childUid;
    ChildStatus.HotPlug.Connected = connect;
    Status = pDXGKInterface->DxgkCbIndicateChildStatus(pDXGKInterface->DeviceHandle, &ChildStatus);
    if (Status != STATUS_SUCCESS)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("<--- %s DxgkCbIndicateChildStatus failed with status %x\n ", __FUNCTION__, Status));
    }
    return Status;
}

void VioGpuAdapter::SetCustomDisplay(_In_ UINT scanId, _In_ USHORT xres, _In_ USHORT yres)
{
    PAGED_CODE();

    if (scanId >= MAX_SCANOUTS)
    {
        scanId = 0;
    }

    VIOGPU_DISP_MODE tmpModeInfo = {0};

    if (xres < MIN_WIDTH_SIZE || yres < MIN_HEIGHT_SIZE)
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s: (%dx%d) less than (%dx%d)\n", __FUNCTION__, xres, yres, MIN_WIDTH_SIZE, MIN_HEIGHT_SIZE));
    }
    tmpModeInfo.XResolution = m_pVioGpuDod->IsFlexResolution() ? xres : max(MIN_WIDTH_SIZE, xres);
    tmpModeInfo.YResolution = m_pVioGpuDod->IsFlexResolution() ? yres : max(MIN_HEIGHT_SIZE, yres);


    SetVideoModeInfo(m_CustomModeIndex[scanId], &tmpModeInfo);
}

NTSTATUS VioGpuAdapter::BuildModeList(DXGK_DISPLAY_INFORMATION *pDispInfo)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    delete[] m_ModeInfo;
    m_ModeInfo = NULL;
    m_ModeCount = 0;

    // Reserve one custom mode slot PER scanout at the end of the list, so each
    // head can carry its own resize target without overwriting the others.
    ULONG numScanouts = GetNumScanouts();
    m_ModeCount = ProcessEdid() + numScanouts;

    m_ModeInfo = new (PagedPool) VIDEO_MODE_INFORMATION[m_ModeCount];
    if (!m_ModeInfo)
    {
        Status = STATUS_NO_MEMORY;
        DbgPrint(TRACE_LEVEL_ERROR, ("VioGpuAdapter::GetModeList failed to allocate m_ModeInfo memory\n"));
        return Status;
    }
    RtlZeroMemory(m_ModeInfo, sizeof(VIDEO_MODE_INFORMATION) * m_ModeCount);

    SetCurrentModeIndex(0, 0);

    pDispInfo->Height = max(pDispInfo->Height, MIN_HEIGHT_SIZE);
    pDispInfo->Width = max(pDispInfo->Width, MIN_WIDTH_SIZE);
    pDispInfo->ColorFormat = D3DDDIFMT_X8R8G8B8;
    pDispInfo->Pitch = (BPPFromPixelFormat(pDispInfo->ColorFormat) / BITS_PER_BYTE) * pDispInfo->Width;

    for (USHORT indx = 0; indx < m_ModeCount - numScanouts; indx++)
    {

        PVIOGPU_DISP_MODE pModeInfo = &gpu_disp_modes[indx];

        DbgPrint(TRACE_LEVEL_INFORMATION,
                 ("%s: modes[%d] x_res = %d, y_res = %d\n",
                  __FUNCTION__,
                  indx,
                  pModeInfo->XResolution,
                  pModeInfo->YResolution));

        SetVideoModeInfo(indx, pModeInfo);
        if (pModeInfo->XResolution == NOM_WIDTH_SIZE && pModeInfo->YResolution == NOM_HEIGHT_SIZE)
        {
            SetCurrentModeIndex(0, indx);
            DbgPrint(TRACE_LEVEL_VERBOSE,
                     ("%s: modes[%d] x_res = %d, y_res = %d\n",
                      __FUNCTION__,
                      GetCurrentModeIndex(0),
                      pModeInfo->XResolution,
                      pModeInfo->YResolution));
        }
    }

    // Custom slots are the last numScanouts entries: m_CustomModeIndex[i] = base + i.
    // Seed each with a valid default so a head that never reports a host size is
    // still a usable mode (GetDisplayInfo refines it when a real size arrives).
    for (UINT i = 0; i < numScanouts; i++)
    {
        m_CustomModeIndex[i] = (USHORT)(m_ModeCount - numScanouts + i);
        SetCustomDisplay(i, NOM_WIDTH_SIZE, NOM_HEIGHT_SIZE);
    }

    DbgPrint(TRACE_LEVEL_INFORMATION, ("ModeCount filtered %d\n", m_ModeCount));

    GetDisplayInfo();

    if (m_pVioGpuDod->IsPersistentDispMode0Set())
    {
        SetCustomDisplay(0, m_pVioGpuDod->GetPersistentDispMode0Width(), m_pVioGpuDod->GetPersistentDispMode0Height());
        SetCurrentModeIndex(0, GetCurrentModeIndex(0));
    }

    for (UINT idx = 0; idx < m_ModeCount; idx++)
    {
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("index %d, XRes = %d, YRes = %d\n",
                  m_ModeInfo[idx].ModeIndex,
                  m_ModeInfo[idx].VisScreenWidth,
                  m_ModeInfo[idx].VisScreenHeight));
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}
PAGED_CODE_SEG_END

BOOLEAN VioGpuAdapter::ResetToVgaMode(void)
{
    DestroyFrameBufferObj(TRUE, TRUE, 0);
    VioGpuAdapterClose();
    return TRUE;
}

void VioGpuAdapter::DestroyFrameBufferObj(BOOLEAN bReset, BOOLEAN bKeepBuffer, UINT scanId)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s scan %d\n", __FUNCTION__, scanId));
    UINT resid = 0;

    if (m_pFrameBuf[scanId] != NULL)
    {
        resid = (UINT)m_pFrameBuf[scanId]->GetId();
        m_CtrlQueue.DetachBacking(resid);
        m_CtrlQueue.DestroyResource(resid);
        if (bReset == TRUE)
        {
            m_CtrlQueue.SetScanout(scanId, 0, 0, 0, 0, 0);
        }

        if (bKeepBuffer)
        {
            DbgPrint(TRACE_LEVEL_FATAL,
                     ("%s: Keeping frame buffer object. Don't use except in bugcheck flow!\n", __FUNCTION__));
        }
        else
        {
            delete m_pFrameBuf[scanId];
        }
        m_pFrameBuf[scanId] = NULL;
        m_Idr.PutId(resid);
    }
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

void VioGpuAdapter::VioGpuAdapterClose()
{
    DbgPrint(TRACE_LEVEL_FATAL, ("---> %s\n", __FUNCTION__));

    if (m_pVioGpuDod->IsHardwareInit())
    {
        m_pVioGpuDod->SetHardwareInit(FALSE);
        m_CtrlQueue.DisableInterrupt();
        m_CursorQueue.DisableInterrupt();
        if (KeGetCurrentIrql() < DISPATCH_LEVEL)
        {
            KeFlushQueuedDpcs();
        }
        virtio_device_reset(&m_VioDev);
        virtio_delete_queues(&m_VioDev);
        m_CtrlQueue.Close();
        m_CursorQueue.Close();
        virtio_device_shutdown(&m_VioDev);
    }
    DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s\n", __FUNCTION__));
}

BOOLEAN VioGpuAdapter::InterruptRoutine(_In_ PDXGKRNL_INTERFACE pDxgkInterface, _In_ ULONG MessageNumber)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s MessageNumber = %d\n", __FUNCTION__, MessageNumber));
    BOOLEAN serviced = TRUE;
    ULONG intReason = 0;

    if (m_PciResources.IsMSIEnabled())
    {
        switch (MessageNumber)
        {
            case 0:
                intReason = ISR_REASON_CHANGE;
                break;
            case 1:
                intReason = ISR_REASON_DISPLAY;
                break;
            case 2:
                intReason = ISR_REASON_CURSOR;
                break;
            default:
                serviced = FALSE;
                DbgPrint(TRACE_LEVEL_FATAL,
                         ("---> %s Unknown Interrupt Reason MessageNumber%d\n", __FUNCTION__, MessageNumber));
        }
    }
    else
    {
        UNREFERENCED_PARAMETER(MessageNumber);
        UCHAR isrstat = virtio_read_isr_status(&m_VioDev);

        switch (isrstat)
        {
            case 1:
                intReason = (ISR_REASON_DISPLAY | ISR_REASON_CURSOR);
                break;
            case 3:
                intReason = ISR_REASON_CHANGE;
                break;
            default:
                serviced = FALSE;
        }
    }

    if (serviced)
    {
        if (m_pVioGpuDod->IsUsePresentProgress() && (intReason & ISR_REASON_DISPLAY) == ISR_REASON_DISPLAY)
        {
            DXGKARGCB_NOTIFY_INTERRUPT_DATA NotifyInterrupt = {};
            NotifyInterrupt.InterruptType = DXGK_INTERRUPT_DISPLAYONLY_PRESENT_PROGRESS;
            NotifyInterrupt.DisplayOnlyPresentProgress.VidPnSourceId = 0;

            NotifyInterrupt.DisplayOnlyPresentProgress.ProgressId = DXGK_PRESENT_DISPLAYONLY_PROGRESS_ID_COMPLETE;
            pDxgkInterface->DxgkCbNotifyInterrupt(pDxgkInterface->DeviceHandle, &NotifyInterrupt);
        }

        InterlockedOr((PLONG)&m_PendingWorks, intReason);
        pDxgkInterface->DxgkCbQueueDpc(pDxgkInterface->DeviceHandle);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return serviced;
}

void VioGpuAdapter::ThreadWork(_In_ PVOID Context)
{
    VioGpuAdapter *pdev = reinterpret_cast<VioGpuAdapter *>(Context);
    pdev->ThreadWorkRoutine();
}

void VioGpuAdapter::TriggerInitialScan(void)
{
    // One-shot, DETERMINISTIC: called by VioGpuDod::CommitVidPn the first time Windows commits a boot topology.
    // No timer (timers are non-deterministic and can fire in unexpected states) -- this is tied to a real dxgk
    // event. It only SIGNALS the worker; the actual GetDisplayInfo / DxgkCbIndicateChildStatus runs there
    // (PASSIVE_LEVEL), never re-entrantly from inside the CommitVidPn DDI. m_bInitialScanArmed makes it fire once.
    // CommitVidPn calls are serialized by dxgkrnl, so the plain BOOLEAN guard needs no interlock.
    if (!m_bInitialScanArmed)
    {
        m_bInitialScanArmed = TRUE;
        InterlockedExchange(&m_InitialScanPending, 1);
        KeSetEvent(&m_ConfigUpdateEvent, IO_NO_INCREMENT, FALSE);
    }
}

void VioGpuAdapter::ThreadWorkRoutine(void)
{
    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);

    for (;;)
    {
        KeWaitForSingleObject(&m_ConfigUpdateEvent, Executive, KernelMode, FALSE, NULL);

        if (m_bStopWorkThread)
        {
            PsTerminateSystemThread(STATUS_SUCCESS);
            break;
        }
        if (InterlockedExchange(&m_InitialScanPending, 0))
        {
            // One-shot post-start scan (see TriggerInitialScan): now that the driver is active, re-read the host display
            // state so any secondary the host enabled at boot -- kept disconnected at the HWInit seed until now --
            // is indicated as a hotplug ARRIVAL -> Windows extends. (Matches the DRM model: index>0 starts disabled
            // and comes up via the display event.)
            GetDisplayInfo();
        }
        ConfigChanged();
        NotifyResolutionEvent();
    }
}

void VioGpuAdapter::ConfigChanged(void)
{
    DbgPrint(TRACE_LEVEL_FATAL, ("<--> %s\n", __FUNCTION__));
    UINT32 events_read, events_clear = 0;
    virtio_get_config(&m_VioDev, FIELD_OFFSET(GPU_CONFIG, events_read), &events_read, sizeof(m_u32NumScanouts));
    if (events_read & VIRTIO_GPU_EVENT_DISPLAY)
    {
        GetDisplayInfo();
        events_clear |= VIRTIO_GPU_EVENT_DISPLAY;
        virtio_set_config(&m_VioDev, FIELD_OFFSET(GPU_CONFIG, events_clear), &events_clear, sizeof(m_u32NumScanouts));
        // GetDisplayInfo() above now drives per-scanout hotplug (connect/disconnect
        // from pmodes[i].enabled) via UpdateChildStatus — nothing else to do here.
    }
}

VOID VioGpuAdapter::DpcRoutine(_In_ PDXGKRNL_INTERFACE pDxgkInterface)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    UNREFERENCED_PARAMETER(pDxgkInterface);
    PGPU_VBUFFER pvbuf = NULL;
    UINT len = 0;
    ULONG reason;
    while ((reason = InterlockedExchange((PLONG)&m_PendingWorks, 0)) != 0)
    {
        if ((reason & ISR_REASON_DISPLAY))
        {
            while ((pvbuf = m_CtrlQueue.DequeueBuffer(&len)) != NULL)
            {
                DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s m_CtrlQueue pvbuf = %p len = %d\n", __FUNCTION__, pvbuf, len));
                PGPU_CTRL_HDR pcmd = (PGPU_CTRL_HDR)pvbuf->buf;
                PGPU_CTRL_HDR resp = (PGPU_CTRL_HDR)pvbuf->resp_buf;

                if (resp->type >= VIRTIO_GPU_RESP_ERR_UNSPEC)
                {
                    DbgPrint(TRACE_LEVEL_FATAL, ("!!!!! Command failed resp=%d (0x%x) cmd=0x%x", resp->type, resp->type, pcmd->type));
                }
                if (resp->type != VIRTIO_GPU_RESP_OK_NODATA)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("<--- %s type = %xlu flags = %lu fence_id = %llu ctx_id = %lu cmd_type = %lu\n",
                              __FUNCTION__,
                              resp->type,
                              resp->flags,
                              resp->fence_id,
                              resp->ctx_id,
                              pcmd->type));
                }
                if (pvbuf->complete_cb != NULL)
                {
                    pvbuf->complete_cb(pvbuf->complete_ctx);
                }
                if (pvbuf->auto_release)
                {
                    m_CtrlQueue.ReleaseBuffer(pvbuf);
                }
            };
        }
        if ((reason & ISR_REASON_CURSOR))
        {
            while ((pvbuf = m_CursorQueue.DequeueCursor(&len)) != NULL)
            {
                DbgPrint(TRACE_LEVEL_VERBOSE,
                         ("---> %s m_CursorQueue pvbuf = %p len = %u\n", __FUNCTION__, pvbuf, len));
                m_CursorQueue.ReleaseBuffer(pvbuf);
            };
        }
        if (reason & ISR_REASON_CHANGE)
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("---> %s ConfigChanged\n", __FUNCTION__));
            KeSetEvent(&m_ConfigUpdateEvent, IO_NO_INCREMENT, FALSE);
        }
    }
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

VOID VioGpuAdapter::ResetDevice(VOID)
{
    DbgPrint(TRACE_LEVEL_INFORMATION, ("---> %s\n", __FUNCTION__));
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

UINT ColorFormat(UINT format)
{
    switch (format)
    {
        case D3DDDIFMT_A8R8G8B8:
            return VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
        case D3DDDIFMT_X8R8G8B8:
            return VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
        case D3DDDIFMT_A8B8G8R8:
            return VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM;
        case D3DDDIFMT_X8B8G8R8:
            return VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM;
    }
    DbgPrint(TRACE_LEVEL_ERROR, ("---> %s Unsupported color format %d\n", __FUNCTION__, format));
    return VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
}

PAGED_CODE_SEG_BEGIN
BOOLEAN VioGpuAdapter::CreateFrameBufferObj(PVIDEO_MODE_INFORMATION pModeInfo, CURRENT_MODE *pCurrentMode, UINT scanId)
{
    UINT resid, format, size;
    VioGpuObj *obj;
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("---> %s - %d: scan %d (%d x %d)\n", __FUNCTION__, m_Id, scanId, pModeInfo->VisScreenWidth, pModeInfo->VisScreenHeight));
    ASSERT(m_pFrameBuf[scanId] == NULL);
    size = pModeInfo->ScreenStride * pModeInfo->VisScreenHeight;
    format = ColorFormat(pCurrentMode->DispInfo.ColorFormat);
    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("---> %s - (%d -> %d)\n", __FUNCTION__, pCurrentMode->DispInfo.ColorFormat, format));
    resid = m_Idr.GetId();
    m_CtrlQueue.CreateResource(resid, format, pModeInfo->VisScreenWidth, pModeInfo->VisScreenHeight);
    obj = new (NonPagedPoolNx) VioGpuObj();
    if (!obj->Init(size, &m_FrameSegment[scanId]))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s Failed to init obj size = %d\n", __FUNCTION__, size));
        m_CtrlQueue.DestroyResource(resid);
        m_Idr.PutId(resid);
        delete obj;
        return FALSE;
    }

    GpuObjectAttach(resid, obj);
    m_CtrlQueue.SetScanout(scanId, resid, pModeInfo->VisScreenWidth, pModeInfo->VisScreenHeight, 0, 0);
    m_CtrlQueue.TransferToHost2D(resid, 0, pModeInfo->VisScreenWidth, pModeInfo->VisScreenHeight, 0, 0);
    m_CtrlQueue.ResFlush(resid, pModeInfo->VisScreenWidth, pModeInfo->VisScreenHeight, 0, 0);
    m_pFrameBuf[scanId] = obj;
    pCurrentMode->FrameBuffer = obj->GetVirtualAddress();
    pCurrentMode->Flags.FrameBufferIsActive = TRUE;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return TRUE;
}

BOOLEAN VioGpuAdapter::CreateCursor(_In_ CONST DXGKARG_SETPOINTERSHAPE *pSetPointerShape,
                                    _In_ CONST CURRENT_MODE *pCurrentMode)
{
    UINT resid, format, size;
    VioGpuObj *obj;
    BOOLEAN status = TRUE;
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("---> %s - %d: (%d x %d - %d) (%d + %d)\n",
              __FUNCTION__,
              m_Id,
              pSetPointerShape->Width,
              pSetPointerShape->Height,
              pSetPointerShape->Pitch,
              pSetPointerShape->XHot,
              pSetPointerShape->YHot));

    size = POINTER_SIZE * POINTER_SIZE * 4;
    format = ColorFormat(D3DDDIFMT_A8R8G8B8);
    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("---> %s - (%x -> %x)\n", __FUNCTION__, pCurrentMode->DispInfo.ColorFormat, format));
    resid = (UINT)m_Idr.GetId();
    m_CtrlQueue.CreateResource(resid, format, POINTER_SIZE, POINTER_SIZE);
    obj = new (NonPagedPoolNx) VioGpuObj();
    if (!obj->Init(size, &m_CursorSegment))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s Failed to init obj size = %d\n", __FUNCTION__, size));
        status = FALSE;
    }
    else if (!GpuObjectAttach(resid, obj))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s Failed to attach gpu object\n", __FUNCTION__));
        status = FALSE;
    }
    if (status)
    {
        m_pCursorBuf = obj;
    }
    else
    {
        VioGpuDbgBreak();
        m_CtrlQueue.DestroyResource(resid);
        m_Idr.PutId(resid);
        delete obj;
    }
    return status;
}

// Converts a Windows monochrome pointer to A8R8G8B8 in the cursor resource.
// Source = 1bpp AND mask (Width x Height) immediately followed by the 1bpp XOR mask.
// AND/XOR semantics: 0/0 = opaque black, 0/1 = opaque white, 1/0 = transparent,
// 1/1 = invert-screen (unsupported on virtio-gpu -> rendered as opaque black).
static void ConvertMonochromeToBgra(_In_ CONST DXGKARG_SETPOINTERSHAPE *pSetPointerShape, BYTE *dst, ULONG dstPitch)
{
    const BYTE *src = (const BYTE *)pSetPointerShape->pPixels;
    const ULONG srcPitch = pSetPointerShape->Pitch;
    UINT w = pSetPointerShape->Width;
    UINT h = pSetPointerShape->Height;
    if (w > POINTER_SIZE)
        w = POINTER_SIZE;
    if (h > POINTER_SIZE)
        h = POINTER_SIZE;

    for (UINT y = 0; y < h; y++)
    {
        const BYTE *andRow = src + (size_t)y * srcPitch;
        const BYTE *xorRow = src + (size_t)(pSetPointerShape->Height + y) * srcPitch;
        ULONG *dstRow = (ULONG *)(dst + (size_t)y * dstPitch);
        for (UINT x = 0; x < w; x++)
        {
            BYTE bit = (BYTE)(0x80 >> (x & 7));
            BOOLEAN a = (andRow[x >> 3] & bit) != 0;
            BOOLEAN xo = (xorRow[x >> 3] & bit) != 0;
            if (!a && !xo)
                dstRow[x] = 0xFF000000; // opaque black
            else if (!a && xo)
                dstRow[x] = 0xFFFFFFFF; // opaque white
            else if (a && !xo)
                dstRow[x] = 0x00000000; // transparent
            else
                dstRow[x] = 0xFF000000; // invert -> opaque black (best effort)
        }
    }
}

// Masked-color pointer: src = Width x Height of 32bpp BGRA where the alpha byte acts as the
// AND mask (0x00 = opaque color, 0xFF = XOR/transparent). virtio-gpu can't XOR, so:
// A==0 -> opaque RGB, A==0xFF & RGB==0 -> transparent, A==0xFF & RGB!=0 -> opaque RGB (best effort).
static void ConvertMaskedColorToBgra(_In_ CONST DXGKARG_SETPOINTERSHAPE *pSetPointerShape, BYTE *dst, ULONG dstPitch)
{
    const BYTE *src = (const BYTE *)pSetPointerShape->pPixels;
    const ULONG srcPitch = pSetPointerShape->Pitch;
    UINT w = pSetPointerShape->Width;
    UINT h = pSetPointerShape->Height;
    if (w > POINTER_SIZE)
        w = POINTER_SIZE;
    if (h > POINTER_SIZE)
        h = POINTER_SIZE;

    for (UINT y = 0; y < h; y++)
    {
        const ULONG *srcRow = (const ULONG *)(src + (size_t)y * srcPitch);
        ULONG *dstRow = (ULONG *)(dst + (size_t)y * dstPitch);
        for (UINT x = 0; x < w; x++)
        {
            ULONG px = srcRow[x];
            ULONG rgb = px & 0x00FFFFFF;
            BYTE a = (BYTE)(px >> 24);
            if (a == 0)
                dstRow[x] = 0xFF000000 | rgb; // opaque color
            else if (rgb == 0)
                dstRow[x] = 0x00000000; // transparent
            else
                dstRow[x] = 0xFF000000 | rgb; // invert -> opaque color (best effort)
        }
    }
}

BOOLEAN VioGpuAdapter::UpdateCursor(_In_ CONST DXGKARG_SETPOINTERSHAPE *pSetPointerShape,
                                    _In_ CONST CURRENT_MODE *pCurrentMode)
{
    PAGED_CODE();
    RECT Rect;
    Rect.left = 0;
    Rect.top = 0;
    Rect.right = Rect.left + pSetPointerShape->Width;
    Rect.bottom = Rect.top + pSetPointerShape->Height;

    if ((m_pCursorBuf == NULL) && !CreateCursor(pSetPointerShape, pCurrentMode))
    {
        VioGpuDbgBreak();
        DbgPrint(TRACE_LEVEL_ERROR, ("<--- %s Cannot create cursor\n", __FUNCTION__));
        return FALSE;
    }

    BLT_INFO DstBltInfo;
    DstBltInfo.pBits = m_pCursorBuf->GetVirtualAddress();
    DstBltInfo.Pitch = POINTER_SIZE * 4;
    DstBltInfo.BitsPerPel = BPPFromPixelFormat(D3DDDIFMT_A8R8G8B8);
    DstBltInfo.Offset.x = 0;
    DstBltInfo.Offset.y = 0;
    DstBltInfo.Rotation = D3DKMDT_VPPR_IDENTITY;
    DstBltInfo.Width = POINTER_SIZE;
    DstBltInfo.Height = POINTER_SIZE;

    if (pSetPointerShape->Flags.Color)
    {
        BLT_INFO SrcBltInfo;
        SrcBltInfo.pBits = (PVOID)pSetPointerShape->pPixels;
        SrcBltInfo.Pitch = pSetPointerShape->Pitch;
        SrcBltInfo.BitsPerPel = BPPFromPixelFormat(D3DDDIFMT_A8R8G8B8);
        SrcBltInfo.Offset.x = 0;
        SrcBltInfo.Offset.y = 0;
        SrcBltInfo.Rotation = pCurrentMode->Rotation;
        SrcBltInfo.Width = pSetPointerShape->Width;
        SrcBltInfo.Height = pSetPointerShape->Height;
        BltBits(&DstBltInfo, &SrcBltInfo, &Rect);
    }
    else if (pSetPointerShape->Flags.Monochrome)
    {
        // Monochrome cursor (e.g. the text I-beam). Without this, SetPointerShape would fail
        // for mono shapes and Windows would fall back to a SOFTWARE cursor composited into the
        // framebuffer -> captured into the video/remote stream and laggy (and shown alongside
        // the client's own cursor). Convert the 1bpp AND/XOR masks to A8R8G8B8 and push them
        // through the same hardware-cursor path.
        RtlZeroMemory(DstBltInfo.pBits, (size_t)DstBltInfo.Pitch * POINTER_SIZE);
        ConvertMonochromeToBgra(pSetPointerShape, (BYTE *)DstBltInfo.pBits, DstBltInfo.Pitch);
    }
    else if (pSetPointerShape->Flags.MaskedColor)
    {
        // Masked-color cursor (some text/I-beam cursors come this way). Same rationale as the
        // monochrome case: otherwise Windows software-renders it into the framebuffer where it
        // gets captured into the video/remote stream (laggy + double with the client cursor).
        RtlZeroMemory(DstBltInfo.pBits, (size_t)DstBltInfo.Pitch * POINTER_SIZE);
        ConvertMaskedColorToBgra(pSetPointerShape, (BYTE *)DstBltInfo.pBits, DstBltInfo.Pitch);
    }
    else
    {
        VioGpuDbgBreak();
        DbgPrint(TRACE_LEVEL_ERROR, ("<--- %s Invalid cursor flags %d\n", __FUNCTION__, pSetPointerShape->Flags.Value));
        return FALSE;
    }

    // Wait for the transfer to complete before SetPointerShape() issues the
    // UPDATE_CURSOR command on the separate cursor queue, otherwise the device
    // may show a stale cursor image (issue #977).
    m_CtrlQueue.TransferToHost2D(m_pCursorBuf->GetId(), 0, pSetPointerShape->Width, pSetPointerShape->Height, 0, 0, TRUE);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return TRUE;
}

void VioGpuAdapter::DestroyCursor()
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    if (m_pCursorBuf != NULL)
    {
        UINT id = (UINT)m_pCursorBuf->GetId();
        m_CtrlQueue.DetachBacking(id);
        m_CtrlQueue.DestroyResource(id);
        delete m_pCursorBuf;
        m_pCursorBuf = NULL;
        m_Idr.PutId(id);
    }
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

BOOLEAN VioGpuAdapter::GpuObjectAttach(UINT res_id, VioGpuObj *obj)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    PGPU_MEM_ENTRY ents = NULL;
    PSCATTER_GATHER_LIST sgl = NULL;
    UINT size = 0;
    sgl = obj->GetSGList();
    size = sizeof(GPU_MEM_ENTRY) * sgl->NumberOfElements;
    ents = reinterpret_cast<PGPU_MEM_ENTRY>(new (NonPagedPoolNx) BYTE[size]);

    if (!ents)
    {
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("<--- %s cannot allocate memory %x bytes numberofentries = %d\n",
                  __FUNCTION__,
                  size,
                  sgl->NumberOfElements));
        return FALSE;
    }
    // FIXME
    RtlZeroMemory(ents, size);

    for (UINT i = 0; i < sgl->NumberOfElements; i++)
    {
        ents[i].addr = sgl->Elements[i].Address.QuadPart;
        ents[i].length = sgl->Elements[i].Length;
        ents[i].padding = 0;
    }

    m_CtrlQueue.AttachBacking(res_id, ents, sgl->NumberOfElements);
    obj->SetId(res_id);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return TRUE;
}
PAGED_CODE_SEG_END

PDXGKRNL_INTERFACE VioGpuAdapter::GetDxgkInterface()
{
    return m_pVioGpuDod->GetDxgkInterface();
}
