//####################################################################
/* Copyright (C) 2003 Louis Thomas. License: http://www.latenighthacking.com/projects/lnhfslicense.html */
/* Copyright (C) 2009 J�r�me Delagnes. License: http://www.latenighthacking.com/projects/lnhfslicense.html */
//####################################################################

#include <windows.h>
#include <stdio.h>
#include <malloc.h>
#include <aclapi.h>
#include <string.h>

//####################################################################

typedef unsigned int RETVAL;

#define STRINGIFY(A) #A

#define EXIT_OK 0

#define _TeardownLastError(rv, errorsource) \
    { \
        RETVAL rv2__=GetLastError(); \
        printf(errorsource " failed with 0x%08X.\n", rv2__); \
        if (EXIT_OK==rv) { \
            rv=rv2__; \
        } \
    }

#define _TeardownIfError(rv, rv2, errorsource) \
    if (EXIT_OK!=rv2) { \
        printf(errorsource " failed with 0x%08X.\n", rv2); \
        if (EXIT_OK==rv) { \
            rv=rv2; \
        } \
    }

#define _JumpLastError(rv, label, errorsource) \
    rv=GetLastError(); \
    printf(errorsource " failed with 0x%08X.\n", rv); \
    goto label;

#define _JumpLastErrorStr(rv, label, errorsource, str) \
    rv=GetLastError(); \
    printf( errorsource "(%s) failed with 0x%08X.\n", str, rv); \
    goto label;

#define _JumpIfError(rv, label, errorsource) \
    if (EXIT_OK!=rv) {\
        printf( errorsource " failed with 0x%08X.\n", rv); \
        goto label; \
    }

#define _JumpIfErrorStr(rv, label, errorsource, str) \
    if (EXIT_OK!=rv) {\
        printf( errorsource "(%s) failed with 0x%08X.\n", str, rv); \
        goto label; \
    }

#define _JumpError(rv, label, errorsource) \
    printf( errorsource " failed with 0x%08X.\n", rv); \
    goto label;

#define _JumpErrorStr(rv, label, errorsource, str) \
    printf( errorsource "(%s) failed with 0x%08X.\n", str, rv); \
    goto label;

