#include <alloca.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/file.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* The execXXX functions in POSIX are async-signal-safe.  (e.g. After a process
 * forks, its heap can be in an inconsistent state if another thread was in a
 * malloc call, but it's OK to call exec because exec does not care about the
 * heap consistency.)  Therefore, these execXXX wrappers must also be
 * async-signal-safe.
 *
 * There are several lists of async-safe functions, and most of them leave out
 * functions like strcpy and sprintf, so avoid using them in the wrapper
 * functions.  Define guaranteed-safe versions instead. */

#define BTRACE_LOG_ENV_VAR "BTRACE_LOG"

/* Circumvent a compiler optimization.  The fix to glibc bugzilla issue
 * #11056 modified the execl* family of system calls by marking the arg
 * parameter with __nonnull.  Contrary to the summary of that issue:
 *  - POSIX says that the parameter *should* be a filename, not that it
 *    *shall* be a filename.
 *  - It is widespread practice on UNIX for argv[0] to be a non-filename like
 *    "-bash".
 * Moreover:
 *  - The execve(2) Linux man page documents that both a NULL argv and an argv
 *    pointing to an empty list are acceptable:
 *        On Linux, argv can be specified as NULL, which has the same effect
 *        as specifying this argument as a pointer to a list containing a
 *        single NULL pointer.  Do not take advantage of this misfeature!  It
 *        is nonstandard and nonportable: on most other UNIX systems doing
 *        this will result in an error (EFAULT).
 *  - Even if arg shall not be NULL, the right behavior would be to return
 *    an error code, not to invoke undefined behavior.  As the manpage says,
 *    like "most other UNIX systems."
 * In any case, the result of the __nonnull marking is that GCC optimizes out
 * the (arg != NULL) checks here in this code.  Block the optimization by
 * exploiting the lack of whole-program compile-time optimization.  This symbol
 * will be hidden by the linker version script. */
void *libbtrace_c_NULL_constant = NULL;
#define hiddenNull libbtrace_c_NULL_constant

static void *safeMemSet(void *s, int c, size_t n)
{
    char *dest = (char*)s;
    while (n > 0) {
        *dest++ = (char)c;
        n--;
    }
    return s;
}

static char *safeStrChr(const char *s, int c)
{
    while (true) {
        char ch = *s;
        if (ch == c) {
            /* If c is NUL, return a pointer to the NUL terminator. */
            return (char*)s;
        } else if (ch == '\0') {
            return NULL;
        }
        s++;
    }
}

static char *safeStrRChr(const char *s, int c)
{
    char *ret = NULL;
    while (true) {
        char ch = *s;
        if (ch == c) {
            /* If c is NUL, return a pointer to the NUL terminator. */
            ret = (char*)s;
        }
        if (ch == '\0')
            return ret;
        s++;
    }
}

static size_t safeStrLen(const char *str)
{
    size_t i = 0;
    while (str[i] != '\0')
        ++i;
    return i;
}

static int safeStrNCmp(const char *s1, const char *s2, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        unsigned char ch1 = s1[i];
        unsigned char ch2 = s2[i];
        if (ch1 != ch2 || ch1 == '\0' || ch2 == '\0')
            return ch1 - ch2;
    }
    return 0;
}

static char *safeStrCpy(char *dest, const char *src)
{
    size_t i = 0;
    do {
        dest[i] = src[i];
    } while(src[i++] != '\0');
    return dest;
}

static void uint64ToString(char *output, uint64_t val)
{
    char tempBuf[32];
    char *ptr = &tempBuf[31];
    *ptr = '\0';
    do {
        *(--ptr) = '0' + val % 10;
        val /= 10;
    } while (val != 0);
    safeStrCpy(output, ptr);
}

static inline bool safeIsDigit(char ch)
{
    return ch >= '0' && ch <= '9';
}

static uint64_t stringToUint64(const char *input)
{
    uint64_t temp = 0;
    while (safeIsDigit(*input)) {
        temp = temp * 10 + (*input - '0');
        input++;
    }
    return temp;
}

/* sprintf is not documented as async-signal-safe. */
/* parts is terminated by a NULL pointer. */
static size_t strListLen(const char *parts[])
{
    size_t total = 0;
    for (int i = 0; parts[i] !=  NULL; ++i)
        total += safeStrLen(parts[i]);
    return total + 1;
}

/* sprintf is not documented as async-signal-safe. */
/* parts is terminated by a NULL pointer. */
static void strListCat(char *target, const char *parts[])
{
    for (int i = 0; parts[i] !=  NULL; ++i) {
        safeStrCpy(target, parts[i]);
        target += safeStrLen(parts[i]);
    }
}

