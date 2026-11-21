/* smx_drv.c - SMX kernel transport.
 *
 * One private device serving request types against a target process:
 * memory read/write, PEB and module base, region query, batched reads,
 * remote allocation, thread context, and process masking.
 *
 * Layout
 *   smx.h        wire contract: names, request numbers, structures
 *   smx_drv.c    device lifecycle, dispatch, memory helpers   (this file)
 *
 * Loading
 *   DriverEntry launches a single system thread and returns. Object and
 *   registry work happens on that thread at PASSIVE_LEVEL. This separates
 *   initialization from the mapper-supplied entry context, which differs
 *   from a normal driver load.
 *
 * Compatibility
 *   smx.h must stay byte-compatible with the client transport in
 *   2_src_port/rr_mem.c. Mismatches can cause rejected requests or incorrectly
 *   interpreted fields; a successful call alone does not prove compatibility.
 *
 * IMPORTANT
 *   SMX_IO_HIDE performs DKOM on ActiveProcessLinks. PatchGuard will
 *   detect this within tens of minutes and bugcheck 0x109. The mutex below
 *   only serializes against our own mutations; it cannot hide the change
 *   from KPP. Define SMX_DISABLE_DKOM to make SMX_IO_HIDE return
 *   STATUS_NOT_IMPLEMENTED so the rest of the driver can be used safely.
 */
#ifndef SMX_DISABLE_DKOM
#define SMX_DISABLE_DKOM
#endif

#include <ntddk.h>

NTKERNELAPI
PPEB
PsGetProcessPeb(
    PEPROCESS Process
);

NTKERNELAPI
NTSTATUS
PsLookupProcessByProcessId(
    HANDLE ProcessId,
    PEPROCESS* Process
);

NTKERNELAPI
NTSTATUS
MmCopyVirtualMemory(
    PEPROCESS SourceProcess,
    PVOID SourceAddress,
    PEPROCESS TargetProcess,
    PVOID TargetAddress,
    SIZE_T BufferSize,
    KPROCESSOR_MODE PreviousMode,
    PSIZE_T ReturnSize
);

NTKERNELAPI
NTSTATUS
IoCreateDriver(
    PUNICODE_STRING DriverName,
    PDRIVER_INITIALIZE InitializationFunction
);

#ifndef PROCESS_VM_OPERATION
#define PROCESS_VM_OPERATION 0x0008
#endif
#ifndef MEM_COMMIT
#define MEM_COMMIT 0x1000
#endif
#ifndef MEM_RESERVE
#define MEM_RESERVE 0x2000
#endif
#ifndef PAGE_READWRITE
#define PAGE_READWRITE 0x04
#endif
#ifndef PAGE_EXECUTE_READ
#define PAGE_EXECUTE_READ 0x20
#endif
#ifndef PAGE_EXECUTE_READWRITE
#define PAGE_EXECUTE_READWRITE 0x40
#endif

NTSYSAPI
NTSTATUS
NTAPI
ZwAllocateVirtualMemory(
    HANDLE ProcessHandle,
    PVOID* BaseAddress,
    ULONG_PTR ZeroBits,
    PSIZE_T RegionSize,
    ULONG AllocationType,
    ULONG Protect
);

NTKERNELAPI NTSTATUS NTAPI PsLookupThreadByThreadId(HANDLE ThreadId, PETHREAD* Thread);
NTKERNELAPI NTSTATUS NTAPI PsGetContextThread(PETHREAD Thread,
    PCONTEXT ThreadContext, KPROCESSOR_MODE PreviousMode);
#define PROCESS_QUERY_INFORMATION 0x0400
#define MEMORY_BASIC_INFORMATION_CLASS_ 0

typedef struct _SMX_MBI {
    PVOID  BaseAddress;
    PVOID  AllocationBase;
    ULONG  AllocationProtect;
    USHORT PartitionId;
    USHORT Reserved0;
    SIZE_T RegionSize;
    ULONG  State;
    ULONG  Protect;
    ULONG  Type;
    ULONG  Reserved1;
} SMX_MBI;

NTKERNELAPI
NTSTATUS
ObOpenObjectByPointer(
    PVOID Object,
    ULONG HandleAttributes,
    PVOID PassedAccessState,
    ACCESS_MASK DesiredAccess,
    POBJECT_TYPE ObjectType,
    KPROCESSOR_MODE AccessMode,
    PHANDLE Handle
);

NTKERNELAPI
NTSTATUS
ZwQueryVirtualMemory(
    HANDLE ProcessHandle,
    PVOID BaseAddress,
    ULONG MemoryInformationClass,
    PVOID MemoryInformation,
    SIZE_T MemoryInformationLength,
    PSIZE_T ReturnLength
);

