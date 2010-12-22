
#ifndef _LPEL_H_
#define _LPEL_H_

#define LPEL_USE_CAPABILITIES


#include <pthread.h>

#include "bool.h"

#include "monitoring.h"

/**
 * Specification for configuration:
 *
 * proc_workers is the number of processors used for workers.
 * num_workers must be a multiple of proc_workers.
 * proc_others is the number of processors assigned to other than
 *   worker threads.
 * flags:
 *   AUTO - use default setting for num_workers, proc_workers, proc_others
 *   REALTIME - set realtime priority for workers, will succeed only if
 *              there is a 1:1 mapping of workers to procs,
 *              proc_others > 0 and the process has needed privileges.
 */
typedef struct {
  int num_workers;
  int proc_workers;
  int proc_others;
  int flags;
  int node;
} lpelconfig_t;

#define LPEL_FLAG_AUTO     (1<<0)
#define LPEL_FLAG_AUTO2    (1<<1)
#define LPEL_FLAG_REALTIME (1<<4)



typedef struct lpelthread lpelthread_t;

struct lpelthread {
  pthread_t pthread;
  bool detached;
  void (*func)(void *);
  void *arg;
};


extern void LpelInit(lpelconfig_t *cfg);
extern void LpelCleanup(void);


extern int LpelNumWorkers(void);


extern lpelthread_t *LpelThreadCreate( void (*func)(void *),
    void *arg, bool detached);

extern void LpelThreadJoin( lpelthread_t *env);
extern void LpelThreadAssign( int core);


#endif /* _LPEL_H_ */