/* parts is terminated by a NULL pointer. */
static void writeError(const char *parts[])
{
    char message[strListLen(parts)];
    strListCat(message, parts);
    write(STDERR_FILENO, message, safeStrLen(message));
}

static void safeAssertFail(
        const char *filename,
        int line,
        const char *func,
        const char *condition)
{
    char lineString[32];
    uint64ToString(lineString, line);
    writeError((const char*[]) {
        "libbtrace.so: ", filename, ":", lineString, ": ",
        func, ": Assertion `", condition, "' failed.\n",
        NULL });
    abort();
}

#define safeAssert(condition) \
        if (!(condition)) \
            safeAssertFail(__FILE__, __LINE__, \
                           __FUNCTION__, #condition)

#define EINTR_LOOP(expr)                        \
    ({                                          \
        __typeof__(expr) ret;                   \
        do {                                    \
            ret = (expr);                       \
        } while (ret == -1 && errno == EINTR);  \
        ret;                                    \
    })

static int (*real_execve)(const char*, char *const[], char *const[]) = NULL;
static int (*real_execvpe)(const char*, char *const[], char *const[]) = NULL;
static char g_logFileName[1024];
static uint64_t g_bootTimeInJiffies;

void _init(void)
{
    /* dlsym is not async-signal-safe, so try calling it up-front in _init
     * rather than lazily. */
    real_execve = dlsym(RTLD_NEXT, "execve");
    real_execvpe = dlsym(RTLD_NEXT, "execvpe");
    safeAssert(real_execve != NULL);
    safeAssert(real_execvpe != NULL);

    const char *logFileVar = getenv(BTRACE_LOG_ENV_VAR);
    if (logFileVar != NULL && safeStrLen(logFileVar) < sizeof(g_logFileName))
        safeStrCpy(g_logFileName, logFileVar);

    /* Determine the boot time in jiffies. */
    {
        const int jiffiesPerSecond = sysconf(_SC_CLK_TCK);
        safeAssert(jiffiesPerSecond >= 1);

        int fd = open("/proc/stat", O_RDONLY | O_CLOEXEC);
        safeAssert(fd != -1 && "Error opening /proc/stat");
        FILE *fp = fdopen(fd, "r");
        safeAssert(fp != NULL && "Error opening /proc/stat");
        char *line = NULL;
        size_t lineSize = 0;
        g_bootTimeInJiffies = 0;
        while (getline(&line, &lineSize, fp) != -1) {
            if (safeStrNCmp(line, "btime ", 6) == 0) {
                g_bootTimeInJiffies =
                        stringToUint64(line + 6) * jiffiesPerSecond;
                safeAssert(g_bootTimeInJiffies > 0);
                break;
            }
        }
        safeAssert(!ferror(fp) && "Error reading /proc/stat");
        safeAssert(g_bootTimeInJiffies > 0 && "btime missing from /proc/stat");
        free(line);
        fclose(fp);
    }
}

typedef struct {
    int fd;
    char buf[1024];
    int bufCount;
} LogFile;

static void openLogFile(LogFile *logFile, const char *filename)
{
    logFile->fd = 0;
    logFile->bufCount = 0;
    errno = 0;
    logFile->fd = EINTR_LOOP(open(
            filename, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644));
    safeAssert(logFile->fd != -1 && "Error opening trace file for append.");
    struct flock lock;
    safeMemSet(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;
    int lockRet = EINTR_LOOP(fcntl(logFile->fd, F_SETLKW, &lock));
    safeAssert(lockRet == 0 && "Error locking trace file.");
}

static void flushLogFile(LogFile *logFile)
{
    ssize_t amount = EINTR_LOOP(
                write(logFile->fd, logFile->buf, logFile->bufCount));
    safeAssert(amount == logFile->bufCount && "Error writing to trace file.");
    logFile->bufCount = 0;
}

static inline void writeChar(LogFile *logFile, char ch)
{
    if (logFile->bufCount == sizeof(logFile->buf))
        flushLogFile(logFile);
    logFile->buf[logFile->bufCount++] = ch;
}

static void closeLogFile(LogFile *logFile)
{
    flushLogFile(logFile);
    struct flock lock;
    safeMemSet(&lock, 0, sizeof(lock));
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;
    int unlockRet = fcntl(logFile->fd, F_SETLK, &lock);
    safeAssert(unlockRet == 0 && "Error unlocking trace file.");
    close(logFile->fd);
}

static void writeString(LogFile *logFile, const char *text)
{
    for (; text[0] != '\0'; ++text)
        writeChar(logFile, text[0]);
}

/* Write the string to the LogFile.  Spaces and newlines are quoted.
 * Backslashes and quotes are escaped. */
static void writeEscapedString(LogFile *logFile, const char *text)
{
    bool needsQuotes =
        (safeStrChr(text, ' ') != NULL ||
         safeStrChr(text, '\n') != NULL);
    if (needsQuotes)
        writeChar(logFile, '\"');
    for (int i = 0; text[i] != '\0'; ++i) {
        char ch = text[i];
        if (ch == '\\' || ch == '\"')
            writeChar(logFile, '\\');
        writeChar(logFile, ch);
    }
    if (needsQuotes)
        writeChar(logFile, '\"');
}

static bool attemptWriteSymLinkTarget(
        LogFile *logFile,
        const char *symLinkTarget,
        size_t size)
{
    char *buf = alloca(size);
    ssize_t amount = readlink(symLinkTarget, buf, size);
    if (amount == -1) {
        writeError((const char*[]) {
            "libbtrace.so: Error calling readlink on ", symLinkTarget, NULL});
        abort();
    }
    if ((size_t)amount < size) {
        buf[amount] = '\0';
        writeEscapedString(logFile, buf);
        writeChar(logFile, '\n');
        return true;
    } else {
        safeAssert((size_t)amount == size &&
                   "Invalid return value from readlink");
        return false;
    }
}

static void writeSymLinkTarget(
        LogFile *logFile,
        const char *symLinkTarget)
{
    bool success = false;
    size_t bufSize = 256;
    while (bufSize < 1024 * 1024) {
        if (attemptWriteSymLinkTarget(logFile, symLinkTarget, bufSize)) {
            success = true;
            break;
        }
        bufSize <<= 1;
    }
    if (!success) {
        writeError((const char*[]) {
            "libbtrace.so: Error reading symlink ", symLinkTarget});
        abort();
    }
}

/* Write two lines to the logfile:
 *  - The given pid.
 *  - The start-time of the pid, measured in jiffies since the epoch. */
static void writePid(LogFile *logFile, uint32_t pid)
{
    char pidString[32];
    int statFd;

    {
        uint64ToString(pidString, pid);
        writeString(logFile, pidString);
        writeChar(logFile, '\n');
    }
    {
        char procStatPath[64];
        strListCat(procStatPath, (const char*[]) {
                "/proc/", pidString, "/stat", NULL });
        statFd = open(procStatPath, O_RDONLY | O_CLOEXEC);
        if (statFd == -1) {
            writeError((const char*[]) {
                    "libbtrace.so: Error opening ", procStatPath, NULL });
            abort();
        }
    }
    {
        /* Read the process start time from /proc/<pid>/stat.  The start time
         * is recorded as a number-of-jiffies-since-boot in field 22.  Parsing
         * the field is complicated because the second field of the stat line
         * is an unescaped executable name.  The name is parenthesized, but it
         * can contain parentheses.  The 'ps' utility parses it by searching
         * for the last ')' character in the stat-line, which it assumes is the
         * end of the executable name field. */

        /* The maximum size of the file through field 22 easily fits in 1024
         * bytes, so use a fixed-size buffer of that size.  Incidentally, ps
         * also uses a 1024-byte buffer.  Implementing read-buffering is not
         * worth the trouble.
         *
         * Size of fields 1..22 (see man proc):
              D = 12        [len(2 ** 31) + 1 for sign + 1 for space]
              U = 11        [len(2 ** 32 - 1) + 1 for space]
              LD = 21       [len(2 ** 63) + 1 for sign + 1 for space]
              LU = 21       [len(2 ** 64 - 1) + 1 for space]
              LLU = LU
              COMM = 16     [measured]
              D + COMM + 2 + 5 * D + U + 6 * LU + 6 * LD + LLU
         */
        char procStatContent[1024];
        ssize_t amt = read(statFd, procStatContent, sizeof(procStatContent) - 1);
        safeAssert(amt >= 0 && "Error reading /proc/<pid>/stat");
        procStatContent[amt] = '\0';
        const char *p = safeStrRChr(procStatContent, ')');
        safeAssert(p != NULL && "No ')' found in /proc/<pid>/stat");
        p += 2; /* Skip ") ". */

        /* p now points at field 3.  Advance to field 22. */
        for (int i = 3; i < 22; ++i) {
            p = safeStrChr(p, ' ');
            safeAssert(p != NULL &&
                    "Could not find starttime field in proc/<pid>/stat");
            p++;
        }
        char *pEnd = safeStrChr(p, ' ');
        safeAssert(pEnd != NULL &&
                "Could not find end of starttime field in /proc/<pid>/stat");

        /* Write the start-time. */
        char startTime[32];
        uint64ToString(startTime, g_bootTimeInJiffies + stringToUint64(p));
        writeString(logFile, startTime);
        writeChar(logFile, '\n');
    }
}

static void logExecution(const char *filename, char *const argv[])
{
    if (g_logFileName[0] == '\0')
        return;
    LogFile logFile;
    openLogFile(&logFile, g_logFileName);

    /* Each exec record is a series of lines in this order:
     *  - "exec"
     *  - parent pid
     *  - parent jiffies-since-epoch
     *  - self pid
     *  - self jiffies-since-epoch
     *  - cwd
     *  - exec filename
     *  - command-line
     *  - blank line */

    writeString(&logFile, "exec\n");
    writePid(&logFile, getppid());
    writePid(&logFile, getpid());
    writeSymLinkTarget(&logFile, "/proc/self/cwd");
    writeEscapedString(&logFile, filename);
    writeChar(&logFile, '\n');

    /* Write arguments. */
    if (argv != hiddenNull) {
        for (int i = 0; argv[i] != hiddenNull; ++i) {
            if (i > 0)
                writeChar(&logFile, ' ');
            writeEscapedString(&logFile, argv[i]);
        }
    }
    writeChar(&logFile, '\n');

    /* Write blank line. */
    writeChar(&logFile, '\n');

    closeLogFile(&logFile);
}

static int wrap_execve(
        const char *path,
        char *const argv[],
        char *const envp[])
{
    logExecution(path, argv);
    return real_execve(path, argv, envp);
}

static int wrap_execvpe(
        const char *file,
        char *const argv[],
        char *const envp[])
{
    logExecution(file, argv);
    return real_execvpe(file, argv, envp);
}

int execv(const char *path, char *const argv[])
{
    return wrap_execve(path, argv, environ);
}

int execvp(const char *file, char *const argv[])
{
    return wrap_execvpe(file, argv, environ);
}

int execve(const char *path,
           char *const argv[],
           char *const envp[])
{
    return wrap_execve(path, argv, envp);
}

int execvpe(const char *file,
            char *const argv[],
            char *const envp[])
{
    return wrap_execvpe(file, argv, envp);
}

#define ARG_COUNT(ARG_PARAM) ({                                 \
        int COUNT = 0;                                          \
        if (ARG_PARAM != hiddenNull) {                          \
            va_list AP;                                         \
            va_start(AP, ARG_PARAM);                            \
            COUNT++;                                            \
            while (va_arg(AP, const char*) != NULL)             \
                COUNT++;                                        \
            va_end(AP);                                         \
        }                                                       \
        COUNT;                                                  \
    })

