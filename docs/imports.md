# Xbox 360 imports required by the recompiled code

These are the 164 guest imports the generated `ppc\` code calls but does not define:
the complete list of symbols a host runtime must provide before the code will link.
Each must be implemented as `PPC_FUNC(__imp__<Name>)` (the trial link reports them as
`void __imp__<Name>(PPCContext&, uint8_t*)`).

Source: trial link of all generated code (2026-10-20, XenonRecomp `pacman-instructions` @ fdc35ee)
against the CRT with a stub `main`. No other symbols were unresolved: every recompiled function and
save/restore helper is present, and host maths symbols (`sqrt`, `sqrtf`, `exp2f`, `log2f`, `_fltused`)
come from the CRT. Regenerate this list after any recompile that changes imports.

`sprintf`, `_vsnprintf`, `__C_specific_handler` and similar are listed because xboxkrnl exports
them; the game calls the kernel's versions, not the host CRT.

| Subsystem | Count |
|---|---|
| XAM (user, content, UI, notifications, settings) | 41 |
| Ke/Kf/Ki (kernel scheduling, events, DPC) | 30 |
| Vd (video/GPU) | 20 |
| Rtl (runtime: strings, critsec, unwind) | 17 |
| Nt (files, handles, waits) | 17 |
| Ob/Ex/Hal/Io | 9 |
| Xe/Xex (crypto, module info) | 8 |
| CRT-like kernel exports | 8 |
| Audio (XAudio, XMA) | 7 |
| Mm (memory) | 5 |
| Net | 2 |
| **Total** | **164** |

## Audio (XAudio, XMA)

- `XAudioGetVoiceCategoryVolume`
- `XAudioGetVoiceCategoryVolumeChangeMask`
- `XAudioRegisterRenderDriverClient`
- `XAudioSubmitRenderDriverFrame`
- `XAudioUnregisterRenderDriverClient`
- `XMACreateContext`
- `XMAReleaseContext`

## CRT-like kernel exports

- `DbgPrint`
- `__C_specific_handler`
- `_snwprintf`
- `_vscwprintf`
- `_vsnprintf`
- `sprintf`
- `swprintf`
- `vswprintf`

## Ke/Kf/Ki (kernel scheduling, events, DPC)

- `KeAcquireSpinLockAtRaisedIrql`
- `KeBugCheck`
- `KeBugCheckEx`
- `KeDelayExecutionThread`
- `KeEnterCriticalRegion`
- `KeGetCurrentProcessType`
- `KeInitializeSemaphore`
- `KeLeaveCriticalRegion`
- `KeLockL2`
- `KeQueryPerformanceFrequency`
- `KeQuerySystemTime`
- `KeRaiseIrqlToDpcLevel`
- `KeReleaseSemaphore`
- `KeReleaseSpinLockFromRaisedIrql`
- `KeResetEvent`
- `KeResumeThread`
- `KeSetAffinityThread`
- `KeSetBasePriorityThread`
- `KeSetEvent`
- `KeTlsAlloc`
- `KeTlsFree`
- `KeTlsGetValue`
- `KeTlsSetValue`
- `KeUnlockL2`
- `KeWaitForMultipleObjects`
- `KeWaitForSingleObject`
- `KfAcquireSpinLock`
- `KfLowerIrql`
- `KfReleaseSpinLock`
- `KiApcNormalRoutineNop`

## Mm (memory)

- `MmAllocatePhysicalMemoryEx`
- `MmFreePhysicalMemory`
- `MmGetPhysicalAddress`
- `MmQueryAddressProtect`
- `MmQueryAllocationSize`

## Net

- `NetDll_WSACleanup`
- `NetDll_WSAStartup`

## Nt (files, handles, waits)

- `NtAllocateVirtualMemory`
- `NtClearEvent`
- `NtClose`
- `NtCreateEvent`
- `NtCreateFile`
- `NtFlushBuffersFile`
- `NtFreeVirtualMemory`
- `NtOpenFile`
- `NtQueryFullAttributesFile`
- `NtQueryInformationFile`
- `NtQueryVirtualMemory`
- `NtQueryVolumeInformationFile`
- `NtReadFile`
- `NtResumeThread`
- `NtSetInformationFile`
- `NtWaitForSingleObjectEx`
- `NtWriteFile`

## Ob/Ex/Hal/Io

- `ExCreateThread`
- `ExGetXConfigSetting`
- `ExRegisterTitleTerminateNotification`
- `ExTerminateThread`
- `HalReturnToFirmware`
- `ObCreateSymbolicLink`
- `ObDeleteSymbolicLink`
- `ObDereferenceObject`
- `ObReferenceObjectByHandle`

## Rtl (runtime: strings, critsec, unwind)

- `RtlCaptureContext`
- `RtlCompareMemoryUlong`
- `RtlEnterCriticalSection`
- `RtlFillMemoryUlong`
- `RtlFreeAnsiString`
- `RtlImageXexHeaderField`
- `RtlInitAnsiString`
- `RtlInitUnicodeString`
- `RtlInitializeCriticalSection`
- `RtlLeaveCriticalSection`
- `RtlMultiByteToUnicodeN`
- `RtlNtStatusToDosError`
- `RtlRaiseException`
- `RtlTimeToTimeFields`
- `RtlUnicodeStringToAnsiString`
- `RtlUnicodeToMultiByteN`
- `RtlUnwind`

## Vd (video/GPU)

- `VdCallGraphicsNotificationRoutines`
- `VdEnableDisableClockGating`
- `VdEnableRingBufferRPtrWriteBack`
- `VdGetCurrentDisplayGamma`
- `VdGetCurrentDisplayInformation`
- `VdGetSystemCommandBuffer`
- `VdInitializeEngines`
- `VdInitializeRingBuffer`
- `VdInitializeScalerCommandBuffer`
- `VdIsHSIOTrainingSucceeded`
- `VdPersistDisplay`
- `VdQueryVideoFlags`
- `VdQueryVideoMode`
- `VdRetrainEDRAM`
- `VdRetrainEDRAMWorker`
- `VdSetDisplayMode`
- `VdSetGraphicsInterruptCallback`
- `VdSetSystemCommandBufferGpuIdentifierAddress`
- `VdShutdownEngines`
- `VdSwap`

## XAM (user, content, UI, notifications, settings)

- `XGetAVPack`
- `XGetGameRegion`
- `XGetLanguage`
- `XGetVideoMode`
- `XMsgInProcessCall`
- `XMsgStartIORequest`
- `XNotifyGetNext`
- `XNotifyPositionUI`
- `XamAlloc`
- `XamContentClose`
- `XamContentCreateEnumerator`
- `XamContentCreateEx`
- `XamContentGetCreator`
- `XamContentGetDeviceState`
- `XamContentGetLicenseMask`
- `XamContentSetThumbnail`
- `XamEnumerate`
- `XamFree`
- `XamGetExecutionId`
- `XamGetSystemVersion`
- `XamInputGetCapabilities`
- `XamInputGetKeystrokeEx`
- `XamInputGetState`
- `XamLoaderLaunchTitle`
- `XamLoaderTerminateTitle`
- `XamNotifyCreateListener`
- `XamSessionCreateHandle`
- `XamSessionRefObjByHandle`
- `XamShowAchievementsUI`
- `XamShowDeviceSelectorUI`
- `XamShowGamerCardUIForXUID`
- `XamShowMarketplaceUI`
- `XamShowMessageBoxUI`
- `XamShowMessageBoxUIEx`
- `XamShowSigninUI`
- `XamUserCreateAchievementEnumerator`
- `XamUserCreateStatsEnumerator`
- `XamUserGetName`
- `XamUserGetSigninState`
- `XamUserGetXUID`
- `XamWriteGamerTile`

## Xe/Xex (crypto, module info)

- `XeCryptBnQwBeSigVerify`
- `XeCryptRotSumSha`
- `XeCryptSha`
- `XeKeysGetKey`
- `XexCheckExecutablePrivilege`
- `XexGetModuleHandle`
- `XexGetModuleSection`
- `XexGetProcedureAddress`
