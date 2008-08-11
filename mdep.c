/*******************************************************************************
 * Copyright (c) 2007, 2008 Wind River Systems, Inc. and others.
 * All rights reserved. This program and the accompanying materials 
 * are made available under the terms of the Eclipse Public License v1.0 
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 * The Eclipse Public License is available at
 * http://www.eclipse.org/legal/epl-v10.html
 * and the Eclipse Distribution License is available at 
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *  
 * Contributors:
 *     Wind River Systems - initial API and implementation
 *******************************************************************************/

/*
 * Machine and OS dependend definitions.
 * This module implements host OS abstraction layer that helps make
 * agent code portable between Linux, Windows, VxWorks and potentially other OSes.
 */

#include "mdep.h"
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include "myalloc.h"
#include "errors.h"

pthread_attr_t pthread_create_attr;

#if defined(WIN32)

#include <process.h>
#include <fcntl.h>

/*********************************************************************
    Support of pthreads on Windows is implemented according to
    reccomendations from the paper:
    
    Strategies for Implementing POSIX Condition Variables on Win32
    C++ Report, SIGS, Vol. 10, No. 5, June, 1998

    Douglas C. Schmidt and Irfan Pyarali
    Department of Computer Science
    Washington University, St. Louis, Missouri
**********************************************************************/

typedef struct {
    int waiters_count;
    CRITICAL_SECTION waiters_count_lock;
    HANDLE sema;
    HANDLE waiters_done;
    size_t was_broadcast;
} PThreadCond;

static void check_w32_error(const char * fn, int ok) {
    char msg[256];
    LPVOID msg_buf;
    DWORD error;

    if (ok) return;

    error = GetLastError();
    if (!FormatMessage( 
        FORMAT_MESSAGE_ALLOCATE_BUFFER | 
        FORMAT_MESSAGE_FROM_SYSTEM | 
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), /* Default language */
        (LPTSTR) &msg_buf,
        0,
        NULL))
    {
        snprintf(msg, sizeof(msg), "Win32 error %d", error);
    }
    else {
        int l;
        snprintf(msg, sizeof(msg), "Win32 error %d: %s", error, msg_buf, sizeof(msg));
        LocalFree(msg_buf);
        l = strlen(msg);
        while (l > 0 && (msg[l - 1] == '\n' || msg[l - 1] == '\r')) l--;
        msg[l] = 0;
    }
    fprintf(stderr, "Fatal error: pthreads, %s: %s\n", fn, msg);
    exit(1);
}

int pthread_mutex_init(pthread_mutex_t * mutex, const pthread_mutexattr_t * attr) {
    assert(attr == NULL);
    *mutex = CreateMutex(NULL, FALSE, NULL);
    check_w32_error("CreateMutex", *mutex != NULL);
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t * mutex) {
    assert(mutex != NULL);
    assert(*mutex != NULL);
    check_w32_error("WaitForSingleObject", WaitForSingleObject(*mutex, INFINITE) != WAIT_FAILED);
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t * mutex) {
    assert(mutex != NULL);
    assert(*mutex != NULL);
    check_w32_error("ReleaseMutex", ReleaseMutex(*mutex));
    return 0;
}

int pthread_cond_init(pthread_cond_t * cond, const pthread_condattr_t * attr) {
    PThreadCond * p = loc_alloc_zero(sizeof(PThreadCond));
    assert(attr == NULL);
    p->waiters_count = 0;
    p->was_broadcast = 0;
    p->sema = CreateSemaphore(NULL, 0, 0x7fffffff, NULL);
    check_w32_error("CreateSemaphore", p->sema != NULL);
    InitializeCriticalSection(&p->waiters_count_lock);
    p->waiters_done = CreateEvent(NULL, FALSE, FALSE, NULL);
    check_w32_error("CreateEvent", p->waiters_done != NULL);
    *cond = (pthread_cond_t)p;
    return 0;
}