#define KEY_SALT 0x8F2A9C4BUL

#include "smx.h"

static ULONG   g_token = 0;
static BOOLEAN g_created = FALSE;
static volatile LONG g_linkOff = 0;
static PDEVICE_OBJECT g_devObj = NULL;

// FIX: serialize all ActiveProcessLinks mutations. KeAcquireGuardedMutex
// requires IRQL <= APC_LEVEL and PASSIVE_LEVEL init.
static KGUARDED_MUTEX g_hideLock;

// ---------------------------------------------------------------- session key

static ULONG Xorshift(ULONG key) {
    key ^= key << 13;
    key ^= key >> 17;
    key ^= key << 5;
    return key;
}

static NTSTATUS ReadBootId(PULONG bootId) {
    UNICODE_STRING keyPath, valueName;
    OBJECT_ATTRIBUTES attributes;
    HANDLE keyHandle = NULL;
    ULONG result = 0;
    NTSTATUS status;

    *bootId = 0;
    RtlInitUnicodeString(&keyPath,
        L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\Session Manager"
        L"\\Memory Management\\PrefetchParameters");
    RtlInitUnicodeString(&valueName, L"BootId");
    InitializeObjectAttributes(&attributes, &keyPath,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    status = ZwOpenKey(&keyHandle, KEY_READ, &attributes);
    if (!NT_SUCCESS(status))
        return status;

    UCHAR buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)] = { 0 };
    ULONG length = 0;
    status = ZwQueryValueKey(keyHandle, &valueName, KeyValuePartialInformation,
        buffer, sizeof(buffer), &length);
    if (NT_SUCCESS(status)) {
        PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buffer;
        if (info->Type == REG_DWORD && info->DataLength == sizeof(ULONG))
            result = *(ULONG*)info->Data;
        else
            status = STATUS_OBJECT_TYPE_MISMATCH;
    }

    ZwClose(keyHandle);
    *bootId = result;
    return status;
}

static ULONG ComputeRingToken(VOID) {
    ULONG bootId = 0;
    if (!NT_SUCCESS(ReadBootId(&bootId)) || bootId == 0) {
        LARGE_INTEGER systemTime;
        KeQuerySystemTime(&systemTime);
        bootId = systemTime.LowPart ^ systemTime.HighPart;
    }

    ULONG key = Xorshift(bootId ^ KEY_SALT);
    return (key == 0) ? 0xA5A5A5A5UL : key;
}

// ---------------------------------------------------------------- memory ops

// FIX: validate user addresses only on the side that is NOT the current
// process. The side that IS current may be a kernel system buffer, and
// must not be probed as user. PreviousMode stays KernelMode because the
// caller is in kernel; __try/__except in the dispatch contains residual
// faults from a user address that changes between validation and copy.
static NTSTATUS CopyCrossProcess(PEPROCESS sourceProcess, ULONG_PTR sourceAddress,
    PEPROCESS targetProcess, ULONG_PTR targetAddress, SIZE_T size, PSIZE_T transferred) {
    const ULONG64 MAX_USER = 0x00007FFFFFFEFFFFULL;
    ULONG64 src = (ULONG64)sourceAddress;
    ULONG64 dst = (ULONG64)targetAddress;

    if (transferred) *transferred = 0;
    if (!sourceProcess || !targetProcess || size == 0)
        return STATUS_INVALID_PARAMETER;

    if (sourceProcess != PsGetCurrentProcess()) {
        if (src < 0x10000ULL || src > MAX_USER ||
            (src + size) < src || (src + size) > MAX_USER)
            return STATUS_ACCESS_VIOLATION;
    }
    if (targetProcess != PsGetCurrentProcess()) {
        if (dst < 0x10000ULL || dst > MAX_USER ||
            (dst + size) < dst || (dst + size) > MAX_USER)
            return STATUS_ACCESS_VIOLATION;
    }

    return MmCopyVirtualMemory(sourceProcess, (PVOID)sourceAddress,
        targetProcess, (PVOID)targetAddress, size, KernelMode, transferred);
}

static NTSTATUS ReadTargetMemory(ULONG processId, ULONG_PTR address,
    PVOID kernelBuffer, SIZE_T size) {
    PEPROCESS process = NULL;
    NTSTATUS status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)processId, &process);
    if (!NT_SUCCESS(status))
        return status;

    SIZE_T transferred = 0;
    status = CopyCrossProcess(process, address, PsGetCurrentProcess(),
        (ULONG_PTR)kernelBuffer, size, &transferred);
    ObDereferenceObject(process);

    if (NT_SUCCESS(status) && transferred != size)
        return STATUS_PARTIAL_COPY;
    return status;
}

