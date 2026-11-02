#!/usr/bin/env bash
# check-xp-compat.sh - audit a PE file for Windows XP compatibility:
#   1. PE subsystem version must not exceed the XP ceiling (5.1 for 32-bit).
#   2. No imported symbol known to be Vista+-only.
# Adapted from ffmpeg-windows-build-helpers' scripts/check-xp-compat.sh.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
OBJDUMP=sandbox/cross_compilers/mingw-w64-i686/bin/i686-w64-mingw32-objdump

BLOCKLIST='\<(CancelIoEx|BCryptGenRandom|BCryptOpenAlgorithmProvider|BCryptCloseAlgorithmProvider|InitOnceExecuteOnce|InitOnceBeginInitialize|InitOnceComplete|InitializeSRWLock|AcquireSRWLockExclusive|AcquireSRWLockShared|ReleaseSRWLockExclusive|ReleaseSRWLockShared|TryAcquireSRWLockExclusive|TryAcquireSRWLockShared|InitializeConditionVariable|WakeConditionVariable|WakeAllConditionVariable|SleepConditionVariableCS|SleepConditionVariableSRW|GetTickCount64|CreateSymbolicLinkA|CreateSymbolicLinkW|SetThreadDescription|GetFileInformationByHandleEx|SetFileInformationByHandle|GetLogicalProcessorInformation|GetLogicalProcessorInformationEx|K32EnumProcesses|CreateWaitableTimerExA|CreateWaitableTimerExW|GetSystemTimePreciseAsFileTime|WSAPoll|GetAddrInfoExA|GetAddrInfoExW|inet_ntop|inet_pton|WSASendMsg|GetActiveProcessorCount|GetActiveProcessorGroupCount|GetMaximumProcessorCount|GetThreadGroupAffinity|SetThreadGroupAffinity|GetNumaProcessorNodeEx|GetNumaNodeProcessorMaskEx|GetNumaHighestNodeNumberEx|CreateFile2|InitializeProcThreadAttributeList|UpdateProcThreadAttribute|IsWow64Process2|[a-zA-Z_][a-zA-Z0-9_]*_s)\>'

fail=0
for f in "$@"; do
	echo "== $(basename "$f") =="
	dump=$("$OBJDUMP" -p "$f")
	major=$(grep -m1 -i MajorSubsystemVersion <<<"$dump" | awk '{print $2}')
	minor=$(grep -m1 -i MinorSubsystemVersion <<<"$dump" | awk '{print $2}')
	echo "  PE subsystem version: $major.$minor (ceiling: 5.1)"
	if (( major > 5 || (major == 5 && minor > 1) )); then
		echo "  FAIL: subsystem version exceeds XP"; fail=1
	fi
	hits=$(grep -oE "$BLOCKLIST" <<<"$dump" | sort -u || true)
	if [[ -n $hits ]]; then
		echo "  FAIL: Vista+-only import(s):"; sed 's/^/    /' <<<"$hits"; fail=1
	else
		echo "  OK: no known Vista+-only imports."
	fi
	echo "  imported DLLs:"
	grep -i "DLL Name" <<<"$dump" | sed 's/^/    /'
done
exit $fail