int pthread_cond_wait(pthread_cond_t * cond, pthread_mutex_t * mutex) {
    DWORD res = 0;
    int last_waiter = 0;
    PThreadCond * p = (PThreadCond *)*cond;

    EnterCriticalSection(&p->waiters_count_lock);
    p->waiters_count++;
    LeaveCriticalSection(&p->waiters_count_lock);

    /* This call atomically releases the mutex and waits on the */
    /* semaphore until <pthread_cond_signal> or <pthread_cond_broadcast> */
    /* are called by another thread. */
    res = SignalObjectAndWait(*mutex, p->sema, INFINITE, FALSE);
    check_w32_error("SignalObjectAndWait", res != WAIT_FAILED);

    /* Reacquire lock to avoid race conditions. */
    EnterCriticalSection(&p->waiters_count_lock);

    /* We're no longer waiting... */
    p->waiters_count--;

    /* Check to see if we're the last waiter after <pthread_cond_broadcast>. */
    last_waiter = p->was_broadcast && p->waiters_count == 0;

    LeaveCriticalSection(&p->waiters_count_lock);

    /* If we're the last waiter thread during this particular broadcast */
    /* then let all the other threads proceed. */
    if (last_waiter) {
        /* This call atomically signals the <waiters_done_> event and waits until */
        /* it can acquire the <mutex>.  This is required to ensure fairness.  */
        DWORD err = SignalObjectAndWait(p->waiters_done, *mutex, INFINITE, FALSE);
        check_w32_error("SignalObjectAndWait", err != WAIT_FAILED);
    }
    else {
        /* Always regain the external mutex since that's the guarantee we */
        /* give to our callers.  */
        DWORD err = WaitForSingleObject(*mutex, INFINITE);
        check_w32_error("WaitForSingleObject", err != WAIT_FAILED);
    }
    assert(res == WAIT_OBJECT_0);
    return 0;
}

int pthread_cond_timedwait(pthread_cond_t * cond, pthread_mutex_t * mutex, const struct timespec * abstime) {
    DWORD res = 0;
    int last_waiter = 0;
    PThreadCond * p = (PThreadCond *)*cond;
    DWORD timeout = 0;
    struct timespec timenow;

    if (clock_gettime(CLOCK_REALTIME, &timenow)) return errno;
    if (abstime->tv_sec < timenow.tv_sec) return ETIMEDOUT;
    if (abstime->tv_sec == timenow.tv_sec) {
        if (abstime->tv_nsec <= timenow.tv_nsec) return ETIMEDOUT;
    }
    timeout = (DWORD)((abstime->tv_sec - timenow.tv_sec) * 1000 + (abstime->tv_nsec - timenow.tv_nsec) / 1000000 + 5);

    EnterCriticalSection(&p->waiters_count_lock);
    p->waiters_count++;
    LeaveCriticalSection(&p->waiters_count_lock);

    /* This call atomically releases the mutex and waits on the */
    /* semaphore until <pthread_cond_signal> or <pthread_cond_broadcast> */
    /* are called by another thread. */
    res = SignalObjectAndWait(*mutex, p->sema, timeout, FALSE);
    check_w32_error("SignalObjectAndWait", res != WAIT_FAILED);

    /* Reacquire lock to avoid race conditions. */
    EnterCriticalSection(&p->waiters_count_lock);

    /* We're no longer waiting... */
    p->waiters_count--;

    /* Check to see if we're the last waiter after <pthread_cond_broadcast>. */
    last_waiter = p->was_broadcast && p->waiters_count == 0;

    LeaveCriticalSection(&p->waiters_count_lock);

    /* If we're the last waiter thread during this particular broadcast */
    /* then let all the other threads proceed. */
    if (last_waiter) {
        /* This call atomically signals the <waiters_done> event and waits until */
        /* it can acquire the <mutex>.  This is required to ensure fairness.  */
        DWORD err = SignalObjectAndWait(p->waiters_done, *mutex, INFINITE, FALSE);
        check_w32_error("SignalObjectAndWait", err != WAIT_FAILED);
    }
    else {
        /* Always regain the external mutex since that's the guarantee we */
        /* give to our callers.  */
        DWORD err = WaitForSingleObject(*mutex, INFINITE);
        check_w32_error("WaitForSingleObject", err != WAIT_FAILED);
    }

    if (res == WAIT_TIMEOUT) return errno = ETIMEDOUT;
    assert(res == WAIT_OBJECT_0);
    return 0;
}