#define _JumpIfOutOfMemory(rv, label, pointer) \
    if (NULL==(pointer)) { \
        rv=ERROR_NOT_ENOUGH_MEMORY; \
        printf("Out of memory ('" #pointer "').\n"); \
        goto label; \
    }

#define _Verify(expression, rv, label) \
    if (!(expression)) { \
        printf("Verify failed: '%s' is false.\n", #expression); \
        rv=E_UNEXPECTED; \
        goto label; \
    }

//####################################################################

static DWORD g_dwCtrlRoutineAddr=0;
static HANDLE g_hAddrFoundEvent=NULL;
static DWORD g_dwCtrlEvent=0;

//####################################################################

//--------------------------------------------------------------------
void PrintError(DWORD dwError) {
	char * szErrorMessage=NULL;
	DWORD dwResult=FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM, 
		NULL/*ignored*/, dwError, 0/*language*/, (char *)&szErrorMessage, 0/*min-size*/, NULL/*valist*/);
	if (0==dwResult) {
		printf("(FormatMessage failed)");
	} else {
		printf("%s", szErrorMessage);
	}
	if (NULL!=szErrorMessage) {
		LocalFree(szErrorMessage);
	}
}

//--------------------------------------------------------------------
RETVAL StartRemoteThread(HANDLE hRemoteProc, DWORD dwEntryPoint){
    RETVAL rv;

    // must be cleaned up
    HANDLE hRemoteThread=NULL;

    // inject the thread
    hRemoteThread=CreateRemoteThread(hRemoteProc, NULL, 0, (LPTHREAD_START_ROUTINE)dwEntryPoint, (void *)g_dwCtrlEvent, CREATE_SUSPENDED, NULL);
    if (NULL==hRemoteThread) {
        _JumpLastError(rv, error, "CreateRemoteThread");
    }

    // wake up the thread
    if (-1==ResumeThread(hRemoteThread)) {
        _JumpLastError(rv, error, "ResumeThread");
    }

    // wait for the thread to finish
    if (WAIT_OBJECT_0!=WaitForSingleObject(hRemoteThread, INFINITE)) {
        _JumpLastError(rv, error, "WaitForSingleObject");
    }

    // find out what happened
    if (!GetExitCodeThread(hRemoteThread, (DWORD *)&rv)) {
        _JumpLastError(rv, error, "GetExitCodeThread");
    }

    if (STATUS_CONTROL_C_EXIT==rv) {
        printf("Target process was killed.\n");
        rv=EXIT_OK;
    } else if (EXIT_OK!=rv) {
        printf("(remote function) failed with 0x%08X.\n", rv);
        //if (ERROR_INVALID_HANDLE==rv) {
        //    printf("Are you sure this is a console application?\n");
        //}
    }


error:
    if (NULL!=hRemoteThread) {
        if (!CloseHandle(hRemoteThread)) {
            _TeardownLastError(rv, "CloseHandle");
        }
    }

    return rv;
}

//--------------------------------------------------------------------
void PrintHelp(void) {
    printf(
        "usage: winkill signal pid\n"
        "  signal - windows signals\n"
        "           SIGINT  [%d] interrupt (Ctrl-C windows signal)\n"
        "           SIGQUIT [%d] quit (Ctrl-Break windows signal)\n"
        "  pid    - process PID (hex ok)\n",
        CTRL_C_EVENT,
        CTRL_BREAK_EVENT
        );
}

//--------------------------------------------------------------------
RETVAL SetPrivilege(HANDLE hToken, char * szPrivilege, bool bEnablePrivilege) {
    RETVAL rv;

    TOKEN_PRIVILEGES tp;
    LUID luid;

    if (!LookupPrivilegeValue(NULL, szPrivilege, &luid)) {
        _JumpLastError(rv, error, "LookupPrivilegeValue");
    }

    tp.PrivilegeCount=1;
    tp.Privileges[0].Luid=luid;
    if (bEnablePrivilege) {
        tp.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;
    } else {
        tp.Privileges[0].Attributes=0;
    }

    AdjustTokenPrivileges(hToken, false, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL); // may return true though it failed
    rv=GetLastError();
    _JumpIfError(rv, error, "AdjustTokenPrivileges");

    rv=EXIT_OK;
error:
    return rv;
}

//--------------------------------------------------------------------
RETVAL AdvancedOpenProcess(DWORD dwPid, HANDLE * phRemoteProc) {
    RETVAL rv, rv2;

    #define NEEDEDACCESS    PROCESS_QUERY_INFORMATION|PROCESS_VM_WRITE|PROCESS_VM_READ|PROCESS_VM_OPERATION|PROCESS_CREATE_THREAD

    // must be cleaned up
    HANDLE hThisProcToken=NULL;

    // initialize out params
    *phRemoteProc=NULL;
    bool bDebugPriv=false;

    // get a process handle with the needed access
    *phRemoteProc=OpenProcess(NEEDEDACCESS, false, dwPid);
    if (NULL==*phRemoteProc) {
        rv=GetLastError();
        if (ERROR_ACCESS_DENIED!=rv) {
            _JumpError(rv, error, "OpenProcess");
        }
        printf("Access denied; retrying with increased privileges.\n");

        // give ourselves god-like access over process handles
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hThisProcToken)) {
            _JumpLastError(rv, error, "OpenProcessToken");
        }

        rv=SetPrivilege(hThisProcToken, SE_DEBUG_NAME, true);
        if (EXIT_OK==rv) {
            bDebugPriv=true;
        }
        _JumpIfErrorStr(rv, error, "SetPrivilege", SE_DEBUG_NAME);

        // get a process handle with the needed access
        *phRemoteProc=OpenProcess(NEEDEDACCESS, false, dwPid);
        if (NULL==*phRemoteProc) {
            _JumpLastError(rv, error, "OpenProcess");
        }
    }

    // success
    rv=EXIT_OK;

error:
    if (ERROR_ACCESS_DENIED==rv && false==bDebugPriv) {
        printf("You need administrative access (debug privilege) to access this process.\n");
    }
    if (true==bDebugPriv) {
        rv2=SetPrivilege(hThisProcToken, SE_DEBUG_NAME, false);
        _TeardownIfError(rv, rv2, "SetPrivilege");
    }
    if (NULL!=hThisProcToken) {
        if (!CloseHandle(hThisProcToken)) {
            _TeardownLastError(rv, "CloseHandle");
        }
    }
    return rv;
}

//--------------------------------------------------------------------
BOOL WINAPI MyHandler(DWORD dwCtrlType) {
    // test
    //__asm { int 3 };
    if (g_dwCtrlEvent!=dwCtrlType) {
        return FALSE;
    }


    //printf("Received ctrl-break event\n");
    if (0==g_dwCtrlRoutineAddr) {

        // read the stack base address from the TEB
        #define TEB_OFFSET 4
        DWORD * pStackBase;

#if defined __unix__ || ( defined _WIN32 && defined __GNUG__ )
        // this is for gcc and MinGW
        __asm__ ( "mov %%fs:(0x04), %%eax" ::);
        __asm__ ( "mov %%eax, %[pStackBase]" : [pStackBase] "=r" (pStackBase) );
#elif defined _WIN32 && !defined __GNUG__
        // this is for MSVC
        __asm { mov eax, fs:[TEB_OFFSET] }
        __asm { mov pStackBase, eax }
#elif
        asm style unknown
#endif

        // read the parameter off the stack
        #define PARAM_0_OF_BASE_THEAD_START_OFFSET -3
        g_dwCtrlRoutineAddr=pStackBase[PARAM_0_OF_BASE_THEAD_START_OFFSET];

        // notify that we now have the address
        if (!SetEvent(g_hAddrFoundEvent)) {
            printf("SetEvent failed with 0x08X.\n", GetLastError());
        }
    }
    return TRUE;
}