int execl(const char *path, const char *arg, ...)
{
    int count = ARG_COUNT(arg);
    char **argv = alloca(sizeof(char*) * (count + 1));
    safeAssert(argv);
    va_list ap;
    va_start(ap, arg);
    if (arg != hiddenNull) {
        /* Casting away const here is safe.
         * http://stackoverflow.com/questions/190184/execv-and-const-ness */
        argv[0] = (char*)arg;
        for (int i = 1; i < count; ++i)
            argv[i] = va_arg(ap, char*);
    }
    argv[count] = NULL;
    va_end(ap);
    return wrap_execve(path, argv, environ);
}

int execlp(const char *file, const char *arg, ...)
{
    int count = ARG_COUNT(arg);
    char **argv = alloca(sizeof(char*) * (count + 1));
    safeAssert(argv);
    va_list ap;
    va_start(ap, arg);
    if (arg != hiddenNull) {
        /* Casting away const here is safe.
         * http://stackoverflow.com/questions/190184/execv-and-const-ness */
        argv[0] = (char*)arg;
        for (int i = 1; i < count; ++i)
            argv[i] = va_arg(ap, char*);
    }
    argv[count] = NULL;
    va_end(ap);
    return wrap_execvpe(file, argv, environ);
}

int execle(const char *path, const char *arg, ...)
{
    int count = ARG_COUNT(arg);
    char **argv = alloca(sizeof(char*) * (count + 1));
    safeAssert(argv);
    va_list ap;
    va_start(ap, arg);
    if (arg != hiddenNull) {
        /* Casting away const here is safe.
         * http://stackoverflow.com/questions/190184/execv-and-const-ness */
        argv[0] = (char*)arg;
        for (int i = 1; i < count; ++i)
            argv[i] = va_arg(ap, char*);
        va_arg(ap, char*);
    }
    argv[count] = NULL;
    char **envp = va_arg(ap, char**);
    va_end(ap);
    return wrap_execve(path, argv, envp);
}