int pthread_cond_signal(pthread_cond_t * cond) {
    int have_waiters = 0;
    PThreadCond * p = (PThreadCond *)*cond;
    
    EnterCriticalSection(&p->waiters_count_lock);
    have_waiters = p->waiters_count > 0;
    LeaveCriticalSection(&p->waiters_count_lock);

    /* If there aren't any waiters, then this is a no-op.   */
    if (have_waiters) check_w32_error("ReleaseSemaphore", ReleaseSemaphore(p->sema, 1, 0));
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t * cond) {
    int have_waiters = 0;
    PThreadCond * p = (PThreadCond *)*cond;

    /* This is needed to ensure that <waiters_count_> and <was_broadcast_> are */
    /* consistent relative to each other. */
    EnterCriticalSection(&p->waiters_count_lock);

    if (p->waiters_count > 0) {
        /* We are broadcasting, even if there is just one waiter... */
        /* Record that we are broadcasting, which helps optimize */
        /* <pthread_cond_wait> for the non-broadcast case. */
        p->was_broadcast = 1;
        have_waiters = 1;
    }

    if (have_waiters) {
        /* Wake up all the waiters atomically. */
        check_w32_error("ReleaseSemaphore", ReleaseSemaphore(p->sema, p->waiters_count, 0));

        LeaveCriticalSection(&p->waiters_count_lock);

        /* Wait for all the awakened threads to acquire the counting */
        /* semaphore.  */
        check_w32_error("WaitForSingleObject", WaitForSingleObject(p->waiters_done, INFINITE) != WAIT_FAILED);
        /* This assignment is okay, even without the <waiters_count_lock_> held  */
        /* because no other waiter threads can wake up to access it. */
        p->was_broadcast = 0;
    }
    else {
        LeaveCriticalSection(&p->waiters_count_lock);
    }
    return 0;
}

int pthread_cond_destroy(pthread_cond_t * cond) {
    PThreadCond * p = (PThreadCond *)*cond;

    DeleteCriticalSection(&p->waiters_count_lock);
    check_w32_error("CloseHandle", CloseHandle(p->sema));
    check_w32_error("CloseHandle", CloseHandle(p->waiters_done));

    loc_free(p);
    *cond = NULL;
    return 0;
}

typedef struct ThreadArgs ThreadArgs;

struct ThreadArgs {
    void * (*start)(void *);
    void * args;
};

static void start_thread(void * x) {
    ThreadArgs a = *(ThreadArgs *)x;

    loc_free(x);
    ExitThread((DWORD)a.start(a.args));
}

int pthread_create(pthread_t * thread, const pthread_attr_t * attr,
                   void * (*start)(void *), void * args) {
    HANDLE r;
    ThreadArgs * a;

    a = (ThreadArgs *)loc_alloc(sizeof(ThreadArgs));
    a->start = start;
    a->args = args;
#ifdef __CYGWIN__
    r = CreateThread (0, 0, (LPTHREAD_START_ROUTINE)start_thread, a, 0, 0);
    if (r == NULL) {
        loc_free(a);
        return errno = EINVAL;
    }
#else
    r = (HANDLE)_beginthread(start_thread, 0, a);
    if (r == (HANDLE)-1) {
        int error = errno;
        loc_free(a);
        return errno = error;
    }
#endif
    *thread = r;
    return 0;
}

