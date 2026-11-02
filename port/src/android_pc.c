/**
 * Android glue (the armeabi-v7a APK, android/README.md). SDL's Java activity
 * loads libmain.so and calls SDL_main below, which sets up what a desktop
 * launch gets for free and then runs main.c's main (portMain here):
 *
 *  - The root dir (main.c portRootPath) is the app's external files dir,
 *    /storage/emulated/0/Android/data/<package>/files/: midas.ini, and Data/
 *    with the cart image, saves and logs. It is reachable over USB and adb
 *    without any storage permission.
 *  - There is no console, so a thread copies stdout and stderr into logcat
 *    (tag ProjectMidas) and Data/Logs/ge_direct.log, like the Windows
 *    direct-launch log.
 *  - The files a desktop release ships beside the exe (controller mappings
 *    and layouts, the ini help, Data/shaders_base.txt) travel as APK assets
 *    and are copied into the root dir on every start. The player's own files
 *    (gamecontrolleruser.txt, Data/shaders.txt, ...) are never touched.
 *  - SDL calls SDL_main on a Java thread with a ~1 MB stack. The game's
 *    recursive room/portal walks overflow that (the Windows exe links a
 *    16 MB stack), so the game runs on a thread of its own.
 *  - That thread asks for the fastest cores a 32-bit process may use. Left
 *    alone the scheduler keeps it on the efficiency cores (on a Snapdragon
 *    8 Gen 2, the A510s rather than the A710s): GL time per frame in the
 *    attract demos was 5-13 ms there and 2-3 ms on the fast cores. The threads
 *    it starts (render, audio) inherit the mask.
 */
#ifdef __ANDROID__
#define _GNU_SOURCE /* sched_setaffinity, CPU_SET */
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <android/log.h>
#include <SDL.h>

#define PORT_ANDROID_LOG_TAG "ProjectMidas"
#define PORT_ANDROID_STACK (16u * 1024u * 1024u)

extern int portMain(int argc, char **argv); /* main.c's main */

static char s_Root[1024];
static FILE *s_LogFile;
static int s_LogPipe[2];

const char *portAndroidRootDir(void)
{
    return s_Root;
}

static void portAndroidMkdir(const char *rel)
{
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", s_Root, rel);
    mkdir(path, 0755); /* fails harmlessly when it exists */
}

static void portAndroidInitRoot(void)
{
    const char *dir = SDL_AndroidGetExternalStoragePath();
    if (dir == NULL || dir[0] == '\0') {
        /* no external storage mounted: the private dir still works, it is
         * just out of the user's reach */
        dir = SDL_AndroidGetInternalStoragePath();
    }
    snprintf(s_Root, sizeof(s_Root), "%s", dir != NULL ? dir : ".");
    mkdir(s_Root, 0755);
    portAndroidMkdir("Data");
    portAndroidMkdir("Data/Logs");
}

/* Forward the stdout/stderr pipe a line at a time. (The C TUs see the N64
 * <string.h> stub, so no strlen.) */
static void *portAndroidLogThread(void *arg)
{
    char line[2048];
    FILE *in = fdopen(s_LogPipe[0], "r");
    (void) arg;
    if (in == NULL) {
        return NULL;
    }
    while (fgets(line, sizeof(line), in) != NULL) {
        int n = 0;
        if (s_LogFile != NULL) {
            fputs(line, s_LogFile);
            fflush(s_LogFile);
        }
        while (line[n] != '\0') {
            n++;
        }
        if (n > 0 && line[n - 1] == '\n') {
            line[n - 1] = '\0';
        }
        __android_log_write(ANDROID_LOG_INFO, PORT_ANDROID_LOG_TAG, line);
    }
    return NULL;
}

static void portAndroidStartLog(void)
{
    char path[1200];
    pthread_t thread;

    snprintf(path, sizeof(path), "%s/Data/Logs/ge_direct.log", s_Root);
    s_LogFile = fopen(path, "w");
    if (pipe(s_LogPipe) != 0) {
        __android_log_write(ANDROID_LOG_ERROR, PORT_ANDROID_LOG_TAG, "pipe() failed: no game log");
        return;
    }
    dup2(s_LogPipe[1], STDOUT_FILENO);
    dup2(s_LogPipe[1], STDERR_FILENO);
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (pthread_create(&thread, NULL, portAndroidLogThread, NULL) == 0) {
        pthread_detach(thread);
    }
}

