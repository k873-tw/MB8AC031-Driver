// IBonDriver.h -- the BonDriver interface used by TVTest, EDCB and others
// (IBonDriver / IBonDriver2), declared for this driver.
//
// SPDX-License-Identifier: MIT
//
// BonDriver is an ABI: a DLL exports CreateBonDriver(), which returns an
// object whose vtable the host (TVTest, EDCB, ...) calls by slot.  What has
// to match is the order of the slots and the calling convention, so that
// is all this file pins down; the declarations are written from that ABI,
// not copied from any SDK header.
//
// THE OVERLOADED GetTsStream.  The ABI was laid down by MSVC, which puts a
// class's overloaded virtuals next to each other in REVERSE declaration
// order.  The slot order the host expects is therefore
//     ..., GetReadyCount, GetTsStream(BYTE **...), GetTsStream(BYTE *...),
//     PurgeTsStream, Release
// MSVC gets there from the conventional declaration order; GCC/Clang
// (Itanium-style vtables, declaration order) need the two swapped.
#ifndef MB8_IBONDRIVER_H
#define MB8_IBONDRIVER_H

#include <windows.h>

class IBonDriver {
public:
    virtual const BOOL OpenTuner(void) = 0;
    virtual void CloseTuner(void) = 0;
    virtual const BOOL SetChannel(const BYTE channel) = 0;
    virtual const float GetSignalLevel(void) = 0;
    virtual const DWORD WaitTsStream(const DWORD timeoutMs = 0) = 0;
    virtual const DWORD GetReadyCount(void) = 0;
#if defined(_MSC_VER)
    virtual const BOOL GetTsStream(BYTE *dst, DWORD *size,
                                   DWORD *remaining) = 0;
    virtual const BOOL GetTsStream(BYTE **dst, DWORD *size,
                                   DWORD *remaining) = 0;
#else
    virtual const BOOL GetTsStream(BYTE **dst, DWORD *size,
                                   DWORD *remaining) = 0;
    virtual const BOOL GetTsStream(BYTE *dst, DWORD *size,
                                   DWORD *remaining) = 0;
#endif
    virtual void PurgeTsStream(void) = 0;
    virtual void Release(void) = 0;
};

class IBonDriver2 : public IBonDriver {
public:
    virtual LPCTSTR GetTunerName(void) = 0;
    virtual const BOOL IsTunerOpening(void) = 0;
    virtual LPCTSTR EnumTuningSpace(const DWORD space) = 0;
    virtual LPCTSTR EnumChannelName(const DWORD space,
                                    const DWORD channel) = 0;
    virtual const BOOL SetChannel(const DWORD space,
                                  const DWORD channel) = 0;
    virtual const DWORD GetCurSpace(void) = 0;
    virtual const DWORD GetCurChannel(void) = 0;
};

#endif
