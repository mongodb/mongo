extern bool __wt_absolute_path(const char *path) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_has_priv(void) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern const char *__wt_path_separator(void) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cond_alloc(WT_SESSION_IMPL *session, const char *name, WT_CONDVAR **condp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_dlclose(WT_SESSION_IMPL *session, WT_DLH *dlh)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_dlopen(WT_SESSION_IMPL *session, const char *path, WT_DLH **dlhp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_dlsym(WT_SESSION_IMPL *session, WT_DLH *dlh, const char *name, bool fail,
  void *sym_ret) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_get_vm_pagesize(void) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_getenv(WT_SESSION_IMPL *session, const char *variable, const char **envp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
    WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_localtime(WT_SESSION_IMPL *session, const time_t *timep, struct tm *result)
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
    WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_once(void (*init_routine)(void)) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_os_posix(WT_SESSION_IMPL *session) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_posix_directory_list(WT_FILE_SYSTEM *file_system, WT_SESSION *wt_session,
  const char *directory, const char *prefix, char ***dirlistp, uint32_t *countp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_posix_directory_list_free(WT_FILE_SYSTEM *file_system, WT_SESSION *wt_session,
  char **dirlist, uint32_t count) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_posix_directory_list_single(WT_FILE_SYSTEM *file_system, WT_SESSION *wt_session,
  const char *directory, const char *prefix, char ***dirlistp, uint32_t *countp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_posix_file_extend(WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session,
  wt_off_t offset) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_posix_map(WT_FILE_HANDLE *fh, WT_SESSION *wt_session, void *mapped_regionp,
  size_t *lenp, void *mapped_cookiep) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_posix_map_discard(WT_FILE_HANDLE *fh, WT_SESSION *wt_session, void *map,
  size_t length, void *mapped_cookie) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_posix_map_preload(WT_FILE_HANDLE *fh, WT_SESSION *wt_session, const void *map,
  size_t length, void *mapped_cookie) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_posix_unmap(WT_FILE_HANDLE *fh, WT_SESSION *wt_session, void *mapped_region,
  size_t len, void *mapped_cookie) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_thread_create(WT_SESSION_IMPL *session, wt_thread_t *tidret,
  WT_THREAD_CALLBACK (*func)(void *), void *arg) WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_thread_join(WT_SESSION_IMPL *session, wt_thread_t *tid) WT_GCC_FUNC_DECL_ATTRIBUTE(
  (visibility("default"))) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_thread_str(char *buf, size_t buflen) WT_GCC_FUNC_DECL_ATTRIBUTE(
  (visibility("default"))) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_vsnprintf_len_incr(char *buf, size_t size, size_t *retsizep, const char *fmt,
  va_list ap) WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern uintmax_t __wt_process_id(void) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern void __wt_cond_destroy(WT_SESSION_IMPL *session, WT_CONDVAR **condp);
extern void __wt_cond_signal(WT_SESSION_IMPL *session, WT_CONDVAR *cond);
extern void __wt_cond_wait_signal(WT_SESSION_IMPL *session, WT_CONDVAR *cond, uint64_t usecs,
  bool (*run_func)(WT_SESSION_IMPL *), bool *signalled);
extern void __wt_epoch_raw(WT_SESSION_IMPL *session, struct timespec *tsp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")));
extern void __wt_map_file(WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session);
extern void __wt_prepare_remap_resize_file(WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session);
extern void __wt_release_without_remap(WT_FILE_HANDLE *file_handle);
extern void __wt_remap_resize_file(WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session);
extern void __wt_sleep(uint64_t seconds, uint64_t micro_seconds)
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")));
extern void __wt_stream_set_line_buffer(FILE *fp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")));
extern void __wt_stream_set_no_buffer(FILE *fp) WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")));
extern void __wt_thread_id(uintmax_t *id) WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")));
extern void __wt_unmap_file(WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session);
extern void __wt_yield(void) WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")));
extern void __wt_yield_no_barrier(void) WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")));

#ifdef HAVE_UNITTEST

#endif
