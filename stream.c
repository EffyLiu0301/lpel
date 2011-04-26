/**
 * File: stream.c
 * Auth: Daniel Prokesch <daniel.prokesch@gmail.com>
 * Date: 2010/08/26
 *
 * Desc:
 *
 * Core stream handling functions, including stream descriptors.
 *
 * A stream is the communication and synchronization primitive between two
 * tasks. If a task wants to use a stream, it must open it in order to
 * retrieve a stream descriptor. A task can open it for reading ('r',
 * a consumer) or writing ('w', a producer) but not both, meaning that
 * streams are uni-directional. A single stream can be opened by at most one
 * producer task and by at most one consumer task simultaneously.
 *
 * After opening, a consumer can read from a stream and a producer can write
 * to a stream, using the retrieved stream descriptor. Note that only the
 * streams are shared, not the stream descriptors.
 *
 * Within a stream a buffer struct is holding the actual data, which is
 * implemented as circular single-producer single-consumer in a concurrent,
 * lock-free manner.
 *
 * For synchronization between tasks, three blocking functions are provided:
 * LpelStreamRead() suspends the consumer trying to read from an empty stream,
 * LpelStreamWrite() suspends the producer trying to write to a full stream,
 * and a consumer can use LpelStreamPoll() to wait for the arrival of data
 * on any of the streams specified in a set.
 *
 *
 * @see http://www.cs.colorado.edu/department/publications/reports/docs/CU-CS-1023-07.pdf
 *      accessed Aug 26, 2010
 *      for more details on the FastForward queue.
 *
 *
 */

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

#include "arch/atomic.h"
#include "buffer.h"

#include "task.h"

#include "stream.h"
#include "streamset.h"

#include "monitoring.h"

/** Macros for lock handling */

#ifdef STREAM_POLL_SPINLOCK

#define PRODLOCK_TYPE       pthread_spinlock_t
#define PRODLOCK_INIT(x)    pthread_spin_init(x, PTHREAD_PROCESS_PRIVATE)
#define PRODLOCK_DESTROY(x) pthread_spin_destroy(x)
#define PRODLOCK_LOCK(x)    pthread_spin_lock(x)
#define PRODLOCK_UNLOCK(x)  pthread_spin_unlock(x)

#else

#define PRODLOCK_TYPE       pthread_mutex_t
#define PRODLOCK_INIT(x)    pthread_mutex_init(x, NULL)
#define PRODLOCK_DESTROY(x) pthread_mutex_destroy(x)
#define PRODLOCK_LOCK(x)    pthread_mutex_lock(x)
#define PRODLOCK_UNLOCK(x)  pthread_mutex_unlock(x)

#endif /* STREAM_POLL_SPINLOCK */





/**
 * A stream which is shared between a
 * (single) producer and a (single) consumer.
 */
struct lpel_stream_t {
  buffer_t buffer;          /** buffer holding the actual data */
  unsigned int uid;         /** unique sequence number */
  PRODLOCK_TYPE prod_lock;  /** to support polling a lock is needed */
  int is_poll;              /** indicates if a consumer polls this stream,
                                is_poll is protected by the prod_lock */
  lpel_stream_desc_t *prod_sd;   /** points to the sd of the producer */
  lpel_stream_desc_t *cons_sd;   /** points to the sd of the consumer */
  atomic_t n_sem;           /** counter for elements in the stream */
  atomic_t e_sem;           /** counter for empty space in the stream */
};

static atomic_t stream_seq = ATOMIC_INIT(0);



/**
 * Create a stream
 *
 * Allocate and initialize memory for a stream.
 *
 * @return pointer to the created stream
 */
