#include "thread_pool.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

struct thread_task {
    thread_task_f function;
    void *arg;
    void *result;
    bool is_pushed;
    bool is_running;
    bool is_finished;
    bool is_detached;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
};

struct thread_pool {
    pthread_t *threads;
    int max_threads;
    int active_threads;
    struct thread_task **tasks;
    int task_count;
    int running_tasks;
    int head;
    int tail;
    bool shutdown;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
};

static void *worker_thread(void *arg) {
    struct thread_pool *pool = (struct thread_pool *)arg;
    
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
    
    while (1) {
        struct thread_task *task = NULL;
        
        pthread_mutex_lock(&pool->mutex);
        
        while (pool->task_count == 0 && !pool->shutdown) {
            pthread_cond_wait(&pool->cond, &pool->mutex);
        }
        
        if (pool->shutdown) {
            pool->active_threads--;
            pthread_mutex_unlock(&pool->mutex);
            pthread_exit(NULL);
        }
        
        if (pool->task_count > 0) {
            task = pool->tasks[pool->head];
            pool->head = (pool->head + 1) % TPOOL_MAX_TASKS;
            pool->task_count--;
            
            pool->running_tasks++;
        }
        
        pthread_mutex_unlock(&pool->mutex);
        
        if (task != NULL) {
            pthread_mutex_lock(&task->mutex);
            task->is_running = true;
            pthread_mutex_unlock(&task->mutex);
            
            void *result = task->function(task->arg);
            
            pthread_mutex_lock(&task->mutex);
            task->result = result;
            task->is_running = false;
            task->is_finished = true;
            
            pthread_cond_broadcast(&task->cond);
            
            pthread_mutex_lock(&pool->mutex);
            pool->running_tasks--;
            pthread_mutex_unlock(&pool->mutex);
            
            if (task->is_detached) {
                pthread_mutex_unlock(&task->mutex);
                thread_task_delete(task);
            } else {
                pthread_mutex_unlock(&task->mutex);
            }
        }
    }
    
    return NULL;
}

static int create_worker(struct thread_pool *pool) {
    if (pool->active_threads >= pool->max_threads) {
        return -1;
    }
    
    pthread_t thread;
    if (pthread_create(&thread, NULL, worker_thread, pool) != 0) {
        return -1;
    }
    
    pool->threads[pool->active_threads++] = thread;
    return 0;
}

int thread_pool_new(int max_thread_count, struct thread_pool **pool) {
    if (max_thread_count <= 0 || max_thread_count > TPOOL_MAX_THREADS) {
        return TPOOL_ERR_INVALID_ARGUMENT;
    }
    
    *pool = calloc(1, sizeof(struct thread_pool));
    if (*pool == NULL) {
        return TPOOL_ERR_NOT_IMPLEMENTED;
    }
    
    (*pool)->threads = malloc(sizeof(pthread_t) * max_thread_count);
    if ((*pool)->threads == NULL) {
        free(*pool);
        return TPOOL_ERR_NOT_IMPLEMENTED;
    }
    
    (*pool)->tasks = calloc(TPOOL_MAX_TASKS, sizeof(struct thread_task *));
    if ((*pool)->tasks == NULL) {
        free((*pool)->threads);
        free(*pool);
        return TPOOL_ERR_NOT_IMPLEMENTED;
    }
    
    (*pool)->max_threads = max_thread_count;
    (*pool)->active_threads = 0;
    (*pool)->task_count = 0;
    (*pool)->running_tasks = 0;
    (*pool)->head = 0;
    (*pool)->tail = 0;
    (*pool)->shutdown = false;
    
    pthread_mutex_init(&(*pool)->mutex, NULL);
    pthread_cond_init(&(*pool)->cond, NULL);
    
    return 0;
}

int thread_pool_thread_count(const struct thread_pool *pool) {
    pthread_mutex_lock((pthread_mutex_t *)&pool->mutex);
    int count = pool->active_threads;
    pthread_mutex_unlock((pthread_mutex_t *)&pool->mutex);
    return count;
}

int thread_pool_delete(struct thread_pool *pool) {
    pthread_mutex_lock(&pool->mutex);
    
    if (pool->task_count > 0 || pool->running_tasks > 0) {
        pthread_mutex_unlock(&pool->mutex);
        return TPOOL_ERR_HAS_TASKS;
    }
    
    pool->shutdown = true;
    pthread_cond_broadcast(&pool->cond);
    
    int active_threads = pool->active_threads;
    pthread_mutex_unlock(&pool->mutex);
    
    for (int i = 0; i < active_threads; i++) {
        pthread_join(pool->threads[i], NULL);
    }
    
    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->cond);
    free(pool->threads);
    free(pool->tasks);
    free(pool);
    
    return 0;
}