int pthread_join(pthread_t thread, void ** value_ptr) {
    check_w32_error("WaitForSingleObject", WaitForSingleObject(thread, INFINITE) != WAIT_FAILED);
    if (value_ptr != NULL && !GetExitCodeThread(thread, (LPDWORD)value_ptr)) {
        return EINVAL;
    }
    check_w32_error("CloseHandle", CloseHandle(thread));
    return 0;
}

pthread_t pthread_self(void) {
    return GetCurrentThread();
}

int pthread_attr_init(pthread_attr_t * attr) {
    return 0;
}

#undef socket
int wsa_socket(int af, int type, int protocol) {
    int res = 0;
    SetLastError(0);
    WSASetLastError(0);
    res = socket(af, type, protocol);
    if (res < 0) {
        set_win32_errno(WSAGetLastError());
        return -1;
    }
    return res;
}

#undef bind
int wsa_bind(int socket, const struct sockaddr * addr, int addr_size) {
    int res = 0;
    SetLastError(0);
    WSASetLastError(0);
    res = bind(socket, addr, addr_size);
    if (res != 0) {
        set_win32_errno(WSAGetLastError());
        return -1;
    }
    return 0;
}

#undef listen
int wsa_listen(int socket, int size) {
    int res = 0;
    SetLastError(0);
    WSASetLastError(0);
    res = listen(socket, size);
    if (res != 0) {
        set_win32_errno(WSAGetLastError());
        return -1;
    }
    return 0;
}

#undef recv
int wsa_recv(int socket, void * buf, size_t size, int flags) {
    int res = 0;
    SetLastError(0);
    WSASetLastError(0);
    res = recv(socket, buf, size, flags);
    if (res < 0) {
        set_win32_errno(WSAGetLastError());
        return -1;
    }
    return res;
}

#undef recvfrom
int wsa_recvfrom(int socket, void * buf, size_t size, int flags,
                 struct sockaddr * addr, socklen_t * addr_size) {
    int res = 0;
    SetLastError(0);
    WSASetLastError(0);
    res = recvfrom(socket, buf, size, flags, addr, addr_size);
    if (res < 0) {
        set_win32_errno(WSAGetLastError());
        return -1;
    }
    return res;
}

#undef send
int wsa_send(int socket, const void * buf, size_t size, int flags) {
    int res = 0;
    SetLastError(0);
    WSASetLastError(0);
    res = send(socket, buf, size, flags);
    if (res < 0) {
        set_win32_errno(WSAGetLastError());
        return -1;
    }
    return res;
}

#undef sendto
int wsa_sendto(int socket, const void * buf, size_t size, int flags,
               const struct sockaddr * dest_addr, socklen_t dest_size) {
    int res = 0;
    SetLastError(0);
    WSASetLastError(0);
    res = sendto(socket, buf, size, flags, dest_addr, dest_size);
    if (res < 0) {
        set_win32_errno(WSAGetLastError());
        return -1;
    }
    return res;
}

#endif /* WIN32 */

#if defined(WIN32) && defined(_MSC_VER)

static __int64 file_time_to_unix_time (const FILETIME * ft) {
    __int64 res = (__int64)ft->dwHighDateTime << 32;

    res |= ft->dwLowDateTime;
    res /= 10;                  /* from 100 nano-sec periods to usec */
    res -= 11644473600000000u;  /* from Win epoch to Unix epoch */
    return res;
}

int clock_gettime(clockid_t clock_id, struct timespec * tp) {
    FILETIME ft;
    __int64 tim;

    assert(clock_id == CLOCK_REALTIME);
    if (!tp) {
        errno = EINVAL;
        return -1;
    }
    GetSystemTimeAsFileTime(&ft);
    tim = file_time_to_unix_time(&ft);
    tp->tv_sec  = (long)(tim / 1000000L);
    tp->tv_nsec = (long)(tim % 1000000L) * 1000;
    return 0;
}