lpel_stream_t *LpelStreamCreate(int size)
{
  assert( size >= 0);
  if (0==size) size = STREAM_BUFFER_SIZE;

  /* allocate memory for both the stream struct and the buffer area */
  lpel_stream_t *s = (lpel_stream_t *) malloc( sizeof(lpel_stream_t) );

  /* reset buffer (including buffer area) */
  _LpelBufferInit( &s->buffer, size);

  s->uid = fetch_and_inc( &stream_seq);
  PRODLOCK_INIT( &s->prod_lock );
  atomic_init( &s->n_sem, 0);
  atomic_init( &s->e_sem, size);
  s->is_poll = 0;
  s->prod_sd = NULL;
  s->cons_sd = NULL;
  return s;
}


/**
 * Destroy a stream
 *
 * Free the memory allocated for a stream.
 *
 * @param s   stream to be freed
 * @pre       stream must not be opened by any task!
 */
void LpelStreamDestroy( lpel_stream_t *s)
{
  PRODLOCK_DESTROY( &s->prod_lock);
  atomic_destroy( &s->n_sem);
  atomic_destroy( &s->e_sem);
  _LpelBufferCleanup( &s->buffer);
  free( s);
}


/**
  * Open a stream for reading/writing
 *
 * @param s     pointer to stream
 * @param mode  either 'r' for reading or 'w' for writing
 * @return      a stream descriptor
 * @pre         only one task may open it for reading resp. writing
 *              at any given point in time
 */
lpel_stream_desc_t *LpelStreamOpen( lpel_stream_t *s, char mode)
{
  lpel_stream_desc_t *sd;
  lpel_task_t *ct = LpelTaskSelf();

  assert( mode == 'r' || mode == 'w' );
  sd = (lpel_stream_desc_t *) malloc( sizeof( lpel_stream_desc_t));
  sd->task = ct;
  sd->stream = s;
  sd->mode = mode;
  sd->next  = NULL;

  /* create monitoring object, or NULL if stream
   * is not going to be monitored (depends on ct->mon)
   */
  sd->mon = LpelMonStreamOpen( ct->mon, s->uid, mode);

  switch(mode) {
    case 'r': s->cons_sd = sd; break;
    case 'w': s->prod_sd = sd; break;
  }

  return sd;
}

/**
 * Close a stream previously opened for reading/writing
 *
 * @param sd          stream descriptor
 * @param destroy_s   if != 0, destroy the stream as well
 */
void LpelStreamClose( lpel_stream_desc_t *sd, int destroy_s)
{
  /* MONITORING CALLBACK */
  if (sd->mon) LpelMonStreamClose(sd->mon);

  if (destroy_s) {
    LpelStreamDestroy( sd->stream);
  }
  free(sd);
}


/**
 * Replace a stream opened for reading by another stream
 * Destroys old stream.
 *
 * @param sd    stream descriptor for which the stream must be replaced
 * @param snew  the new stream
 * @pre         snew must not be opened by same or other task
 */
void LpelStreamReplace( lpel_stream_desc_t *sd, lpel_stream_t *snew)
{
  assert( sd->mode == 'r');
  /* destroy old stream */
  LpelStreamDestroy( sd->stream);
  /* assign new stream */
  sd->stream = snew;
  /* new consumer sd of stream */
  sd->stream->cons_sd = sd;

  /* MONITORING CALLBACK */
  if (sd->mon) LpelMonStreamReplace(sd->mon, snew->uid);
}


/**
 * Get the stream opened by a stream descriptor
 *
 * @param sd  the stream descriptor
 * @return    the stream opened by the stream descriptor
 */
lpel_stream_t *LpelStreamGet(lpel_stream_desc_t *sd)
{
  return sd->stream;
}



/**
 * Non-blocking, non-consuming read from a stream
 *
 * @param sd  stream descriptor
 * @return    the top item of the stream, or NULL if stream is empty
 */
void *LpelStreamPeek( lpel_stream_desc_t *sd)
{
  assert( sd->mode == 'r');
  return _LpelBufferTop( &sd->stream->buffer);
}


/**
 * Blocking, consuming read from a stream
 *
 * If the stream is empty, the task is suspended until
 * a producer writes an item to the stream.
 *
 * @param sd  stream descriptor
 * @return    the next item of the stream
 * @pre       current task is single reader
 */