//--------------------------------------------------------------------
RETVAL GetCtrlRoutineAddress(void) {
    RETVAL rv=EXIT_OK;

    // must be cleaned up
    g_hAddrFoundEvent=NULL;

    // create an event so we know when the async callback has completed
    g_hAddrFoundEvent=CreateEvent(NULL, TRUE, FALSE, NULL); // no security, manual reset, initially unsignaled, no name
    if (NULL==g_hAddrFoundEvent) {
        _JumpLastError(rv, error, "CreateEvent");
    }

    // request that we be called on system signals
	if (!SetConsoleCtrlHandler(MyHandler, TRUE)) {
		_JumpLastError(rv, error, "SetConsoleCtrlHandler");
	}

    // generate a signal
	if (!GenerateConsoleCtrlEvent(g_dwCtrlEvent, 0)) {
		_JumpLastError(rv, error, "GenerateConsoleCtrlEvent");
	}

    // wait for our handler to be called
    {
        DWORD dwWaitResult=WaitForSingleObject(g_hAddrFoundEvent, INFINITE);
        if (WAIT_FAILED==dwWaitResult) {
            _JumpLastError(rv, error, "WaitForSingleObject");
        }
    }

    _Verify(0!=g_dwCtrlRoutineAddr, rv, error);

error:
    if (NULL!=g_hAddrFoundEvent) {
        if (!CloseHandle(g_hAddrFoundEvent)) {
            _TeardownLastError(rv, "CloseHandle");
        }
    }
    return rv;
}

//--------------------------------------------------------------------
int main(unsigned int nArgs, char ** rgszArgs) {
    RETVAL rv;

    HANDLE hRemoteProc=NULL;
    HANDLE hRemoteProcToken=NULL;
    bool bSignalThisProcessGroup=false;

    if (3!=nArgs || (('/'==rgszArgs[1][0] || '-'==rgszArgs[1][0]) 
        && ('H'==rgszArgs[1][1] || 'h'==rgszArgs[1][1] || '?'==rgszArgs[1][1]))) 
    {
        PrintHelp();
        exit(1);
    }

    char * szSignal=rgszArgs[1];
    char * szPid=rgszArgs[2];
    DWORD dwPid;

    if (strcmp("SIGINT", szSignal)==0) {
        g_dwCtrlEvent=CTRL_C_EVENT;
    } else if (strcmp("SIGQUIT", szSignal)==0) {
        g_dwCtrlEvent=CTRL_BREAK_EVENT;
    } else {
        printf("\"%s\" is not a valid signal.\n", szSignal);
        PrintHelp();
        rv=ERROR_INVALID_PARAMETER;
        goto error;
    }

    // check for the special parameter
    bSignalThisProcessGroup=('-'==szPid[0]);
    char * szEnd;
    dwPid=strtoul(szPid, &szEnd, 0);
    if (false==bSignalThisProcessGroup && (szEnd==szPid || 0==dwPid)) {
        printf("\"%s\" is not a valid PID.\n", szPid);
        rv=ERROR_INVALID_PARAMETER;
        goto error;
    }

    //printf("Determining address of kernel32!CtrlRoutine...\n");
    rv=GetCtrlRoutineAddress();
    _JumpIfError(rv, error, "GetCtrlRoutineAddress");
    //printf("Address is 0x%08X.\n", g_dwCtrlRoutineAddr);

    // open the process
    if ('-'==rgszArgs[1][0]) {
        printf("Sending signal to self...\n");
        hRemoteProc=GetCurrentProcess();
    } else {
        printf("Sending signal to process %d...\n", dwPid);
        rv=AdvancedOpenProcess(dwPid, &hRemoteProc);
        _JumpIfErrorStr(rv, error, "AdvancedOpenProcess", rgszArgs[1]);
    }

    rv=StartRemoteThread(hRemoteProc, g_dwCtrlRoutineAddr);
    _JumpIfError(rv, error, "StartRemoteThread");

//done:
    rv=EXIT_OK;

error:
    if (NULL!=hRemoteProc && GetCurrentProcess()!=hRemoteProc) {
        if (!CloseHandle(hRemoteProc)) {
            _TeardownLastError(rv, "CloseHandle");
        }
    }
    if (EXIT_OK!=rv) {
        printf("0x%08X == ", rv);
        PrintError(rv);
    }
    return rv;
}

//####################################################################
