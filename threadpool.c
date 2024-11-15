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
#include <pthread.h>
#include "types.h"
#include "threadpool.h"

static bool task_queue_empty(TP_TaskQueue *task_queue)
{
    /* NOTE: Assumes the task_queue lock is acquired ! */
    return task_queue->head == task_queue->tail;
}

static TP_Task task_queue_next(TP_TaskQueue *task_queue)
{
    u32 head = task_queue->head;
    task_queue->head++;
    if (task_queue->head >= task_queue->size) {
        task_queue->head = 0;
    }
    return task_queue->tasks[head];
}

static void task_info_map_insert(TP_TaskInfoMap *map, TP_TaskHandle handle, TP_TaskInfo task_info);

static void task_info_map_resize(TP_TaskInfoMap *map, u32 new_capacity)
{
    /* NOTE: Assumes the task_queue lock is acquired ! */
    TP_TaskHandle *old_keys = map->keys;
    TP_TaskInfo *old_values = map->values;
    u32 old_capacity = map->slots_allocated;

    map->slots_allocated = new_capacity;
    map->keys = calloc(new_capacity, sizeof(TP_TaskHandle));
    map->values = malloc(new_capacity * sizeof(TP_TaskInfo));

    /* Re-insert the entries */
    map->slots_filled = 0;
    for (u32 i = 0; i < old_capacity; i++) {
        if (old_keys[i] != TASK_HANDLE_UNUSED) {
            task_info_map_insert(map, old_keys[i], old_values[i]);
        }
    }

    free(old_keys);
    free(old_values);
}

static void task_info_map_insert(TP_TaskInfoMap *map, TP_TaskHandle handle, TP_TaskInfo task_info)
{
    /* NOTE: Assumes the task_queue lock is acquired ! */
    if ((map->slots_filled + 1) * 100 >= map->slots_allocated * MAP_LOAD_FACTOR_PERCENT) {
        task_info_map_resize(map, map->slots_allocated * 2);
    }

    u32 index = handle % map->slots_allocated;
    while (map->keys[index] != TASK_HANDLE_UNUSED) {
        // TODO: if we ensure this is a power of two we can make this fast and remove the modulo
        index = (index + 1) % map->slots_allocated;
    }

    /* Insert the entry at the index */
    map->keys[index] = handle;
    map->values[index] = task_info;
    map->slots_filled++;
}

static bool task_info_map_update(TP_TaskInfoMap *map, TP_TaskHandle handle, TP_TaskInfo task_info)
{
    /* NOTE: Assumes the task_queue lock is acquired ! */
    u32 index = handle % map->slots_allocated;
    u32 start_index = index;

    while (map->keys[index] != handle) {
        index = (index + 1) % map->slots_allocated;
        if (index == start_index) {
            return false;
        }
    }

    map->values[index] = task_info;
    return true;
}

static TP_TaskInfo task_info_map_get(TP_TaskInfoMap *map, TP_TaskHandle handle, bool remove_if_finished)
{
    /* NOTE: Assumes the task_queue lock is acquired ! */
    u32 index = handle % map->slots_allocated;
    u32 start_index = index;

    while (map->keys[index] != handle) {
        index = (index + 1) % map->slots_allocated;
        if (index == start_index) {
            /* 
             * We completed a loop of the map without finding the handle.
             * In practice, this should never happen since trying to fetch the info of a hanlde
             * without a task info object should never really happen.
             */
            return (TP_TaskInfo){ .status = TASK_NOT_FOUND };
        }
    }

    TP_TaskInfo found = map->values[index];
    if (remove_if_finished && found.status == TASK_FINISHED) {
        map->keys[index] = TASK_HANDLE_UNUSED;
        map->slots_filled--;
    }
    return found;
}

static void *tp_worker_loop(void *arg)
{
    TP_ThreadPool *tp = (TP_ThreadPool*)arg;
    while (1) {
        pthread_mutex_lock(&tp->task_queue.lock);
        while (task_queue_empty(&tp->task_queue) && !tp->stop_new) {
            /* Only join when the queue is exhausted */
            if (tp->join) {
                pthread_mutex_unlock(&tp->task_queue.lock);
                return NULL;
            }
            pthread_cond_wait(&tp->task_queue.cv, &tp->task_queue.lock);
        }
        /* Do not run the new task */
        if (tp->stop_new) {
            pthread_mutex_unlock(&tp->task_queue.lock);
            return NULL;
        }
        TP_Task task = task_queue_next(&tp->task_queue);
        if (task.handle != TASK_HANDLE_UNUSED) { 
            TP_TaskInfo task_info = { .status = TASK_RUNNING, .task = task, .result = NULL };
            task_info_map_update(&tp->task_info_map, task.handle, task_info);
        }

        pthread_cond_signal(&tp->task_queue.cv);
        pthread_mutex_unlock(&tp->task_queue.lock);

        void *result = task.func(task.args);
        if (task.handle != TASK_HANDLE_UNUSED) { 
            pthread_mutex_lock(&tp->task_queue.lock);
            TP_TaskInfo task_info = { .status = TASK_FINISHED, .task = task, .result = result };
            task_info_map_update(&tp->task_info_map, task.handle, task_info);
            pthread_cond_signal(&tp->task_queue.cv);
            pthread_mutex_unlock(&tp->task_queue.lock);
        }
    }
    return NULL;
}