void usleep(useconds_t useconds) {
    Sleep(useconds / 1000);
}

int inet_aton(const char *cp, struct in_addr *inp) {
    return ( inp->s_addr = inet_addr(cp) ) != INADDR_NONE;
}

int truncate(const char * path, int64 size) {
    int res = 0;
    int f = _open(path, _O_RDWR | _O_BINARY);
    if (f < 0) return -1;
    res = ftruncate(f, size);
    _close(f);
    return res;
}

int ftruncate(int fd, int64 size) {
    int64 cur, pos;
    BOOL ret = FALSE;
    HANDLE handle = (HANDLE)_get_osfhandle(fd);

    if (handle == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    /* save the current file pointer */
    cur = _lseeki64(fd, 0, SEEK_CUR);
    if (cur >= 0) {
        pos = _lseeki64(fd, size, SEEK_SET);
        if (pos >= 0) {
            ret = SetEndOfFile(handle);
            if (!ret) errno = EBADF;
        }
        /* restore the file pointer */
        _lseeki64(fd, cur, SEEK_SET);
    }
    return ret ? 0 : -1;
}

DIR * opendir(const char *path) {
    DIR * d = (DIR *)loc_alloc(sizeof(DIR));
    if (!d) { errno = ENOMEM; return 0; }
    strcpy(d->path, path);
    strcat(d->path, "/*.*");
    d->hdl = -1;
    return d;
}

struct dirent * readdir(DIR *d) {
    static struct dirent de;
    if (d->hdl < 0) {
        d->hdl = _findfirsti64(d->path, &d->blk);
        if (d->hdl < 0) {
            if (errno == ENOENT) errno = 0;
            return 0;
        }
    }
    else {
        int r = _findnexti64(d->hdl, &d->blk);
        if (r < 0) {
            if (errno == ENOENT) errno = 0;
            return 0;
        }
    }
    strcpy(de.d_name, d->blk.name);
    de.d_size = d->blk.size;
    de.d_atime = d->blk.time_access;
    de.d_ctime = d->blk.time_create;
    de.d_wtime = d->blk.time_write;
    return &de;
}

int closedir(DIR * d) {
    int r = 0;
    if (!d) {
        errno = EBADF;
        return -1;
    }
    if (d->hdl >= 0) r = _findclose(d->hdl);
    loc_free(d);
    return r;
}

int getuid(void) {
    /* Windows user is always a superuser :) */
    return 0;
}

int geteuid(void) {
    return 0;
}

int getgid(void) {
    return 0;
}

int getegid(void) {
    return 0;
}
#endif


#if defined(WIN32)

#include <shlobj.h>

unsigned char BREAK_INST[] = { 0xcc };

char * get_os_name(void) {
    static char str[256];
    OSVERSIONINFOEX info;
    memset(&info, 0, sizeof(info));
    info.dwOSVersionInfoSize = sizeof(info);
    GetVersionEx((OSVERSIONINFO *)&info);
    switch (info.dwMajorVersion) {
    case 4:
        return "Windows NT";
    case 5:
        switch (info.dwMinorVersion) {
        case 0: return "Windows 2000";
        case 1: return "Windows XP";
        case 2: return "Windows Server 2003";
        }
        break;
    case 6:
        return "Windows Vista";
    }
    snprintf(str, sizeof(str), "Windows %d.%d", info.dwMajorVersion, info.dwMinorVersion);
    return str;
}

char * get_user_home(void) {
    static char buf[MAX_PATH];
    if (buf[0] != 0) return buf;
    if (SUCCEEDED(SHGetFolderPath(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, buf))) return buf;
    return NULL;
}

void ini_mdep(void) {
    WORD wVersionRequested;
    WSADATA wsaData;
    int err;
    wVersionRequested = MAKEWORD( 1, 1 );
    err = WSAStartup( wVersionRequested, &wsaData );
    if ( err != 0 ) {
        fprintf(stderr, "Couldn't access winsock.dll.\n");
        exit(1);
    }
    /* Confirm that the Windows Sockets DLL supports 1.1.*/
    /* Note that if the DLL supports versions greater */
    /* than 1.1 in addition to 1.1, it will still return */
    /* 1.1 in wVersion since that is the version we */
    /* requested.     */
    if (LOBYTE( wsaData.wVersion ) != 1 || HIBYTE( wsaData.wVersion ) != 1) {
        fprintf(stderr, "Unacceptable version of winsock.dll.\n");
        WSACleanup();
        exit(1);
    }
    pthread_attr_init(&pthread_create_attr);
}

#elif defined(_WRS_KERNEL)

void usleep(useconds_t useconds) {
    struct timespec tv;
    tv.tv_sec = useconds / 1000000;
    tv.tv_nsec = (useconds % 1000000) * 1000;
    nanosleep(&tv, NULL);
}

int truncate(char * path, int64 size) {
    int f = open(path, O_RDWR, 0);
    if (f < 0) return -1;
    if (ftruncate(f, size) < 0) {
        int err = errno;
        close(f);
        errno = err;
        return -1;
    }
    return close(f);
}

int getuid(void) {
    return 0;
}

int geteuid(void) {
    return 0;
}

int getgid(void) {
    return 0;
}

int getegid(void) {
    return 0;
}

char * get_os_name(void) {
    static char str[256];
    snprintf(str, sizeof(str), "VxWorks %s", kernelVersion());
    return str;
}

char * get_user_home(void) {
    return "/";
}

void ini_mdep(void) {
    pthread_attr_init(&pthread_create_attr);
    pthread_attr_setstacksize(&pthread_create_attr, 0x4000);
    pthread_attr_setname(&pthread_create_attr, "tTcf");
}

#else

#include <pwd.h>
#include <sys/utsname.h>
#include <asm/unistd.h>

unsigned char BREAK_INST[] = { 0xcc };

char * get_os_name(void) {
    static char str[256];
    struct utsname info;
    memset(&info, 0, sizeof(info));
    uname(&info);
    assert(strlen(info.sysname) + strlen(info.release) < sizeof(str));
    snprintf(str, sizeof(str), "%s %s", info.sysname, info.release);
    return str;
}

char * get_user_home(void) {
    static char buf[PATH_MAX];
    if (buf[0] == 0) {
        struct passwd * pwd = getpwuid(getuid());
        if (pwd == NULL) return NULL;
        strcpy(buf, pwd->pw_dir);
    }
    return buf;
}

int tkill(pid_t pid, int signal) {
    return syscall(__NR_tkill, pid, signal);
}

void ini_mdep(void) {
    pthread_attr_init(&pthread_create_attr);
    pthread_attr_setstacksize(&pthread_create_attr, 0x8000);
}

#endif


/** canonicalize_file_name ****************************************************/

#if defined(WIN32)

char * canonicalize_file_name(const char * path) {
    char buf[MAX_PATH];
    char * basename;
    int i = 0;
    DWORD len = GetFullPathName(path, sizeof(buf), buf, &basename);
    if (len == 0) {
        errno = ENOENT;
        return NULL;
    }
    if (len > MAX_PATH - 1) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    while (buf[i] != 0) {
        if (buf[i] == '\\') buf[i] = '/';
        i++;
    }
    return strdup(buf);
}

#elif defined(_WRS_KERNEL)

char * canonicalize_file_name(const char * path) {
    char buf[PATH_MAX];
    int i = 0, j = 0;
    if (path[0] == '.' && (path[1] == '/' || path[1] == '\\' || path[1] == 0)) {
        getcwd(buf, sizeof(buf));
        j = strlen(buf);
        if (j == 1 && buf[0] == '/') j = 0;
        i = 1;
    }
    else if (path[0] == '.' && path[1] == '.' && (path[2] == '/' || path[2] == '\\' || path[2] == 0)) {
        getcwd(buf, sizeof(buf));
        j = strlen(buf);
        while (j > 0 && buf[j - 1] != '/') j--;
        if (j > 0 && buf[j - 1] == '/') j--;
        i = 2;
    }
    while (path[i] && j < PATH_MAX - 1) {
        char ch = path[i];
        if (ch == '\\') ch = '/';
        if (ch == '/') {
            if (path[i + 1] == '/' || path[i + 1] == '\\') {
                i++;
                continue;
            }
            if (path[i + 1] == '.') {
                if (path[i + 2] == 0) {
                    break;
                }
                if (path[i + 2] == '/' || path[i + 2] == '\\') {
                    i += 2;
                    continue;
                }
                if ((j == 0 || buf[0] == '/') && path[i + 2] == '.') {
                    if (path[i + 3] == '/' || path[i + 3] == '\\' || path[i + 3] == 0) {
                        while (j > 0 && buf[j - 1] != '/') j--;
                        if (j > 0 && buf[j - 1] == '/') j--;
                        i += 3;
                        continue;
                    }
                }
            }
        }
        buf[j++] = ch;
        i++;
    }
    if (j == 0 && path[0] != 0) buf[j++] = '/';
    buf[j] = 0;
    return strdup(buf);
}

#endif


/** getaddrinfo ***************************************************************/

#if defined(_WRS_KERNEL) && defined(USE_VXWORKS_GETADDRINFO)

/* TODO: VxWorks 6.6 getaddrinfo returns error when port is empty string, should return port 0 */
/* TODO: VxWorks 6.6 source (as shipped at 2007 fall release) does not include ipcom header files. */
extern void ipcom_freeaddrinfo();
extern int ipcom_getaddrinfo();

static struct ai_errlist {
    const char * str;
    int code;
} ai_errlist[] = {
    { "Success", 0 },
    /*
    { "Invalid value for ai_flags", IP_EAI_BADFLAGS },
    { "Non-recoverable failure in name resolution", IP_EAI_FAIL },
    { "ai_family not supported", IP_EAI_FAMILY },
    { "Memory allocation failure", IP_EAI_MEMORY },
    { "hostname nor servname provided, or not known", IP_EAI_NONAME },
    { "servname not supported for ai_socktype",     IP_EAI_SERVICE },
    { "ai_socktype not supported", IP_EAI_SOCKTYPE },
    { "System error returned in errno", IP_EAI_SYSTEM },
     */
    /* backward compatibility with userland code prior to 2553bis-02 */
    { "Address family for hostname not supported", 1 },
    { "No address associated with hostname", 7 },
    { NULL, -1 },
};

void loc_freeaddrinfo(struct addrinfo * ai) {
    ipcom_freeaddrinfo(ai);
}

int loc_getaddrinfo(const char * nodename, const char * servname,
       const struct addrinfo * hints, struct addrinfo ** res) {
    return ipcom_getaddrinfo(nodename, servname, hints, res);
}

const char * loc_gai_strerror(int ecode) {
    struct ai_errlist * p;
    static char buf[32];
    for (p = ai_errlist; p->str; p++) {
        if (p->code == ecode) return p->str;
    }
    snprintf(buf, sizeof(buf), "Error code %d", ecode);
    return buf;
}

#elif defined(_WRS_KERNEL)

union sockaddr_union {
    struct sockaddr sa;
    struct sockaddr_in sin;
    struct sockaddr_in6 sin6;
};

extern int ipcom_getsockaddrbyaddr();

void loc_freeaddrinfo(struct addrinfo * ai) {
    while (ai != NULL) {
        struct addrinfo * next = ai->ai_next;
        if (ai->ai_canonname != NULL) loc_free(ai->ai_canonname);
        if (ai->ai_addr != NULL) loc_free(ai->ai_addr);
        loc_free(ai);
        ai = next;
    }
}

int loc_getaddrinfo(const char * nodename, const char * servname,
       const struct addrinfo * hints, struct addrinfo ** res) {
    int family = 0;
    int flags = 0;
    int socktype = 0;
    int protocol = 0;
    int err = 0;
    int port = 0;
    char * canonname = NULL;
    const char * host = NULL;
    struct addrinfo * ai = NULL;
    union sockaddr_union * sa = NULL;
    
    *res = NULL;
    
    if (hints != NULL) {
        flags = hints->ai_flags;
        family = hints->ai_family;
        socktype = hints->ai_socktype;
        protocol = hints->ai_protocol;
    }
    if (family == AF_UNSPEC) {
        struct addrinfo lhints;
        struct addrinfo ** tmp;
        int err_v6;

        if (hints == NULL) memset(&lhints, 0, sizeof(lhints));
        else memcpy(&lhints, hints, sizeof(lhints));
        lhints.ai_family = AF_INET6;
        err_v6 = loc_getaddrinfo(nodename, servname, &lhints, res);
        lhints.ai_family = AF_INET;
        while (*res != NULL) res = &(*res)->ai_next;
        err = loc_getaddrinfo(nodename, servname, &lhints, res);
        return err && err_v6 ? err : 0;
    }
    if (servname != NULL && servname[0] != 0) {
        char * p = NULL;
        port = strtol(servname, &p, 10);
        if (port < 0 || port > 0xffff || *p != '\0' || p == servname) {
            return 1;
        }
    }
    if (nodename != NULL && nodename[0] != 0) {
        host = nodename;
    }
    else if (flags & AI_PASSIVE) {
        host = family == AF_INET ? "0.0.0.0" : "::";
    }
    else {
        host = family == AF_INET ? "127.0.0.1" : "::1";
    }
    if (socktype == 0) {
        socktype = SOCK_STREAM;
    }
    if (protocol == 0) {
        protocol = socktype == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
    }
    
    sa = loc_alloc_zero(sizeof(*sa));
    err = ipcom_getsockaddrbyaddr(family, host, (struct sockaddr *)sa);
    if (err) {
        loc_free(sa);
        return err;
    }
    
    ai = loc_alloc_zero(sizeof(*ai));
    switch (family) {
    case AF_INET:
        assert(sa->sin.sin_family == AF_INET);
        sa->sin.sin_port = htons(port);
        ai->ai_addrlen = sizeof(struct sockaddr_in);
        break;
    case AF_INET6:
        assert(sa->sin6.sin6_family == AF_INET6);
        sa->sin6.sin6_port = htons(port);
        ai->ai_addrlen = sizeof(struct sockaddr_in6);
        break;
    default:
        loc_free(sa);
        loc_free(ai);
        return 2;
    }
    
    ai->ai_flags = 0;
    ai->ai_family = family;
    ai->ai_socktype = socktype;
    ai->ai_protocol = protocol;
    ai->ai_canonname = canonname;
    ai->ai_addr = (struct sockaddr *)sa;
    ai->ai_next = NULL;
    *res = ai;
    return 0;
}

const char * loc_gai_strerror(int ecode) {
    static char buf[32];
    if (ecode == 0) return "Success";
    snprintf(buf, sizeof(buf), "Error code %d", ecode);
    return buf;
}

#elif defined(WIN32) && defined(__GNUC__)

const char * loc_gai_strerror(int ecode) {
    static char buf[128];
    if (ecode == 0) return "Success";
    FormatMessage(
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS |
        FORMAT_MESSAGE_MAX_WIDTH_MASK,
        NULL,
        ecode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        buf,
        sizeof(buf),
        NULL);
    return buf;
}

#endif
