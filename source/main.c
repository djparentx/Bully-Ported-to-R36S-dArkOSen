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

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "hooks.h"
#include "imports.h"
#include "zip_fs.h"

static void check_data(void)
{
  const char *files[] = {
      // Basic structural check for loose files (assets folder)
      "assets/data_0.zip",
      // mod file goes here
      "",
  };
  struct stat st;
  unsigned int numfiles = (sizeof(files) / sizeof(*files)) - 1;
  // if mod is enabled, also check for mod file
  if (config.mod_file[0])
    files[numfiles++] = config.mod_file;
  // check if all the required files are present
  for (unsigned int i = 0; i < numfiles; ++i)
  {
    if (stat(files[i], &st) < 0)
    {
      fatal_error("Could not find\n%s.\nCheck your data files.", files[i]);
      break;
    }
  }
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

int main(void)
{
  int compat_delay_ms = 0;

  chdir_to_exe_dir();
  // try to read the config file and create one with default values if it's missing
  if (read_config(CONFIG_NAME) < 0)
    write_config(CONFIG_NAME);

  compat_delay_ms = config.timing_workaround_ms;
  if (compat_delay_ms < 0)
    compat_delay_ms = 0;
  debugPrintf_setCompatDelayMs(compat_delay_ms);

  zip_fs_init();

  check_data();

  // Load libc++_shared.so first — provides __ndk1 C++ stdlib symbols
  extern int cpplib_load(const char *filename);
  if (cpplib_load("libc++_shared.so") < 0)
    fatal_error("Could not load libc++_shared.so.\nCopy it from the APK\nlib/arm64-v8a/ folder\nto the game directory.");

  if (so_load(SO_NAME) < 0)

  // won't save without it
  mkdir("savegames", 0777);

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

  // Resolve all symbol addresses BEFORE so_finalize.
  // After svcMapProcessCodeMemory, the source heap pages become Perm_None
  // and so_find_addr (which reads syms/dynstrtab from text_base) will crash.
  // Use so_find_addr_rx to get the executable text_virtbase address.
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
