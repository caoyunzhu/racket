#include "schpriv.h"

#ifdef MZ_USE_PLACES

/************************************************************************/
/************************************************************************/
/************************************************************************/
#define MZRT_INTERNAL
#include "mzrt.h"
#include "schgc.h"

#ifdef MZ_XFORM
START_XFORM_SUSPEND;
#endif

/* std C headers */
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <../sconfig.h>

/* platform headers */
#ifdef WIN32
# include <windows.h>
#else
# include <pthread.h>
# include <signal.h>
# include <unistd.h>
# include <time.h>
# if defined(UNIX_LIMIT_STACK) || defined(UNIX_LIMIT_FDSET_SIZE)
#   include <signal.h>
#   include <sys/time.h>
#   include <sys/resource.h>
# endif
#endif

#ifndef MZ_PRECISE_GC
int GC_pthread_join(pthread_t thread, void **retval);
int GC_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void*), void * arg);
int GC_pthread_detach(pthread_t thread);
#endif

void mzrt_set_user_break_handler(void (*user_break_handler)(int))
{
#ifdef WIN32
#else
  signal(SIGINT, user_break_handler);
#endif
}

static void rungdb() {
#ifdef WIN32
#else
  pid_t pid = getpid();
  char outbuffer[100];
  char inbuffer[10];

  fprintf(stderr, "pid # %i resume(r)/gdb(d)/exit(e)?\n", pid);
  fflush(stderr);

  while(1) {
    while(read(fileno(stdin), inbuffer, 10) <= 0){
      if(errno != EINTR){
        fprintf(stderr, "Error detected %i\n", errno);
      }
    }
    switch(inbuffer[0]) {
      case 'r':
        return;
        break;
      case 'd':
        snprintf(outbuffer, 100, "xterm -e gdb ./mzscheme3m %d &", pid);
        fprintf(stderr, "%s\n", outbuffer);
        system(outbuffer);
        break;
      case 'e':
      default:
        exit(1);
        break;
    }
  }
#endif
}

static void segfault_handler(int signal_num) {
  pid_t pid = getpid();
  fprintf(stderr, "sig# %i pid# %i\n", signal_num, pid);
  rungdb();
}


void mzrt_set_segfault_debug_handler()
{
#ifdef WIN32
#else
  signal(SIGSEGV, segfault_handler);
#endif
}

void mzrt_sleep(int seconds)
{
#ifdef WIN32
#else
  struct timespec set;
  struct timespec rem;
  set.tv_sec  = seconds;
  set.tv_nsec = 0;
  rem.tv_sec  = 0;
  rem.tv_nsec = 0;
  while ((-1 == nanosleep(&set, &rem))) {
    //fprintf(stderr, "%i %i INITIAL\n", set.tv_sec, set.tv_nsec);
    //fprintf(stderr, "%i %i LEFT\n", rem.tv_sec, rem.tv_nsec);
    set = rem;
    //fprintf(stderr, "%i %i NOW\n", set.tv_sec, set.tv_nsec);
  }
#endif
}

#ifdef MZ_XFORM
END_XFORM_SUSPEND;
#endif

/***********************************************************************/
/*                Atomic Ops                                           */
/***********************************************************************/

MZ_INLINE uint32_t mzrt_atomic_add_32(volatile unsigned int *counter, unsigned int value) {
#ifdef WIN32
# if defined(__MINGW32__)
  return InterlockedExchangeAdd((long *)counter, value);
# else
  return InterlockedExchangeAdd(counter, value);
# endif

#elif defined (__GNUC__) && (defined(__i386__) || defined(__x86_64__))
  asm volatile ("lock; xaddl %0,%1"
      : "=r" (value), "=m" (*counter)
      : "0" (value), "m" (*counter)
      : "memory", "cc");
  return value;
#else
#error !!!Atomic ops not provided!!!
#endif
}

/* returns the pre-incremented value */
MZ_INLINE uint32_t mzrt_atomic_incr_32(volatile unsigned int *counter) {
  return mzrt_atomic_add_32(counter, 1);
}

/***********************************************************************/
/*                Threads                                              */
/***********************************************************************/
typedef struct mzrt_thread_stub_data {
  void * (*start_proc)(void *);
  void *data;
  mz_proc_thread *thread;
} mzrt_thread_stub_data;

void *mzrt_thread_stub(void *data){
  mzrt_thread_stub_data *stub_data  = (mzrt_thread_stub_data*) data;
  void * (*start_proc)(void *)        = stub_data->start_proc;
  void *start_proc_data               = stub_data->data;
  scheme_init_os_thread();
  proc_thread_self                    = stub_data->thread;

  free(data);

  return start_proc(start_proc_data);
}