void *LpelStreamRead( lpel_stream_desc_t *sd)
{
  void *item;
  lpel_task_t *self = sd->task;

  assert( sd->mode == 'r');

  /* quasi P(n_sem) */
  if ( fetch_and_dec( &sd->stream->n_sem) == 0) {
    /* MONITORING CALLBACK */
    if (sd->mon) LpelMonStreamBlockon(sd->mon);
    /* wait on stream: */
    LpelTaskBlock( self, BLOCKED_ON_INPUT);
  }


  /* read the top element */
  item = _LpelBufferTop( &sd->stream->buffer);
  assert( item != NULL);
  /* pop off the top element */
  _LpelBufferPop( &sd->stream->buffer);


  /* quasi V(e_sem) */
  if ( fetch_and_inc( &sd->stream->e_sem) < 0) {
    /* e_sem was -1 */
    lpel_task_t *prod = sd->stream->prod_sd->task;
    /* wakeup producer: make ready */
    LpelTaskUnblock( self, prod);
    /* MONITORING CALLBACK */
    if (sd->mon) LpelMonStreamWakeup(sd->mon);
  }

  /* MONITORING CALLBACK */
  if (sd->mon) LpelMonStreamMoved(sd->mon, item);

  return item;
}



/**
 * Blocking write to a stream
 *
 * If the stream is full, the task is suspended until the consumer
 * reads items from the stream, freeing space for more items.
 *
 * @param sd    stream descriptor
 * @param item  data item (a pointer) to write
 * @pre         current task is single writer
 * @pre         item != NULL
 */
void LpelStreamWrite( lpel_stream_desc_t *sd, void *item)
{
  lpel_task_t *self = sd->task;
  int poll_wakeup = 0;

  /* check if opened for writing */
  assert( sd->mode == 'w' );
  assert( item != NULL );

  /* quasi P(e_sem) */
  if ( fetch_and_dec( &sd->stream->e_sem)== 0) {
    /* MONITORING CALLBACK */
    if (sd->mon) LpelMonStreamBlockon(sd->mon);
    /* wait on stream: */
    LpelTaskBlock( self, BLOCKED_ON_OUTPUT);
  }

  /* writing to the buffer and checking if consumer polls must be atomic */
  PRODLOCK_LOCK( &sd->stream->prod_lock);
  {
    /* there must be space now in buffer */
    assert( _LpelBufferIsSpace( &sd->stream->buffer) );
    /* put item into buffer */
    _LpelBufferPut( &sd->stream->buffer, item);

    if ( sd->stream->is_poll) {
      /* get consumer's poll token */
      poll_wakeup = atomic_swap( &sd->stream->cons_sd->task->poll_token, 0);
      sd->stream->is_poll = 0;
    }
  }
  PRODLOCK_UNLOCK( &sd->stream->prod_lock);



  /* quasi V(n_sem) */
  if ( fetch_and_inc( &sd->stream->n_sem) < 0) {
    /* n_sem was -1 */
    lpel_task_t *cons = sd->stream->cons_sd->task;
    /* wakeup consumer: make ready */
    LpelTaskUnblock( self, cons);
    /* MONITORING CALLBACK */
    if (sd->mon) LpelMonStreamWakeup(sd->mon);
  } else {
    /* we are the sole producer task waking the polling consumer up */
    if (poll_wakeup) {
      lpel_task_t *cons = sd->stream->cons_sd->task;
      cons->wakeup_sd = sd->stream->cons_sd;

      LpelTaskUnblock( self, cons);
      /* MONITORING CALLBACK */
      if (sd->mon) LpelMonStreamWakeup(sd->mon);
    }
  }

  /* MONITORING CALLBACK */
  if (sd->mon) LpelMonStreamMoved(sd->mon, item);
}



/**
 * Non-blocking write to a stream
 *
 * @param sd    stream descriptor
 * @param item  data item (a pointer) to write
 * @pre         current task is single writer
 * @pre         item != NULL
 * @return 0 if the item could be written, -1 if the stream was full
 */
