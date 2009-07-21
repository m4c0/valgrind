/**
 * @file  rwlock_test.c
 *
 * @brief Multithreaded test program that triggers various access patterns
 *        without triggering any race conditions.
 */


#define _GNU_SOURCE 1

#include <pthread.h>
#include <stdio.h>
#include <string.h>  // strerror()


#define NUM_ITERATIONS 1000

#define PTH_CALL(expr)                                  \
  do                                                    \
  {                                                     \
    int err = (expr);                                   \
    if ((err) != 0)                                     \
    {                                                   \
      fprintf(stderr,                                   \
              "%s:%d %s returned error code %d (%s)\n", \
              __FILE__,                                 \
              __LINE__,                                 \
              #expr,                                    \
              err,                                      \
              strerror(err));                           \
    }                                                   \
  } while (0)


static pthread_rwlock_t s_rwlock;
static int s_counter;


static void* thread_func(void* arg)
{
  int i;
  int sum = 0;

  for (i = 0; i < 1000; i++)
  {
    PTH_CALL(pthread_rwlock_rdlock(&s_rwlock));
    sum += s_counter;
    PTH_CALL(pthread_rwlock_unlock(&s_rwlock));
    PTH_CALL(pthread_rwlock_wrlock(&s_rwlock));
    s_counter++;
    PTH_CALL(pthread_rwlock_unlock(&s_rwlock));
  }

  return 0;
}

int main(int argc, char** argv)
{
  const int thread_count = 10;
  pthread_t tid[thread_count];
  int i;

  PTH_CALL(pthread_rwlock_init(&s_rwlock, NULL));
  for (i = 0; i < thread_count; i++)
  {
    PTH_CALL(pthread_create(&tid[i], 0, thread_func, 0));
  }

  for (i = 0; i < thread_count; i++)
  {
    PTH_CALL(pthread_join(tid[i], 0));
  }

  fprintf(stderr, "s_counter - thread_count * iterations = %d\n",
          s_counter - thread_count * NUM_ITERATIONS);
  fprintf(stderr, "Finished.\n");

  return 0;
}