unsigned int mz_proc_thread_self() {
#ifdef WIN32
#error !!!mz_proc_thread_id not implemented!!!
#else
  return (unsigned int) pthread_self();
#endif
}

unsigned int mz_proc_thread_id(mz_proc_thread* thread) {

  return (unsigned int) thread->threadid;
}

mz_proc_thread* mzrt_proc_first_thread_init() {
  /* initialize mz_proc_thread struct for first thread myself that wasn't created with mz_proc_thread_create,
   * so it can communicate with other mz_proc_thread_created threads via pt_mboxes */
  mz_proc_thread *thread = (mz_proc_thread*)malloc(sizeof(mz_proc_thread));
  thread->mbox      = pt_mbox_create();
  thread->threadid  = mz_proc_thread_self();
  proc_thread_self  = thread;
  return thread;
}

mz_proc_thread* mz_proc_thread_create(mz_proc_thread_start start_proc, void* data) {
  mz_proc_thread *thread = (mz_proc_thread*)malloc(sizeof(mz_proc_thread));
#ifdef MZ_PRECISE_GC
  mzrt_thread_stub_data *stub_data = (mzrt_thread_stub_data*)malloc(sizeof(mzrt_thread_stub_data));
  thread->mbox = pt_mbox_create();
  stub_data->start_proc = start_proc;
  stub_data->data       = data;
  stub_data->thread     = thread;
#   ifdef WIN32
  thread->threadid = CreateThread(NULL, 0, start_proc, data, 0, NULL);
#   else
  pthread_create(&thread->threadid, NULL, mzrt_thread_stub, stub_data);
#   endif
#else
#   ifdef WIN32
  thread->threadid = GC_CreateThread(NULL, 0, start_proc, data, 0, NULL);
#   else
  GC_pthread_create(&thread->threadid, NULL, start_proc, data);
#   endif
#endif
  return thread;
}

void * mz_proc_thread_wait(mz_proc_thread *thread) {
#ifdef WIN32
  DWORD rc;
  WaitForSingleObject(thread->threadid,INFINITE);
  GetExitCodeThread(thread->threadid, &rc);
  return (void *) rc;
#else
  void *rc;
#   ifndef MZ_PRECISE_GC
  GC_pthread_join(thread->threadid, &rc);
#   else
  pthread_join(thread->threadid, &rc);
#   endif
  return rc;
#endif
}

int mz_proc_thread_detach(mz_proc_thread *thread) {
#ifdef WIN32
  DWORD rc;
  return (void *) rc;
#else
  int rc;
#   ifndef MZ_PRECISE_GC
  rc = GC_pthread_detach(thread->threadid);
#   else
  rc = pthread_detach(thread->threadid);
#   endif
  return rc;
#endif
}

/***********************************************************************/
/*                RW Lock                                              */
/***********************************************************************/

/* Unix **************************************************************/

#ifndef WIN32

#ifdef MZ_XFORM
START_XFORM_SUSPEND;
#endif

struct mzrt_rwlock {
  pthread_rwlock_t lock;
};

int mzrt_rwlock_create(mzrt_rwlock **lock) {
  *lock = malloc(sizeof(mzrt_rwlock));
  return pthread_rwlock_init(&(*lock)->lock, NULL);
}

int mzrt_rwlock_rdlock(mzrt_rwlock *lock) {
  return pthread_rwlock_rdlock(&lock->lock);
}

int mzrt_rwlock_wrlock(mzrt_rwlock *lock) {
  return pthread_rwlock_wrlock(&lock->lock);
}

int mzrt_rwlock_tryrdlock(mzrt_rwlock *lock) {
  return pthread_rwlock_tryrdlock(&lock->lock);
}

int mzrt_rwlock_trywrlock(mzrt_rwlock *lock) {
  return pthread_rwlock_trywrlock(&lock->lock);
}
int mzrt_rwlock_unlock(mzrt_rwlock *lock) {
  return pthread_rwlock_unlock(&lock->lock);
}

int mzrt_rwlock_destroy(mzrt_rwlock *lock) {
  return pthread_rwlock_destroy(&lock->lock);
}

struct mzrt_mutex {
  pthread_mutex_t mutex;
};

int mzrt_mutex_create(mzrt_mutex **mutex) {
  *mutex = malloc(sizeof(struct mzrt_mutex));
  return pthread_mutex_init(&(*mutex)->mutex, NULL);
}

int mzrt_mutex_lock(mzrt_mutex *mutex) {
  return pthread_mutex_lock(&mutex->mutex);
}

int mzrt_mutex_trylock(mzrt_mutex *mutex) {
  return pthread_mutex_trylock(&mutex->mutex);
}