static NTSTATUS WriteTargetMemory(ULONG processId, ULONG_PTR address,
    PVOID kernelBuffer, SIZE_T size) {
    PEPROCESS process = NULL;
    NTSTATUS status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)processId, &process);
    if (!NT_SUCCESS(status))
        return status;

    SIZE_T transferred = 0;
    status = CopyCrossProcess(PsGetCurrentProcess(), (ULONG_PTR)kernelBuffer,
        process, address, size, &transferred);
    ObDereferenceObject(process);

    if (NT_SUCCESS(status) && transferred != size)
        return STATUS_PARTIAL_COPY;
    return status;
}

static NTSTATUS AllocateInTarget(ULONG processId, SIZE_T size, ULONG protect,
    PULONG_PTR outAddress) {
    PEPROCESS process = NULL;
    HANDLE hProc = NULL;
    NTSTATUS status;
    PVOID base = NULL;
    SIZE_T region;

    *outAddress = 0;
    if (size == 0 || size > 0x2000000) return STATUS_INVALID_PARAMETER;
    if (protect != PAGE_READWRITE && protect != PAGE_EXECUTE_READ &&
        protect != PAGE_EXECUTE_READWRITE) return STATUS_INVALID_PARAMETER;

    status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)processId, &process);
    if (!NT_SUCCESS(status)) return status;

    status = ObOpenObjectByPointer(process, OBJ_KERNEL_HANDLE, NULL,
                                   PROCESS_VM_OPERATION, *PsProcessType,
                                   KernelMode, &hProc);
    if (NT_SUCCESS(status)) {
        region = size;
        status = ZwAllocateVirtualMemory(hProc, &base, 0, &region,
                                         MEM_COMMIT | MEM_RESERVE, protect);
        ZwClose(hProc);
    }
    ObDereferenceObject(process);

    if (!NT_SUCCESS(status)) return status;
    if (!base) return STATUS_UNSUCCESSFUL;
    *outAddress = (ULONG_PTR)base;
    return STATUS_SUCCESS;
}

// FIX: PsGetContextThread requires PASSIVE_LEVEL. Guard against being
// called from a raised IRQL and contain faults if the thread dies between
// lookup and the register read.
static NTSTATUS GetThreadContextInfo(ULONG threadId, PSMX_CTX out) {
    PETHREAD thread = NULL;
    CONTEXT ctx;
    NTSTATUS status;

    RtlZeroMemory(out, sizeof(*out));
    out->tid = threadId;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_DEVICE_STATE;

    status = PsLookupThreadByThreadId((HANDLE)(ULONG_PTR)threadId, &thread);
    if (!NT_SUCCESS(status)) return status;

    RtlZeroMemory(&ctx, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;

    __try {
        status = PsGetContextThread(thread, &ctx, KernelMode);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }
    if (NT_SUCCESS(status)) {
        out->rip = (ULONGLONG)ctx.Rip;
        out->rsp = (ULONGLONG)ctx.Rsp;
        out->rbp = (ULONGLONG)ctx.Rbp;
        out->rflags = (ULONGLONG)ctx.EFlags;
        out->ok = 1;
    }
    ObDereferenceObject(thread);
    return status;
}

static NTSTATUS GetTargetImageBase(ULONG processId, PULONG_PTR imageBase) {
    *imageBase = 0;

    PEPROCESS process = NULL;
    NTSTATUS status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)processId, &process);
    if (!NT_SUCCESS(status))
        return status;

    PPEB peb = PsGetProcessPeb(process);
    if (!peb) {
        ObDereferenceObject(process);
        return STATUS_UNSUCCESSFUL;
    }

    SIZE_T transferred = 0;
    status = CopyCrossProcess(process, (ULONG_PTR)peb + 0x10,
        PsGetCurrentProcess(), (ULONG_PTR)imageBase, sizeof(ULONG_PTR), &transferred);
    ObDereferenceObject(process);

    if (NT_SUCCESS(status) && transferred != sizeof(ULONG_PTR))
        return STATUS_PARTIAL_COPY;
    return status;
}

// ---------------------------------------------------------------- process hide

typedef struct _SMX_HIDDEN {
    ULONG       pid;
    LIST_ENTRY* entry;
    LIST_ENTRY* flink;
    LIST_ENTRY* blink;
    BOOLEAN     used;
} SMX_HIDDEN;

static SMX_HIDDEN g_hidden[SMX_HIDE_MAX];

