/* main.c
 *
 * Copyright (C) 2026 givethesourceplox, fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
#include <stdio.h>
#include <signal.h>
#include <ucontext.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "hooks.h"
#include "imports.h"
#include "zip_fs.h"
#include "setup.h"
#include "cpplib_loader.h"

static void check_data(void)
{
  struct stat st;

  // the installer puts this in place; missing means a damaged install
  if (stat("assets/data_0.zip", &st) < 0)
    fatal_error("Could not find\n%s.\nCheck your data files.", "assets/data_0.zip");
}

// run from the binary's own folder so relative paths (assets/, savegames/, config.txt) resolve
static void chdir_to_exe_dir(void)
{
  char path[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (len <= 0)
    return;
  path[len] = '\0';

  char *slash = strrchr(path, '/');
  if (!slash || slash == path)
    return;
  *slash = '\0';

  if (chdir(path) < 0)
    fatal_error("Could not enter game folder\n%s", path);
}

// Fatal signals: log where it happened (libBully.so offsets when inside the game), then die
static int crash_in_code(uintptr_t addr)
{
  uintptr_t game = (uintptr_t)text_virtbase, cpp = 0;
  size_t cpp_size = 0;

  cpplib_get_range(&cpp, &cpp_size);
  return (game && addr >= game && addr < game + text_size) ||
         (cpp && addr >= cpp && addr < cpp + cpp_size);
}

static void crash_addr(const char *what, uintptr_t addr)
{
  uintptr_t game = (uintptr_t)text_virtbase, cpp = 0;
  size_t cpp_size = 0;
  char buf[160];
  int n;

  cpplib_get_range(&cpp, &cpp_size);
  if (game && addr >= game && addr < game + text_size)
    n = snprintf(buf, sizeof(buf), "[CRASH] %-5s libBully.so+0x%lx\n", what, (unsigned long)(addr - game));
  else if (cpp && addr >= cpp && addr < cpp + cpp_size)
    n = snprintf(buf, sizeof(buf), "[CRASH] %-5s libc++_shared.so+0x%lx\n", what, (unsigned long)(addr - cpp));
  else
    n = snprintf(buf, sizeof(buf), "[CRASH] %-5s 0x%lx\n", what, (unsigned long)addr);
  write(STDERR_FILENO, buf, n);
}

// the /proc/self/maps line that holds addr: names the library or mapping
static void crash_map(const char *what, uintptr_t addr)
{
  static char maps[1 << 18];
  size_t len = 0;
  ssize_t r;
  int fd = open("/proc/self/maps", O_RDONLY);
  char *line, *next, *eol, *end;
  char head[32];
  int n;

  if (fd < 0)
    return;
  while (len < sizeof(maps) - 1 && (r = read(fd, maps + len, sizeof(maps) - 1 - len)) > 0)
    len += (size_t)r;
  close(fd);
  maps[len] = '\0';

  n = snprintf(head, sizeof(head), "[CRASH] map   %-5s ", what);
  for (line = maps; *line; line = next)
  {
    uintptr_t lo, hi;

    eol = strchr(line, '\n');
    next = eol ? eol + 1 : line + strlen(line);
    if (eol)
      *eol = '\0';
    lo = strtoul(line, &end, 16);
    if (*end != '-')
      continue;
    hi = strtoul(end + 1, &end, 16);
    if (addr >= lo && addr < hi)
    {
      write(STDERR_FILENO, head, n);
      write(STDERR_FILENO, line, strlen(line));
      write(STDERR_FILENO, "\n", 1);
      return;
    }
  }
  write(STDERR_FILENO, head, n);
  write(STDERR_FILENO, "(not mapped)\n", 13);
}

static void crash_regs(const ucontext_t *uc)
{
  char buf[160];
  int n;

  for (int i = 0; i < 31; i += 3)
  {
    n = snprintf(buf, sizeof(buf), "[CRASH] x%-2d=%016lx", i, (unsigned long)uc->uc_mcontext.regs[i]);
    for (int j = i + 1; j < i + 3 && j < 31; j++)
      n += snprintf(buf + n, sizeof(buf) - n, "  x%-2d=%016lx", j, (unsigned long)uc->uc_mcontext.regs[j]);
    buf[n++] = '\n';
    write(STDERR_FILENO, buf, n);
  }
}

static void crash_header(int sig, const siginfo_t *si, const ucontext_t *uc, const char *kind)
{
  char name[16] = "?", buf[256];
  int n;

  prctl(PR_GET_NAME, (unsigned long)name, 0, 0, 0);
  if (si->si_code > 0)
    n = snprintf(buf, sizeof(buf),
                 "[CRASH] %s signal=%d code=%d fault_addr=0x%lx thread=%s tid=%ld sp=0x%lx\n",
                 kind, sig, si->si_code, (unsigned long)si->si_addr, name,
                 (long)syscall(SYS_gettid), (unsigned long)uc->uc_mcontext.sp);
  else
    n = snprintf(buf, sizeof(buf),
                 "[CRASH] %s signal=%d code=%d sent_by_pid=%d uid=%u thread=%s tid=%ld sp=0x%lx\n",
                 kind, sig, si->si_code, (int)si->si_pid, (unsigned)si->si_uid, name,
                 (long)syscall(SYS_gettid), (unsigned long)uc->uc_mcontext.sp);
  write(STDERR_FILENO, buf, n);
}

// Fatal signals: header, pc/lr with their library, registers, then (fatal only)
// the frame-pointer backtrace and return addresses found on the stack that point
// into the game's code (innermost first), then die
static void crash_handler(int sig, siginfo_t *si, void *ucv)
{
  static int s_ignored = 0;
  ucontext_t *uc = (ucontext_t *)ucv;
  uintptr_t sp = (uintptr_t)uc->uc_mcontext.sp;
  const uintptr_t *stack = (const uintptr_t *)sp;
  int found = 0;
  // a SIGSEGV this process sent to itself (raise/tgkill) is not a memory fault:
  // report it, keep the handler armed and let the thread carry on (3 times at most)
  int self_sent = sig == SIGSEGV && si->si_code <= 0 && si->si_pid == getpid() && s_ignored < 3;

  fflush(stdout);
  crash_header(sig, si, uc, self_sent ? "self-sent (ignored)" : "fatal");
  crash_addr("pc", (uintptr_t)uc->uc_mcontext.pc);
  crash_addr("lr", (uintptr_t)uc->uc_mcontext.regs[30]);
  crash_map("pc", (uintptr_t)uc->uc_mcontext.pc);
  crash_map("lr", (uintptr_t)uc->uc_mcontext.regs[30]);
  if (si->si_code > 0)
    crash_map("fault", (uintptr_t)si->si_addr);
  crash_regs(uc);

  if (self_sent)
  {
    struct sigaction again;

    s_ignored++;
    memset(&again, 0, sizeof(again));
    again.sa_sigaction = crash_handler;
    again.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&again.sa_mask);
    sigaction(SIGSEGV, &again, NULL);
    return;
  }

  // 32 KB of stack; if this runs off the mapped stack the second fault is
  // fatal (SA_RESETHAND), so it can't loop
  for (int i = 0; i < 4096 && found < 16; i++)
  {
    if (crash_in_code(stack[i]))
    {
      crash_addr("stack", stack[i]);
      found++;
    }
  }

  // frame-pointer chain: x29 -> {previous x29, return address}
  {
    uintptr_t fp = (uintptr_t)uc->uc_mcontext.regs[29];

    for (int i = 0; i < 24 && fp >= sp && fp - sp < (1u << 20) && !(fp & 7); i++)
    {
      const uintptr_t *frame = (const uintptr_t *)fp;

      crash_addr("bt", frame[1]);
      if (frame[0] <= fp)
        break;
      fp = frame[0];
    }
  }

  raise(sig); // SA_RESETHAND: the default action now runs
}

static void install_crash_handler(void)
{
  static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
  struct sigaction sa;

  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_handler;
  sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
  sigemptyset(&sa.sa_mask);
  for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
    sigaction(sigs[i], &sa, NULL);
}

int main(void)
{
  chdir_to_exe_dir();

  // line-buffered so the log is complete up to a crash
  setvbuf(stdout, NULL, _IOLBF, 0);
  install_crash_handler();

  // the user's values (defaults for anything missing, clamped), then
  // config.txt is rewritten from the commented template with those values
  read_config(CONFIG_NAME);
  write_config(CONFIG_NAME);

  // one window for the installer, the splash and the game
  if (!video_open())
    fatal_error("Could not open the display");

  if (!setup_is_installed() && !setup_run())
    port_exit(0);

  splash_show();

  zip_fs_init();

  check_data();

  // Load libc++_shared.so first — provides __ndk1 C++ stdlib symbols
  extern int cpplib_load(const char *filename);
  if (cpplib_load("libc++_shared.so") < 0)
    fatal_error("Could not load libc++_shared.so.\nCopy it from the APK\nlib/arm64-v8a/ folder\nto the game directory.");
  if (so_load(SO_NAME) < 0)
    fatal_error("Could not load\n%s.", SO_NAME);

  // saves live in savegames/ (created here); older saves in the game folder move into it
  save_migrate();

  update_imports();

  so_relocate();
  so_resolve(dynlib_functions, dynlib_numfunctions, 1);

  patch_openal();
  // patch_opengl();
  patch_game();
  // patch_movie();

  // can't set it in the initializer because it's not constant
  stderr_fake = stderr;
  extern FILE *stdin_fake;
  stdin_fake = stdin;

  // Resolve symbol addresses before so_finalize(). so_find_addr_rx gives the
  // executable text_virtbase address (on Linux the same mapping as text_base).
  extern void jni_init(void);
  jni_init(); // sets up fake_vm and resolves JNI_OnLoad address

  // Set StorageRootPath directly — the game uses this for data file paths.
  // On Android, NvUtilInit sets this via JNI getAppLocalValue("STORAGE_ROOT").
  // We set it here to bypass the JNI roundtrip.
  strcpy((char *)so_find_addr("StorageRootPath"), ".");

  so_finalize();
  so_flush_caches();

  // GOT-based hooks: patch PLT/GOT entries now that data segment is RW.
  // Must be after so_finalize() (GOT accessible) and before jni_start() (game runs).
  patch_game_post_finalize();

  // Run C++ global constructors (.init_array).
  // This populates AllWarTypes, registers WarLangType objects, and performs
  // other critical static initialization that the game expects before main logic.
  so_execute_init_array();

  // Now call into the loaded .so - code is executable at text_virtbase
  extern void jni_start(void);
  jni_start();

  return 0;
}
