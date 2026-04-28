/* -*- c-basic-offset:2; tab-width:2; indent-tabs-mode:nil -*- */

/* --- static functions --- */

#if defined(USE_WIN32API)

static int convert_regis_to_bmp(char *path) {
  size_t len = strlen(path);
  char cmd[17 + len * 2];
  STARTUPINFO si;
  PROCESS_INFORMATION pi;

  path[len - 4] = '\0';
  sprintf(cmd, "registobmp.exe %s.rgs %s.bmp", path, path);

  ZeroMemory(&si, sizeof(STARTUPINFO));
  si.cb = sizeof(STARTUPINFO);
  si.dwFlags = STARTF_FORCEOFFFEEDBACK;

  if (CreateProcess(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
    DWORD code;

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (code == 0) {
      strcat(path, ".bmp");

      return 1;
    }
  }

  return 0;
}

#else

#include <unistd.h> /* execve */
#include <sys/wait.h>
#include <pobl/bl_path.h> /* bl_basename */

static int convert_regis_to_bmp(char *path) {
#ifdef USE_FB_EMBED
  /* fb-embed (downstream fork): ReGIS conversion is not supported
   * in embed mode yet. The conversion logic still lives inside
   * tool/registobmp/main(); lifting it into a callable function
   * is a separate patch. Until then, return 0 so the caller in
   * c_imagelib.c falls back to its "couldn't load image" path —
   * the embed app shows tofu instead of garbled content. */
  (void)path;
  return 0;
#else
  pid_t pid;
  int status;

  if ((pid = fork()) == -1) {
    return 0;
  }

  if (pid == 0) {
    char *new_path;
    size_t len;
#if defined(__CYGWIN__) || defined(__MSYS__)
    /* To make registobmp work even if it (or SDL) doesn't depend on cygwin. */
    char *file;

    file = bl_basename(path);
    if (file && path < file) {
      *(file - 1) = '\0';
      chdir(path);
      path = file;
    }
#endif

    len = strlen(path);

    /* Cast to char* is necessary because this function can be compiled by g++.
     */
    if ((new_path = (char*)malloc(len + 1))) {
      char *argv[4];
      /* fb-embed (downstream fork): honor MLTERM_LIBEXEC_DIR env var
       * for the registobmp lookup. Lets an embedding host (the scev
       * JNI loader, in our case) extract the helpers to a runtime
       * temp dir without having to predict the path at build time.
       * Falls back to the baked-in BL_LIBEXECDIR when unset, so
       * stand-alone mlterm-fb installs still work as before. */
      const char *envdir = getenv("MLTERM_LIBEXEC_DIR");
      char *exe_buf = NULL;
      if (envdir && *envdir) {
        size_t elen = strlen(envdir) + strlen("/mlterm/registobmp") + 1;
        exe_buf = (char *)malloc(elen);
        if (exe_buf) {
          snprintf(exe_buf, elen, "%s/mlterm/registobmp", envdir);
          argv[0] = exe_buf;
        } else {
          argv[0] = BL_LIBEXECDIR("mlterm") "/registobmp";
        }
      } else {
        argv[0] = BL_LIBEXECDIR("mlterm") "/registobmp";
      }

      strncpy(new_path, path, len - 4);
      strcpy(new_path + len - 4, ".bmp");

      argv[1] = path;
      argv[2] = new_path;
      argv[3] = NULL;

      execve(argv[0], argv, NULL);
      free(exe_buf);
    }

    exit(1);
  }

  waitpid(pid, &status, 0);

  /*
   * WEXITSTATUS uses __in and __out for union members, but
   * sal.h included from windows.h defines __in and __out macros on msys.
   * It causes compiling error in WEXITSTATUS.
   */
#ifdef __MSYS__
#undef __in
#undef __out
#endif
  if (WEXITSTATUS(status) == 0) {
    strcpy(path + strlen(path) - 4, ".bmp");

    return 1;
  }

  return 0;
#endif /* USE_FB_EMBED */
}

#endif