int thread_pool_push_task(struct thread_pool *pool, struct thread_task *task) {
    pthread_mutex_lock(&pool->mutex);
    
    if (pool->task_count >= TPOOL_MAX_TASKS) {
        pthread_mutex_unlock(&pool->mutex);
        return TPOOL_ERR_TOO_MANY_TASKS;
    }
    
    pthread_mutex_lock(&task->mutex);
    task->is_pushed = true;
    task->is_running = false;
    task->is_finished = false;
    pthread_mutex_unlock(&task->mutex);
    
    pool->tasks[pool->tail] = task;
    pool->tail = (pool->tail + 1) % TPOOL_MAX_TASKS;
    pool->task_count++;
    
    if (pool->task_count > pool->active_threads && pool->active_threads < pool->max_threads) {
        create_worker(pool);
    }
    
    pthread_cond_signal(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);
    
    return 0;
}

int thread_task_new(struct thread_task **task, thread_task_f function, void *arg) {
    *task = calloc(1, sizeof(struct thread_task));
    if (*task == NULL) {
        return TPOOL_ERR_NOT_IMPLEMENTED;
    }
    
    (*task)->function = function;
    (*task)->arg = arg;
    (*task)->is_pushed = false;
    (*task)->is_running = false;
    (*task)->is_finished = false;
    (*task)->is_detached = false;
    
    pthread_mutex_init(&(*task)->mutex, NULL);
    pthread_cond_init(&(*task)->cond, NULL);
    
    return 0;
}

bool thread_task_is_finished(const struct thread_task *task) {
    pthread_mutex_lock((pthread_mutex_t *)&task->mutex);
    bool finished = task->is_finished;
    pthread_mutex_unlock((pthread_mutex_t *)&task->mutex);
    return finished;
}

bool thread_task_is_running(const struct thread_task *task) {
    pthread_mutex_lock((pthread_mutex_t *)&task->mutex);
    bool running = task->is_running;
    pthread_mutex_unlock((pthread_mutex_t *)&task->mutex);
    return running;
}

int thread_task_join(struct thread_task *task, void **result) {
    pthread_mutex_lock(&task->mutex);
    
    if (!task->is_pushed) {
        pthread_mutex_unlock(&task->mutex);
        return TPOOL_ERR_TASK_NOT_PUSHED;
    }
    
    while (!task->is_finished) {
        pthread_cond_wait(&task->cond, &task->mutex);
    }
    
    if (result != NULL) {
        *result = task->result;
    }
    
    pthread_mutex_unlock(&task->mutex);
    return 0;
}

int thread_task_delete(struct thread_task *task) {
    pthread_mutex_lock(&task->mutex);
    
    if (task->is_pushed && !task->is_finished) {
        pthread_mutex_unlock(&task->mutex);
        return TPOOL_ERR_TASK_IN_POOL;
    }
    
    pthread_mutex_unlock(&task->mutex);
    
    pthread_mutex_destroy(&task->mutex);
    pthread_cond_destroy(&task->cond);
    free(task);
    
    return 0;
}

#if NEED_TIMED_JOIN
int thread_task_timed_join(struct thread_task *task, double timeout, void **result) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    
    ts.tv_sec += (long)timeout;
    ts.tv_nsec += (long)((timeout - (long)timeout) * 1000000000);
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    
    pthread_mutex_lock(&task->mutex);
    
    if (!task->is_pushed) {
        pthread_mutex_unlock(&task->mutex);
        return TPOOL_ERR_TASK_NOT_PUSHED;
    }
    
    int rc = 0;
    while (!task->is_finished && rc == 0) {
        rc = pthread_cond_timedwait(&task->cond, &task->mutex, &ts);
    }
    
    if (rc != 0) {
        pthread_mutex_unlock(&task->mutex);
        return TPOOL_ERR_TIMEOUT;
    }
    
    if (result != NULL) {
        *result = task->result;
    }
    
    pthread_mutex_unlock(&task->mutex);
    return 0;
}
#endif

#if NEED_DETACH
int thread_task_detach(struct thread_task *task) {
    pthread_mutex_lock(&task->mutex);
    
    if (!task->is_pushed) {
        pthread_mutex_unlock(&task->mutex);
        return TPOOL_ERR_TASK_NOT_PUSHED;
    }
    
    task->is_detached = true;
    
    if (task->is_finished) {
        pthread_mutex_unlock(&task->mutex);
        pthread_mutex_destroy(&task->mutex);
        pthread_cond_destroy(&task->cond);
        free(task);
    } else {
        pthread_mutex_unlock(&task->mutex);
    }
    
    return 0;
}
#endif