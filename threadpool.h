/*
 *  Copyright (C) 2024 Nicolai Brand (lytix.dev)
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include "types.h"


typedef void *(*TP_Function)(void *);

#define MAP_LOAD_FACTOR_PERCENT 70

typedef u32 TP_TaskHandle;
// Task was submitted successfully to the queue, but the handle is not used
#define TASK_HANDLE_UNUSED 0
// Task was not successfully submitted as the queue was full
#define TASK_HANDLE_FULL U32_MAX

// NOTE: Could it be useful to allow a callback that runs when the task is finished?
typedef struct {
    TP_TaskHandle handle;
    TP_Function func;
    void *args;
} TP_Task;

typedef enum {
    TASK_NOT_FOUND,
    TASK_IN_QUEUE,
    TASK_RUNNING,
    TASK_FINISHED
} TP_TaskStatus;

typedef struct {
    TP_TaskStatus status;
    TP_Task task;
    void *result; // @NULLABLE
} TP_TaskInfo;

typedef struct {
    TP_TaskHandle *keys; // If entry is unoccupied or deleted, value is TASK_HANDLE_UNUSED
    TP_TaskInfo *values;
    u32 slots_allocated;
    u32 slots_filled; // Number of active entries (i.e no unoccupied or deleted entries)
} TP_TaskInfoMap;


/* FIFO queue implemented as a ring buffer */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cv;
    TP_Task *tasks;
    u32 size;
    u32 head; // Index to the next task to be handled
    u32 tail; // Index after the most recently queued task
} TP_TaskQueue;

typedef struct threadpool_t {
    u32 n_threads;
    pthread_t *threads;
    TP_TaskQueue task_queue;

    TP_TaskHandle task_handle_counter;
    TP_TaskInfoMap task_info_map;

    bool join; // Wait for all running and queued tasks to finish
    bool stop_new; // Stop new tasks from being started, but let threads finish what they're doing
} TP_ThreadPool;


/* Functions */
void tp_init(TP_ThreadPool *tp, u32 n_threads, u32 queue_size);
void tp_destroy(TP_ThreadPool *tp);

void tp_start(TP_ThreadPool *tp);
void tp_join(TP_ThreadPool *tp);
void tp_stop_new(TP_ThreadPool *tp);

/*
 * If wait_if_full then the the function wait until the queue has space for the task
 *
 * If set_up_handle then the function returns a valid handle that later can be used to get the
 *  info of the task using tp_get_info(). NOTE: make sure that the TP_Function store its return 
 *  value somewhere other than the stack so it does not turn into garbage.
 */
TP_TaskHandle tp_queue_task(TP_ThreadPool *tp, TP_Function f, void *args, bool wait_if_full, bool set_up_handle);

/*
 * If remove_if_finished then the threadpool will internally forget about the handle and repurpose 
 *  the memory.
 */
TP_TaskInfo tp_get_info(TP_ThreadPool *tp, TP_TaskHandle handle, bool remove_if_finished);

#endif /* THREADPOOL_H */