static BOOLEAN SmxIsProcessList(PLIST_ENTRY entry, LONG pidDelta) {
    PLIST_ENTRY node = entry;

    __try {
        if (!entry->Flink || !entry->Blink)
            return FALSE;
        if (entry->Flink->Blink != entry || entry->Blink->Flink != entry)
            return FALSE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return FALSE;
    }

    for (ULONG i = 0; i < 8192; i++) {
        __try {
            ULONG_PTR pid;

            node = node->Flink;
            if (!node || node == entry)
                return FALSE;

            pid = *(ULONG_PTR*)((PUCHAR)node + pidDelta);
            if (pid == 4)
                return TRUE;

            if (!node->Flink)
                return FALSE;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return FALSE;
        }
    }
    return FALSE;
}

static ULONG_PTR SmxLinkOffset(PEPROCESS process) {
    PUCHAR base = (PUCHAR)process;
    ULONG_PTR pid = (ULONG_PTR)PsGetProcessId(process);
    ULONG_PTR pidOffset;
    LONG delta;

    for (pidOffset = 0x80; pidOffset < 0xA40; pidOffset += sizeof(ULONG_PTR)) {
        ULONG_PTR value;

        __try {
            value = *(ULONG_PTR*)(base + pidOffset);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
        if (value != pid)
            continue;

        for (delta = -0x40; delta <= 0x40; delta += (LONG)sizeof(ULONG_PTR)) {
            LONG_PTR linkOffset;
            PLIST_ENTRY entry;
            LONG pidDelta;

            if (delta == 0)
                continue;

            linkOffset = (LONG_PTR)pidOffset + delta;
            if (linkOffset < 0x80 || linkOffset >= 0xA40)
                continue;

            entry = (PLIST_ENTRY)(base + linkOffset);
            pidDelta = (LONG)((LONG_PTR)pidOffset - linkOffset);
            if (SmxIsProcessList(entry, pidDelta))
                return (ULONG_PTR)linkOffset;
        }
    }
    return 0;
}

static SMX_HIDDEN* SmxHiddenSlot(ULONG pid) {
    for (int i = 0; i < SMX_HIDE_MAX; i++) {
        if (g_hidden[i].used && g_hidden[i].pid == pid)
            return &g_hidden[i];
    }
    return NULL;
}

static NTSTATUS SmxSetHidden(ULONG pid, BOOLEAN hide) {
#ifndef SMX_DISABLE_DKOM
    PEPROCESS process = NULL;
    NTSTATUS status;
    SMX_HIDDEN* slot;
    ULONG_PTR linkOffset;
    PLIST_ENTRY entry;

    if (KeGetCurrentIrql() > APC_LEVEL)
        return STATUS_INVALID_DEVICE_STATE;
    if (pid == 0)
        return STATUS_INVALID_PARAMETER;

    status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &process);
    if (!NT_SUCCESS(status))
        return status;

    // FIX: serialize every mutation of g_hidden and of ActiveProcessLinks.
    KeAcquireGuardedMutex(&g_hideLock);

    if (!hide) {
        SMX_HIDDEN* found = SmxHiddenSlot(pid);
        if (found) {
            __try {
                // FIX: only rewire if the neighbours still agree that we
                // are the common link. If they don't, either the entry was
                // re-linked by the kernel or the list is already broken;
                // writing anything would deepen the corruption.
                if (found->entry && found->flink && found->blink &&
                    found->flink->Blink == found->entry &&
                    found->blink->Flink == found->entry) {
                    found->entry->Flink = found->flink;
                    found->entry->Blink = found->blink;
                    found->flink->Blink = found->entry;
                    found->blink->Flink = found->entry;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                status = GetExceptionCode();
            }
            found->used = FALSE;
            found->pid = 0;
            found->entry = NULL;
            found->flink = NULL;
            found->blink = NULL;
        }
        KeReleaseGuardedMutex(&g_hideLock);
        ObDereferenceObject(process);
        return NT_SUCCESS(status) ? STATUS_SUCCESS : status;
    }

    slot = SmxHiddenSlot(pid);
    if (slot) {
        BOOLEAN stillDetached = FALSE;
        __try {
            stillDetached = (slot->entry != NULL && slot->entry->Flink == slot->entry);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            stillDetached = FALSE;
        }
        if (stillDetached) {
            KeReleaseGuardedMutex(&g_hideLock);
            ObDereferenceObject(process);
            return STATUS_SUCCESS;
        }
        slot->used = FALSE;
        slot->pid = 0;
        slot->entry = NULL;
        slot->flink = NULL;
        slot->blink = NULL;
    }

    linkOffset = SmxLinkOffset(process);
    if (!linkOffset) {
        KeReleaseGuardedMutex(&g_hideLock);
        ObDereferenceObject(process);
        return STATUS_NOT_FOUND;
    }

    slot = NULL;
    for (int i = 0; i < SMX_HIDE_MAX; i++) {
        if (!g_hidden[i].used) {
            slot = &g_hidden[i];
            break;
        }
    }
    if (!slot) {
        KeReleaseGuardedMutex(&g_hideLock);
        ObDereferenceObject(process);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    entry = (PLIST_ENTRY)((PUCHAR)process + linkOffset);

    if (pid == 4) {
        KeReleaseGuardedMutex(&g_hideLock);
        ObDereferenceObject(process);
        return STATUS_INVALID_PARAMETER;
    }
    if (entry->Flink == entry) {
        KeReleaseGuardedMutex(&g_hideLock);
        ObDereferenceObject(process);
        return STATUS_SUCCESS;
    }

    __try {
        slot->flink = entry->Flink;
        slot->blink = entry->Blink;
        entry->Flink->Blink = entry->Blink;
        entry->Blink->Flink = entry->Flink;
        entry->Flink = entry;
        entry->Blink = entry;
        slot->entry = entry;
        slot->pid = pid;
        slot->used = TRUE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }

    KeReleaseGuardedMutex(&g_hideLock);
    ObDereferenceObject(process);
    return NT_SUCCESS(status) ? STATUS_SUCCESS : status;
#else
    UNREFERENCED_PARAMETER(pid);
    UNREFERENCED_PARAMETER(hide);
    return STATUS_NOT_IMPLEMENTED;
#endif
}

// ---------------------------------------------------------------- dispatch

static NTSTATUS SmxCreateClose(PDEVICE_OBJECT deviceObject, PIRP irp) {
    UNREFERENCED_PARAMETER(deviceObject);
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTKERNELAPI
NTSTATUS
ObQueryNameString(
    _In_ PVOID Object,
    _Out_writes_bytes_opt_(Length) POBJECT_NAME_INFORMATION ObjectNameInfo,
    _In_ ULONG Length,
    _Out_ PULONG ReturnLength
    );

static VOID SmxLinkKeeper(PVOID context) {
    PDEVICE_OBJECT device = (PDEVICE_OBJECT)context;

    LARGE_INTEGER interval;
    interval.QuadPart = -15000 * 10000LL;
    KeDelayExecutionThread(KernelMode, FALSE, &interval);

    for (;;) {
        if (InterlockedCompareExchange(&g_linkOff, 0, 0)) {
            break;
        }

        if (KeGetCurrentIrql() <= APC_LEVEL) {
            __try {
                UNICODE_STRING dosName;
                RtlInitUnicodeString(&dosName, L"\\??\\SmxRing");

                PWCHAR buf = (PWCHAR)ExAllocatePoolWithTag(NonPagedPoolNx, 0x100, 'ibaD');
                if (buf) {
                    POBJECT_NAME_INFORMATION nameInfo = (POBJECT_NAME_INFORMATION)buf;
                    ULONG retLen = 0;
                    NTSTATUS qstatus = ObQueryNameString(device, nameInfo, 0x100, &retLen);
                    BOOLEAN linkOk = FALSE;
                    if (NT_SUCCESS(qstatus) && nameInfo->Name.Buffer && nameInfo->Name.Length > 0) {
                        OBJECT_ATTRIBUTES oa;
                        HANDLE lnk = NULL;
                        InitializeObjectAttributes(&oa, &dosName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
                        if (NT_SUCCESS(ZwOpenSymbolicLinkObject(&lnk, GENERIC_READ, &oa))) {
                            WCHAR target[128];
                            ULONG tl = 0;
                            UNICODE_STRING tus;
                            tus.Buffer = target; tus.Length = 0; tus.MaximumLength = sizeof(target);
                            if (NT_SUCCESS(ZwQuerySymbolicLinkObject(lnk, &tus, &tl))) {
                                ULONG chars = tus.Length / sizeof(WCHAR);
                                if (chars >= (ULONG)(sizeof(target) / sizeof(WCHAR)))
                                    chars = (ULONG)(sizeof(target) / sizeof(WCHAR)) - 1;
                                target[chars] = L'\0';

                                linkOk = (tus.Length >= nameInfo->Name.Length) &&
                                         (wcsncmp(tus.Buffer, nameInfo->Name.Buffer,
                                                  nameInfo->Name.Length / sizeof(WCHAR)) == 0 ||
                                          wcsstr(target, L"SmxRing") != NULL);
                            }
                            ZwClose(lnk);
                        }
                    }
                    if (!linkOk && NT_SUCCESS(qstatus) && nameInfo->Name.Buffer && nameInfo->Name.Length > 0) {
                        NTSTATUS status = IoCreateUnprotectedSymbolicLink(&dosName, &nameInfo->Name);
                        (VOID)status;
                    }
                    ExFreePoolWithTag(buf, 'ibaD');
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }

        interval.QuadPart = -30000 * 10000LL;
        KeDelayExecutionThread(KernelMode, FALSE, &interval);
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

static NTSTATUS SmxDispatch(PDEVICE_OBJECT deviceObject, PIRP irp) {
    UNREFERENCED_PARAMETER(deviceObject);

    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    ULONG controlCode = stack->Parameters.DeviceIoControl.IoControlCode;
    PVOID systemBuffer = irp->AssociatedIrp.SystemBuffer;
    ULONG inputLength = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outputLength = stack->Parameters.DeviceIoControl.OutputBufferLength;
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    ULONG information = 0;

    __try {
        if (inputLength >= sizeof(SMX_CMD) && systemBuffer) {
            SMX_CMD command = *(PSMX_CMD)systemBuffer;

            if (command.magic_key != g_token) {
                status = STATUS_ACCESS_DENIED;
            } else if (controlCode == SMX_IO_READ) {
                if (command.size == 0 || command.size > SMX_MAX_TRANSFER ||
                    command.size > outputLength) {
                    status = STATUS_INVALID_PARAMETER;
                } else {
                    status = ReadTargetMemory(command.process_id,
                        command.address, systemBuffer, (SIZE_T)command.size);
                    if (NT_SUCCESS(status))
                        information = (ULONG)command.size;
                }
            } else if (controlCode == SMX_IO_WRITE) {
                if (command.size == 0 || command.size > SMX_MAX_TRANSFER ||
                    command.size > inputLength - sizeof(SMX_CMD)) {
                    status = STATUS_INVALID_PARAMETER;
                } else {
                    status = WriteTargetMemory(command.process_id,
                        command.address,
                        (PUCHAR)systemBuffer + sizeof(SMX_CMD),
                        (SIZE_T)command.size);
                }
            } else if (controlCode == SMX_IO_BASE) {
                ULONG_PTR base = 0;
                if (outputLength < sizeof(ULONG_PTR)) {
                    status = STATUS_BUFFER_TOO_SMALL;
                } else {
                    status = GetTargetImageBase(command.process_id, &base);
                    if (NT_SUCCESS(status)) {
                        *(ULONG_PTR*)systemBuffer = base;
                        information = sizeof(ULONG_PTR);
                    }
                }
            } else if (controlCode == SMX_IO_PEB) {
                PEPROCESS process = NULL;
                status = PsLookupProcessByProcessId(
                    (HANDLE)(ULONG_PTR)command.process_id, &process);
                if (NT_SUCCESS(status)) {
                    PPEB peb = PsGetProcessPeb(process);
                    ObDereferenceObject(process);
                    if (!peb) {
                        status = STATUS_UNSUCCESSFUL;
                    } else if (outputLength < sizeof(ULONG_PTR)) {
                        status = STATUS_BUFFER_TOO_SMALL;
                    } else {
                        *(ULONG_PTR*)systemBuffer = (ULONG_PTR)peb;
                        information = sizeof(ULONG_PTR);
                    }
                }
            } else if (controlCode == SMX_IO_REGION) {
                PEPROCESS process = NULL;
                status = PsLookupProcessByProcessId(
                    (HANDLE)(ULONG_PTR)command.process_id, &process);
                if (NT_SUCCESS(status)) {
                    HANDLE h = NULL;
                    status = ObOpenObjectByPointer(process, OBJ_KERNEL_HANDLE, NULL,
                        PROCESS_QUERY_INFORMATION, *PsProcessType, KernelMode, &h);
                    if (NT_SUCCESS(status)) {
                        SMX_MBI mbi;
                        SIZE_T ret = 0;
                        RtlZeroMemory(&mbi, sizeof(mbi));
                        status = ZwQueryVirtualMemory(h, (PVOID)command.address,
                            0, &mbi, sizeof(mbi), &ret);
                        if (NT_SUCCESS(status)) {
                            if (outputLength < sizeof(SMX_REGION)) {
                                status = STATUS_BUFFER_TOO_SMALL;
                            } else {
                                PSMX_REGION out = (PSMX_REGION)systemBuffer;
                                out->base = (ULONG_PTR)mbi.BaseAddress;
                                out->size = mbi.RegionSize;
                                out->state = mbi.State;
                                out->protect = mbi.Protect;
                                out->type = mbi.Type;
                                out->pad = 0;
                                information = sizeof(SMX_REGION);
                            }
                        }
                        ZwClose(h);
                    }
                    ObDereferenceObject(process);
                }
            } else if (controlCode == SMX_IO_BATCH) {
                SMX_BATCH req;
                if (inputLength >= sizeof(SMX_BATCH))
                    RtlCopyMemory(&req, systemBuffer, sizeof(SMX_BATCH));
                else
                    RtlZeroMemory(&req, sizeof(SMX_BATCH));

                if (req.count == 0 || req.count > SMX_BATCH_MAX ||
                    inputLength < sizeof(SMX_BATCH) ||
                    req.count > outputLength ||
                    req.total > outputLength - req.count ||
                    req.total > SMX_MAX_TRANSFER ||
                    req.count > SMX_MAX_TRANSFER - req.total) {
                    status = STATUS_INVALID_PARAMETER;
                } else {
                    PEPROCESS process = NULL;
                    status = PsLookupProcessByProcessId(
                        (HANDLE)(ULONG_PTR)req.process_id, &process);
                    if (NT_SUCCESS(status)) {
                        PUCHAR base = (PUCHAR)systemBuffer;
                        PUCHAR flags = base + req.total;

                        RtlZeroMemory(base, (SIZE_T)req.total + (SIZE_T)req.count);

                        for (ULONG i = 0; i < req.count; i++) {
                            PSMX_BITEM it = &req.items[i];
                            flags[i] = 0;
                            if (it->size == 0 ||
                                it->offset > req.total ||
                                it->size > req.total - it->offset) continue;
                            SIZE_T transferred = 0;
                            NTSTATUS st = CopyCrossProcess(process, it->address,
                                PsGetCurrentProcess(),
                                (ULONG_PTR)(base + it->offset),
                                (SIZE_T)it->size, &transferred);
                            if (NT_SUCCESS(st) && transferred == it->size)
                                flags[i] = 1;
                            else
                                RtlZeroMemory(base + it->offset, it->size);
                        }
                        ObDereferenceObject(process);
                        information = req.total + req.count;
                    }
                }
            } else if (controlCode == SMX_IO_ALLOC) {
                ULONG_PTR allocated = 0;
                if (outputLength < sizeof(ULONG_PTR) || command.size == 0) {
                    status = STATUS_BUFFER_TOO_SMALL;
                } else {
                    status = AllocateInTarget(command.process_id,
                                              (SIZE_T)command.size,
                                              (ULONG)command.address,
                                              &allocated);
                    if (NT_SUCCESS(status)) {
                        *(ULONG_PTR*)systemBuffer = allocated;
                        information = (ULONG)sizeof(ULONG_PTR);
                    }
                }
            } else if (controlCode == SMX_IO_CONTEXT) {
                if (outputLength < sizeof(SMX_CTX) || command.size == 0) {
                    status = STATUS_BUFFER_TOO_SMALL;
                } else {
                    status = GetThreadContextInfo((ULONG)command.size,
                                                  (PSMX_CTX)systemBuffer);
                    if (NT_SUCCESS(status)) information = sizeof(SMX_CTX);
                }
            } else if (controlCode == SMX_IO_HIDE) {
                status = SmxSetHidden((ULONG)command.process_id,
                                      command.address != 0);
            } else if (controlCode == SMX_IO_UNLOAD) {
                UNICODE_STRING dosName;
                RtlInitUnicodeString(&dosName, L"\\??\\SmxRing");
                InterlockedExchange(&g_linkOff, 1);
                IoDeleteSymbolicLink(&dosName);
                g_created = FALSE;
                status = STATUS_SUCCESS;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
        information = 0;
    }

    irp->IoStatus.Status = status;
    irp->IoStatus.Information = information;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

// ---------------------------------------------------------------- driver init

static NTSTATUS SmxInit(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath) {
    UNREFERENCED_PARAMETER(registryPath);

    UNICODE_STRING deviceName;
    PDEVICE_OBJECT device = NULL;
    NTSTATUS status;

    status = STATUS_OBJECT_NAME_COLLISION;
    for (ULONG attempt = 0; attempt < 64 && status == STATUS_OBJECT_NAME_COLLISION; attempt++) {
        WCHAR nameBuf[48] = { 0 };
        const WCHAR base[] = L"\\Device\\SmxRing";
        USHORT len = (USHORT)(sizeof(base) / sizeof(WCHAR) - 1);
        USHORT pos = len;
        ULONG val = attempt;
        WCHAR digits[12];
        int dn = 0;
        do { digits[dn++] = (WCHAR)(L'0' + (val % 10)); val /= 10; } while (val);
        while (dn > 0 && pos + 1 < 48) nameBuf[pos++] = digits[--dn];
        nameBuf[pos] = L'\0';
        for (USHORT i = 0; i < len; i++) nameBuf[i] = base[i];
        RtlInitUnicodeString(&deviceName, nameBuf);

        status = IoCreateDevice(driverObject, 0, &deviceName, FILE_DEVICE_UNKNOWN,
            FILE_DEVICE_SECURE_OPEN, FALSE, &device);
    }
    if (!NT_SUCCESS(status)) {
        return status;
    }

    device->Flags |= DO_BUFFERED_IO;
    device->Flags &= ~DO_DEVICE_INITIALIZING;

    driverObject->MajorFunction[IRP_MJ_CREATE]         = SmxCreateClose;
    driverObject->MajorFunction[IRP_MJ_CLOSE]          = SmxCreateClose;
    driverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = SmxDispatch;

    g_devObj = device;
    g_created = TRUE;

    {
        HANDLE keeper = NULL;
        if (NT_SUCCESS(PsCreateSystemThread(&keeper, THREAD_ALL_ACCESS,
                NULL, NULL, NULL, SmxLinkKeeper, device))) {
            ZwClose(keeper);
        }
    }

    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------- init thread

// FIX: the old SmxTrace built "S<n>" in a local char buffer but then
// reinitialized the UNICODE_STRING with a constant L"Step", so every
// stage overwrote the same value. Write to the intended name.
static VOID SmxTrace(ULONG step, NTSTATUS st) {
    UNICODE_STRING keyPath, valName;
    OBJECT_ATTRIBUTES attributes;
    HANDLE keyHandle = NULL;
    RtlInitUnicodeString(&keyPath, L"\\Registry\\Machine\\SYSTEM\\SmxRing");
    InitializeObjectAttributes(&attributes, &keyPath,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (NT_SUCCESS(ZwCreateKey(&keyHandle, KEY_SET_VALUE, &attributes, 0,
            NULL, REG_OPTION_VOLATILE, NULL))) {
        WCHAR nameBuf[8];
        nameBuf[0] = L'S';
        nameBuf[1] = (WCHAR)(L'0' + (step % 10));
        nameBuf[2] = L'\0';
        RtlInitUnicodeString(&valName, nameBuf);
        ZwSetValueKey(keyHandle, &valName, 0, REG_DWORD, &st, sizeof(st));
        ZwClose(keyHandle);
    }
}

static VOID SmxBootstrap(PVOID context) {
    UNREFERENCED_PARAMETER(context);

    SmxTrace(1, 0);

    g_token = ComputeRingToken();
    SmxTrace(2, 0);

    // FIX: initialize the guarded mutex once, at PASSIVE_LEVEL, before
    // any dispatch can run.
    KeInitializeGuardedMutex(&g_hideLock);

    NTSTATUS status = STATUS_OBJECT_NAME_COLLISION;
    for (ULONG attempt = 0; attempt < 64 && status == STATUS_OBJECT_NAME_COLLISION; attempt++) {
        WCHAR nameBuf[48] = { 0 };
        UNICODE_STRING driverName;
        const WCHAR base[] = L"\\Driver\\SmxRing";
        USHORT len = (USHORT)(sizeof(base) / sizeof(WCHAR) - 1);
        USHORT pos = len;
        ULONG val = attempt;
        WCHAR digits[12];
        int dn = 0;
        do { digits[dn++] = (WCHAR)(L'0' + (val % 10)); val /= 10; } while (val);
        while (dn > 0 && pos + 1 < 48) nameBuf[pos++] = digits[--dn];
        nameBuf[pos] = L'\0';
        for (USHORT i = 0; i < len; i++) nameBuf[i] = base[i];
        RtlInitUnicodeString(&driverName, nameBuf);

        status = IoCreateDriver(&driverName, SmxInit);
        SmxTrace(3, status);
    }

    {
        UNICODE_STRING keyPath, valName;
        OBJECT_ATTRIBUTES attributes;
        HANDLE keyHandle = NULL;
        RtlInitUnicodeString(&keyPath, L"\\Registry\\Machine\\SYSTEM\\SmxRing");
        InitializeObjectAttributes(&attributes, &keyPath,
            OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        if (NT_SUCCESS(ZwCreateKey(&keyHandle, KEY_SET_VALUE, &attributes, 0,
                NULL, REG_OPTION_VOLATILE, NULL))) {
            RtlInitUnicodeString(&valName, L"InitStatus");
            ULONG st = (ULONG)status;
            ZwSetValueKey(keyHandle, &valName, 0, REG_DWORD, &st, sizeof(st));
            RtlInitUnicodeString(&valName, L"RingToken");
            ULONG sk = g_token;
            ZwSetValueKey(keyHandle, &valName, 0, REG_DWORD, &sk, sizeof(sk));
            ZwClose(keyHandle);
        }
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

// ---------------------------------------------------------------- entry

NTSTATUS DriverEntry(PVOID parameterOne, PVOID parameterTwo) {
    UNREFERENCED_PARAMETER(parameterOne);
    UNREFERENCED_PARAMETER(parameterTwo);

    HANDLE thread = NULL;
    NTSTATUS status = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS,
        NULL, NULL, NULL, SmxBootstrap, NULL);
    if (!NT_SUCCESS(status))
        return status;

    ZwClose(thread);
    return STATUS_SUCCESS;
}