int LpelStreamTryWrite( lpel_stream_desc_t *sd, void *item)
{
  if (!_LpelBufferIsSpace(&sd->stream->buffer)) {
    return -1;
  }
  LpelStreamWrite( sd, item );
  return 0;
}

/**
 * Poll a set of streams
 *
 * This is a blocking function called by a consumer which wants to wait
 * for arrival of data on any of a specified set of streams.
 * The consumer task is suspended while there is no new data on all streams.
 *
 * @param set     a stream descriptor set the task wants to poll
 * @pre           set must not be empty (*set != NULL)
 *
 * @post          The first element when iterating through the set after
 *                LpelStreamPoll() will be the one after the one which
 *                caused the task to wakeup,
 *                i.e., the first stream where data arrived.
 */
lpel_stream_desc_t *LpelStreamPoll( lpel_streamset_t *set)
{
  lpel_task_t *self;
  lpel_stream_iter_t *iter;
  int do_ctx_switch = 1;
  int cnt = 0;

  assert( *set != NULL);

  /* get 'self', i.e. the task calling LpelStreamPoll() */
  self = (*set)->task;

  /* place a poll token */
  atomic_set( &self->poll_token, 1);

  /* for each stream in the set */
  iter = LpelStreamIterCreate( set);
  while( LpelStreamIterHasNext( iter)) {
    lpel_stream_desc_t *sd = LpelStreamIterNext( iter);
    lpel_stream_t *s = sd->stream;
    /* lock stream (prod-side) */
    PRODLOCK_LOCK( &s->prod_lock);
    { /* CS BEGIN */
      /* check if there is something in the buffer */
      if ( _LpelBufferTop( &s->buffer) != NULL) {
        /* yes, we can stop iterating through streams.
         * determine, if we have been woken up by another producer:
         */
        int tok = atomic_swap( &self->poll_token, 0);
        if (tok) {
          /* we have not been woken yet, no need for ctx switch */
          do_ctx_switch = 0;
          self->wakeup_sd = sd;
        }
        /* unlock stream */
        PRODLOCK_UNLOCK( &s->prod_lock);
        /* exit loop */
        break;

      } else {
        /* nothing in the buffer, register stream as activator */
        s->is_poll = 1;
        cnt++;
        //sd->event_flags |= STDESC_WAITON;
        /* TODO marking all streams does potentially flood the log-files
           - is it desired to have anyway?
        MarkDirty( sd);
        */
      }
    } /* CS END */
    /* unlock stream */
    PRODLOCK_UNLOCK( &s->prod_lock);
  } /* end for each stream */

  /* context switch */
  if (do_ctx_switch) {
    /* set task as blocked */
    LpelTaskBlock( self, BLOCKED_ON_ANYIN);
  }
  assert( atomic_read( &self->poll_token) == 0);

  /* unregister activators
   * - would only be necessary, if the consumer task closes the stream
   *   while the producer is in an is_poll state,
   *   as this could result in a SEGFAULT when the producer
   *   is trying to dereference sd->stream->cons_sd
   * - a consumer closes the stream if it reads
   *   a terminate record or a sync record, and between reading the record
   *   and closing the stream the consumer issues no LpelStreamPoll()
   *   and no entity writes a record on the stream after these records.
   * UPDATE: with static/dynamc collectors in S-Net, this is possible!
   */
  LpelStreamIterReset(iter, set);
  while( LpelStreamIterHasNext( iter)) {
    lpel_stream_t *s = (LpelStreamIterNext(iter))->stream;
    PRODLOCK_LOCK( &s->prod_lock);
    s->is_poll = 0;
    PRODLOCK_UNLOCK( &s->prod_lock);
    if (--cnt == 0) break;
  }

  LpelStreamIterDestroy(iter);

  /* 'rotate' set to stream descriptor for non-empty buffer */
  *set = self->wakeup_sd;

  return self->wakeup_sd;
}


