/*
 * The PS5 SDL2 archive expects POSIX pthread names, while the native-app
 * runtime exposes Sony's equivalent ScePthread imports. Bridge the calls
 * SDL needs during startup without adding unsupported POSIX imports.
 */

#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <time.h>

#include <cerrno>
#include <cstdint>
#include <limits>

extern "C"
{
int scePthreadKeyCreate(pthread_key_t *key, void (*destructor)(void *));
int scePthreadKeyDelete(pthread_key_t key);
void *scePthreadGetspecific(pthread_key_t key);
int scePthreadSetspecific(pthread_key_t key, const void *value);

int scePthreadMutexattrInit(pthread_mutexattr_t *attr);
int scePthreadMutexattrDestroy(pthread_mutexattr_t *attr);
int scePthreadMutexattrSettype(pthread_mutexattr_t *attr, int type);
int scePthreadMutexInit(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr, const char *name);
int scePthreadMutexDestroy(pthread_mutex_t *mutex);
int scePthreadMutexLock(pthread_mutex_t *mutex);
int scePthreadMutexTrylock(pthread_mutex_t *mutex);
int scePthreadMutexUnlock(pthread_mutex_t *mutex);
pthread_t scePthreadSelf(void);
int scePthreadOnce(pthread_once_t *once_control, void (*init_routine)(void));
int scePthreadAttrInit(pthread_attr_t *attr);
int scePthreadAttrDestroy(pthread_attr_t *attr);
int scePthreadAttrSetstacksize(pthread_attr_t *attr, size_t stack_size);
int scePthreadCreate(pthread_t *thread, const pthread_attr_t *attr,
                     void *(*start_routine)(void *), void *arg, const char *name);
int scePthreadDetach(pthread_t thread);
int scePthreadJoin(pthread_t thread, void **value_ptr);
int scePthreadCondInit(pthread_cond_t *cond, const pthread_condattr_t *attr, const char *name);
int scePthreadCondDestroy(pthread_cond_t *cond);
int scePthreadCondSignal(pthread_cond_t *cond);
int scePthreadCondBroadcast(pthread_cond_t *cond);
int scePthreadCondWait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int scePthreadCondTimedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, uint32_t timeout_us);

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
    return scePthreadKeyCreate(key, destructor);
}

int pthread_key_delete(pthread_key_t key)
{
    return scePthreadKeyDelete(key);
}

void *pthread_getspecific(pthread_key_t key)
{
    return scePthreadGetspecific(key);
}

int pthread_setspecific(pthread_key_t key, const void *value)
{
    return scePthreadSetspecific(key, value);
}

int pthread_mutexattr_init(pthread_mutexattr_t *attr)
{
    return scePthreadMutexattrInit(attr);
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attr)
{
    return scePthreadMutexattrDestroy(attr);
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type)
{
    return scePthreadMutexattrSettype(attr, type);
}

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{
    return scePthreadMutexInit(mutex, attr, nullptr);
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    return scePthreadMutexDestroy(mutex);
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    return scePthreadMutexLock(mutex);
}

int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
    return scePthreadMutexTrylock(mutex);
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    return scePthreadMutexUnlock(mutex);
}

pthread_t pthread_self(void)
{
    return scePthreadSelf();
}

int pthread_once(pthread_once_t *once_control, void (*init_routine)(void))
{
    return scePthreadOnce(once_control, init_routine);
}

int pthread_attr_init(pthread_attr_t *attr)
{
    return scePthreadAttrInit(attr);
}

int pthread_attr_destroy(pthread_attr_t *attr)
{
    return scePthreadAttrDestroy(attr);
}

int pthread_attr_setdetachstate(pthread_attr_t *, int state)
{
    // SDL creates joinable threads. Sony's native default is joinable too.
    return state == PTHREAD_CREATE_JOINABLE ? 0 : EINVAL;
}

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stack_size)
{
    return scePthreadAttrSetstacksize(attr, stack_size);
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg)
{
    return scePthreadCreate(thread, attr, start_routine, arg, nullptr);
}

int pthread_detach(pthread_t thread)
{
    return scePthreadDetach(thread);
}

int pthread_join(pthread_t thread, void **value_ptr)
{
    return scePthreadJoin(thread, value_ptr);
}

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr)
{
    return scePthreadCondInit(cond, attr, nullptr);
}

int pthread_cond_destroy(pthread_cond_t *cond)
{
    return scePthreadCondDestroy(cond);
}

int pthread_cond_signal(pthread_cond_t *cond)
{
    return scePthreadCondSignal(cond);
}

int pthread_cond_broadcast(pthread_cond_t *cond)
{
    return scePthreadCondBroadcast(cond);
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    return scePthreadCondWait(cond, mutex);
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                           const struct timespec *deadline)
{
    if (deadline == nullptr)
        return EINVAL;

    struct timespec now{};
    if (clock_gettime(CLOCK_REALTIME, &now) != 0)
        return errno ? errno : EINVAL;

    const uint64_t deadline_us = static_cast<uint64_t>(deadline->tv_sec) * 1000000ULL +
                                 static_cast<uint64_t>(deadline->tv_nsec / 1000);
    const uint64_t now_us = static_cast<uint64_t>(now.tv_sec) * 1000000ULL +
                            static_cast<uint64_t>(now.tv_nsec / 1000);
    const uint64_t remaining_us = deadline_us > now_us ? deadline_us - now_us : 0;
    const uint32_t timeout_us = static_cast<uint32_t>(
        remaining_us > std::numeric_limits<uint32_t>::max()
            ? std::numeric_limits<uint32_t>::max()
            : remaining_us);
    return scePthreadCondTimedwait(cond, mutex, timeout_us);
}

int pthread_set_name_np(pthread_t, const char *)
{
    // Thread naming is diagnostic-only and is not exposed by this runtime.
    return 0;
}

int pthread_sigmask(int, const sigset_t *, sigset_t *)
{
    // The port does not install or depend on per-thread POSIX signal masks.
    return 0;
}

int pthread_setcanceltype(int, int *old_type)
{
    if (old_type != nullptr)
        *old_type = PTHREAD_CANCEL_ENABLE;
    return 0;
}

int pthread_getschedparam(pthread_t, int *policy, struct sched_param *param)
{
    if (policy != nullptr)
        *policy = SCHED_OTHER;
    if (param != nullptr)
        param->sched_priority = 0;
    return 0;
}

int pthread_setschedparam(pthread_t, int, const struct sched_param *)
{
    // SDL thread priority is best-effort on PS5; keep the inherited priority.
    return 0;
}
}