int mzrt_mutex_unlock(mzrt_mutex *mutex) {
  return pthread_mutex_unlock(&mutex->mutex);
}

int mzrt_mutex_destroy(mzrt_mutex *mutex) {
  return pthread_mutex_destroy(&mutex->mutex);
}

struct mzrt_cond {
  pthread_cond_t cond;
};

int mzrt_cond_create(mzrt_cond **cond) {
  *cond = malloc(sizeof(struct mzrt_cond));
  return pthread_cond_init(&(*cond)->cond, NULL);
}

int mzrt_cond_wait(mzrt_cond *cond, mzrt_mutex *mutex) {
  return pthread_cond_wait(&cond->cond, &mutex->mutex);
}

int mzrt_cond_timedwait(mzrt_cond *cond, mzrt_mutex *mutex, long seconds, long nanoseconds) {
  struct timespec timeout;
  timeout.tv_sec  = seconds;
  timeout.tv_nsec = nanoseconds;
  return pthread_cond_timedwait(&cond->cond, &mutex->mutex, &timeout);
}

int mzrt_cond_signal(mzrt_cond *cond) {
  return pthread_cond_signal(&cond->cond);
}

int mzrt_cond_broadcast(mzrt_cond *cond) {
  return pthread_cond_broadcast(&cond->cond);
}

int mzrt_cond_destroy(mzrt_cond *cond) {
  return pthread_cond_destroy(&cond->cond);
}

/****************** PROCESS THREAD MAIL BOX *******************************/

pt_mbox *pt_mbox_create() {
  pt_mbox *mbox = (pt_mbox *)malloc(sizeof(pt_mbox));
  mbox->count = 0;
  mbox->in    = 0;
  mbox->out   = 0;
  mzrt_mutex_create(&mbox->mutex);
  mzrt_cond_create(&mbox->nonempty);
  mzrt_cond_create(&mbox->nonfull);
  return mbox;
}

void pt_mbox_send(pt_mbox *mbox, int type, void *payload, pt_mbox *origin) {
  mzrt_mutex_lock(mbox->mutex);
  while ( mbox->count == 5 ) {
    mzrt_cond_wait(mbox->nonfull, mbox->mutex);
  }
  mbox->queue[mbox->in].type = type;
  mbox->queue[mbox->in].payload = payload;
  mbox->queue[mbox->in].origin = origin;
  mbox->in = (mbox->in + 1) % 5;
  mbox->count++;
  mzrt_cond_signal(mbox->nonempty);
  mzrt_mutex_unlock(mbox->mutex);
}

void pt_mbox_recv(pt_mbox *mbox, int *type, void **payload, pt_mbox **origin){
  mzrt_mutex_lock(mbox->mutex);
  while ( mbox->count == 0 ) {
    mzrt_cond_wait(mbox->nonempty, mbox->mutex);
  }
  *type    = mbox->queue[mbox->out].type;
  *payload = mbox->queue[mbox->out].payload;
  *origin  = mbox->queue[mbox->out].origin;
  mbox->out = (mbox->out + 1) % 5;
  mbox->count--;
  mzrt_cond_signal(mbox->nonfull);
  mzrt_mutex_unlock(mbox->mutex);
}

void pt_mbox_send_recv(pt_mbox *mbox, int type, void *payload, pt_mbox *origin, int *return_type, void **return_payload) {
  pt_mbox *return_origin;
  pt_mbox_send(mbox, type, payload, origin);
  pt_mbox_recv(origin, return_type, return_payload, &return_origin);
}

void pt_mbox_destroy(pt_mbox *mbox) {
  mzrt_mutex_destroy(mbox->mutex);
  mzrt_cond_destroy(mbox->nonempty);
  mzrt_cond_destroy(mbox->nonfull);
  free(mbox);
}

#ifdef MZ_XFORM
END_XFORM_SUSPEND;
#endif

#endif

/* Windows **************************************************************/

#ifdef WIN32

#ifdef MZ_XFORM
START_XFORM_SUSPEND;
#endif

typedef struct mzrt_rwlock {
  HANDLE readEvent;
  HANDLE writeMutex;
  unsigned long readers;
} mzrt_rwlock;

int mzrt_rwlock_create(mzrt_rwlock **lock) {
  *lock = malloc(sizeof(mzrt_rwlock));
  (*lock)->readers = 0;
  /* CreateEvent(LPSECURITY_ATTRIBUTES, manualReset, initiallySignaled, LPCSTR name) */
  if (! ((*lock)->readEvent = CreateEvent(NULL, TRUE, FALSE, NULL)))
    return 0;
  if (! ((*lock)->writeMutex = CreateMutex(NULL, FALSE, NULL)))
    return 0;

  return 1;
}