/* Copy one APK asset to the same path under the root dir. A relative path
 * given to SDL_RWFromFile reads from the APK's assets. */
static void portAndroidUnpack(const char *name)
{
    char path[1200];
    char buf[16384];
    size_t n;
    FILE *out;
    SDL_RWops *in = SDL_RWFromFile(name, "rb");

    if (in == NULL) {
        fprintf(stderr, "ge: android: %s is not in the APK (%s)\n", name, SDL_GetError());
        return;
    }
    snprintf(path, sizeof(path), "%s/%s", s_Root, name);
    out = fopen(path, "wb");
    if (out == NULL) {
        fprintf(stderr, "ge: android: cannot write %s\n", path);
        SDL_RWclose(in);
        return;
    }
    while ((n = SDL_RWread(in, buf, 1, sizeof(buf))) > 0) {
        fwrite(buf, 1, n, out);
    }
    fclose(out);
    SDL_RWclose(in);
}

/* Restrict the calling thread to the allowed cores with the highest top clock.
 * The allowed set already leaves out cores with no 32-bit support. */
static void portAndroidPreferFastCores(void)
{
    cpu_set_t allowed;
    cpu_set_t fast;
    long best = 0;
    int cpu;
    int total = 0;
    int kept = 0;

    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
        return;
    }
    CPU_ZERO(&fast);
    for (cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        char path[96];
        long khz = 0;
        FILE *f;

        if (!CPU_ISSET(cpu, &allowed)) {
            continue;
        }
        total++;
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
        f = fopen(path, "r");
        if (f == NULL) {
            return; /* cannot rank the cores: leave them to the scheduler */
        }
        if (fscanf(f, "%ld", &khz) != 1) {
            khz = 0;
        }
        fclose(f);
        if (khz > best) {
            best = khz;
            CPU_ZERO(&fast);
            kept = 0;
        }
        if (khz == best) {
            CPU_SET(cpu, &fast);
            kept++;
        }
    }
    if (kept == 0 || kept == total) {
        return; /* nothing to gain: every allowed core is the same */
    }
    if (sched_setaffinity(0, sizeof(fast), &fast) != 0) {
        fprintf(stderr, "ge: android: could not move to the fast cores\n");
        return;
    }
    fprintf(stderr, "ge: android: game threads on the %d fastest of %d cores (%ld MHz)\n",
            kept, total, best / 1000);
}

struct PortAndroidMainArgs {
    int argc;
    char **argv;
    int ret;
};

static void *portAndroidMainThread(void *p)
{
    struct PortAndroidMainArgs *a = (struct PortAndroidMainArgs *) p;
    portAndroidPreferFastCores();
    a->ret = portMain(a->argc, a->argv);
    return NULL;
}

int SDL_main(int argc, char *argv[])
{
    struct PortAndroidMainArgs args;
    pthread_attr_t attr;
    pthread_t thread;

    portAndroidInitRoot();
    portAndroidStartLog();
    portAndroidUnpack("gamecontrollerdb.txt");
    portAndroidUnpack("gamecontrollerdefault.txt");
    portAndroidUnpack("midas_ini_help.txt");
    portAndroidUnpack("Data/shaders_base.txt");
    fprintf(stderr, "ge: android: put the GoldenEye 007 (U) cart image in %s/Data/\n", s_Root);

    args.argc = argc;
    args.argv = argv;
    args.ret = 0;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, PORT_ANDROID_STACK);
    if (pthread_create(&thread, &attr, portAndroidMainThread, &args) != 0) {
        /* run it here and hope the stack holds */
        fprintf(stderr, "ge: android: no %u MB game thread, running on SDL's\n", PORT_ANDROID_STACK >> 20);
        return portMain(argc, argv);
    }
    pthread_attr_destroy(&attr);
    pthread_join(thread, NULL);
    return args.ret;
}
#endif /* __ANDROID__ */
