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

#include "viogpu_idr.h"
#include "viogpu.h"
#include "baseobj.h"
#if !DBG
#include "viogpu_idr.tmh"
#endif

VioGpuIdr::VioGpuIdr()
{
    m_nextId = 0;
    KeInitializeSpinLock(&m_lock);
    InitializeListHead(&m_freeList);
}

VioGpuIdr::~VioGpuIdr()
{
    Close();
}

BOOLEAN VioGpuIdr::Init(_In_ ULONG start)
{
    Close();
    m_nextId = start;

    return true;
}

ULONG VioGpuIdr::GetId(VOID)
{
    // Allocate resource ids MONOTONICALLY and never reuse them. Reusing a just-freed id made QEMU reject the
    // recycled resource: on a dual VidPN commit head 0 destroys resource N and head 1 immediately re-creates
    // resource N (same id off the free list). QEMU cannot cleanly destroy-then-recreate the same id back-to-back,
    // so it kept the old (wrong-sized) resource, and every SET_SCANOUT / TRANSFER_TO_HOST_2D to it failed with
    // VIRTIO_GPU_RESP_ERR_UNSPEC (0x1200) — the "occasional 0x1200 on every resolution". A fresh id per resource
    // sidesteps it entirely; 32-bit ids do not exhaust within a driver session (they reset on driver reload).
    ULONG id = (ULONG)InterlockedExchangeAdd((volatile LONG *)&m_nextId, 1);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("[%s] id = %d\n", __FUNCTION__, id));
    return id;
}

VOID VioGpuIdr::PutId(_In_ ULONG id)
{
    // No-op: ids are never recycled (see GetId) — recycling caused the destroy-recreate-same-id 0x1200 hazard.
    UNREFERENCED_PARAMETER(id);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("[%s] id = %d (not recycled)\n", __FUNCTION__, id));
}

VOID VioGpuIdr::Close(VOID)
{

    FreeId *freeId = NULL;
    do
    {
        freeId = reinterpret_cast<FreeId *>(ExInterlockedRemoveHeadList(&m_freeList, &m_lock));
        if (freeId != NULL)
        {
            delete freeId;
        }
    } while (freeId != NULL);
}