static int get_win32_os_error() {
  return 0;
}

static int mzrt_rwlock_rdlock_worker(mzrt_rwlock *lock, DWORD millis) {
  DWORD rc = WaitForSingleObject(lock->writeMutex, millis);
  if (rc == WAIT_FAILED || rc == WAIT_TIMEOUT );
    return 0;

  InterlockedIncrement(&lock->readers);

  if (! ResetEvent(lock->readEvent))
    return 0;

  if (!ReleaseMutex(lock->writeMutex))
    return 0;

  return 1;
}

static int mzrt_rwlock_wrlock_worker(mzrt_rwlock *lock, DWORD millis) {
  DWORD rc = WaitForSingleObject(lock->writeMutex, millis);
  if (rc == WAIT_FAILED || rc == WAIT_TIMEOUT );
    return 0;

  if (lock->readers) {
    if (millis) {
      rc = WaitForSingleObject(lock->readEvent, millis);
    }
    else {
      rc = WAIT_TIMEOUT;
    }

    if (rc == WAIT_FAILED || rc == WAIT_TIMEOUT );
      return 0;
  }

  return 1;
}

int mzrt_rwlock_rdlock(mzrt_rwlock *lock) {
  return mzrt_rwlock_rdlock_worker(lock, INFINITE);
}

int mzrt_rwlock_wrlock(mzrt_rwlock *lock) {
  return mzrt_rwlock_wrlock_worker(lock, INFINITE);
}

int mzrt_rwlock_tryrdlock(mzrt_rwlock *lock) {
  return mzrt_rwlock_rdlock_worker(lock, 0);
}

int mzrt_rwlock_trywrlock(mzrt_rwlock *lock) {
  return mzrt_rwlock_wrlock_worker(lock, 0);
}

int mzrt_rwlock_unlock(mzrt_rwlock *lock) {
  DWORD rc = 0;
  if (!ReleaseMutex(lock->writeMutex)) {
    rc = get_win32_os_error();
  }

  if (rc == ERROR_NOT_OWNER) {
    if (lock->readers && !InterlockedDecrement(&lock->readers) && !SetEvent(lock->readEvent)) {
      rc = get_win32_os_error();
    }
    else {
      rc = 0;
    }
  }

  return !rc;
}

int mzrt_rwlock_destroy(mzrt_rwlock *lock) {
  int rc = 1;
  rc &= CloseHandle(lock->readEvent);
  rc &= CloseHandle(lock->writeMutex);
  return rc; 
}

struct mzrt_mutex {
  CRITICAL_SECTION critical_section;
};

int mzrt_mutex_create(mzrt_mutex **mutex) {
  *mutex = malloc(sizeof(mzrt_mutex));
  InitializeCriticalSection(&(*mutex)->critical_section);
  return 0;
}

int mzrt_mutex_lock(mzrt_mutex *mutex) {
  EnterCriticalSection(&(*mutex)->critical_section);
  return 0;
}

int mzrt_mutex_trylock(mzrt_mutex *mutex) {
  if (!TryEnterCriticalSection(&(*mutex)->critical_section))
    return 1;
  return 0;
}

int mzrt_mutex_unlock(mzrt_mutex *mutex) {
  LeaveCriticalSection(&(*mutex)->critical_section);
  return 0;
}

int mzrt_mutex_destroy(mzrt_mutex *mutex) {
  DeleteCriticalSection(&(*mutex)->critical_section);
  return 0;
}

struct mzrt_cond {
  pthread_cond_t cond;
};

int mzrt_cond_create(mzrt_cond **cond) {
  *cond = malloc(sizeof(mzrt_cond));
  return pthread_cond_init(&(*cond)->cond, NULL);
}

int mzrt_cond_wait(mzrt_cond *cond, mzrt_mutex *mutex) {
  return pthread_cond_wait(&cond->cond, &mutex->mutex);
}

int mzrt_cond_timedwait(mzrt_cond *cond, mzrt_mutex *mutex) {
  return pthread_cond_timedwait(&cond->cond, &mutex->mutex);
}

int mzrt_cond_signal(mzrt_cond *cond) {
  return pthread_cond_signal(&cond->cond);
}

int mzrt_cond_broadcast(mzrt_cond *cond) {
  return pthread_cond_broadcast(&cond->cond);
}

int mzrt_cond_destroy(mzrt_cond *cond) {
  return pthread_cond_destroy(&cond->cond);
}

#ifdef MZ_XFORM
END_XFORM_SUSPEND;
#endif

#endif

/************************************************************************/
/************************************************************************/
/************************************************************************/

#endif