void tp_init(TP_ThreadPool *tp, u32 n_threads, u32 queue_size)
{
    tp->n_threads = n_threads;
    tp->threads = malloc(sizeof(pthread_t) * n_threads);

    tp->task_queue.tasks = malloc(sizeof(TP_Task) * queue_size);
    tp->task_queue.size = queue_size;
    tp->task_queue.head = 0;
    tp->task_queue.tail = 0;

    pthread_mutex_init(&tp->task_queue.lock, NULL);
    pthread_cond_init(&tp->task_queue.cv, NULL);

    tp->task_handle_counter = 0;
    tp->task_info_map.slots_filled = 0;
    tp->task_info_map.slots_allocated = 16;
    /* Zero allocate so the keys always start with the value of 0 aka TASK_HANDLE_UNUSED */
    tp->task_info_map.keys = calloc(tp->task_info_map.slots_allocated, sizeof(TP_TaskHandle));
    tp->task_info_map.values = malloc(sizeof(TP_TaskInfo) * tp->task_info_map.slots_allocated);

    tp->join = false;
    tp->stop_new = false;
}

void tp_destroy(TP_ThreadPool *tp)
{
    pthread_mutex_destroy(&tp->task_queue.lock);
    pthread_cond_destroy(&tp->task_queue.cv);
    free(tp->threads);
    free(tp->task_info_map.keys);
    free(tp->task_info_map.values);
}

void tp_start(TP_ThreadPool *tp)
{
    for (u32 i = 0; i < tp->n_threads; i++) {
	    pthread_create(&tp->threads[i], NULL, tp_worker_loop, (void *)tp);
    }
}

void tp_join(TP_ThreadPool *tp)
{
    pthread_mutex_lock(&tp->task_queue.lock);
    tp->join = true;
    pthread_cond_broadcast(&tp->task_queue.cv);
    pthread_mutex_unlock(&tp->task_queue.lock);

    for (u32 i = 0; i < tp->n_threads; i++) {
        pthread_join(tp->threads[i], NULL);
    }
}

void tp_stop_new(TP_ThreadPool *tp)
{
    pthread_mutex_lock(&tp->task_queue.lock);
    tp->stop_new = true;
    pthread_cond_broadcast(&tp->task_queue.cv);
    pthread_mutex_unlock(&tp->task_queue.lock);

    for (u32 i = 0; i < tp->n_threads; i++) {
        pthread_join(tp->threads[i], NULL);
    }
}

TP_TaskHandle tp_queue_task(TP_ThreadPool *tp, TP_Function f, void *args, bool wait_if_full, bool set_up_handle)
{
    pthread_mutex_lock(&tp->task_queue.lock);

    u32 tail;
    u32 next_tail;
    while (1) {
        tail = tp->task_queue.tail;
        next_tail = tp->task_queue.tail + 1;
        /* Wrap the ring buffer */
        if (next_tail >= tp->task_queue.size) {
            next_tail = 0;
        }
        if (next_tail == tp->task_queue.head) {
            /* Queue is full ! */
            if (!wait_if_full) {
                pthread_mutex_unlock(&tp->task_queue.lock);
                return TASK_HANDLE_FULL;
            }
            /* Wait for space in queue */
           	pthread_cond_wait(&tp->task_queue.cv, &tp->task_queue.lock);
            continue;
        } else {
            /* Queue is not full */
            break;
        }
    }

    tp->task_queue.tail = next_tail;

    TP_Task task = { .handle = TASK_HANDLE_UNUSED, .func = f, .args = args };
    if (set_up_handle) {
        tp->task_handle_counter++;
        task.handle = tp->task_handle_counter;

        TP_TaskInfo task_info = { .status = TASK_IN_QUEUE, .task = task, .result = NULL };
        task_info_map_insert(&tp->task_info_map, task.handle, task_info);
    }

    tp->task_queue.tasks[tail] = task;

    pthread_cond_signal(&tp->task_queue.cv);
    pthread_mutex_unlock(&tp->task_queue.lock);

    return task.handle;
}

TP_TaskInfo tp_get_info(TP_ThreadPool *tp, TP_TaskHandle handle, bool remove_if_finished)
{
    pthread_mutex_lock(&tp->task_queue.lock);
    TP_TaskInfo task_info = task_info_map_get(&tp->task_info_map, handle, remove_if_finished);
    pthread_mutex_unlock(&tp->task_queue.lock);
    return task_info;
}
