#include <stdio.h>
#include <unistd.h>
#include "types.h"
#include "threadpool.h"

#define N_THREADS 8
#define N_TASKS 32
#define STEPS (1e8)
#define H (1.0/STEPS)

void print_task_info(TP_ThreadPool *tp, TP_TaskHandle *handles, u32 n_handles)
{
    for (u32 i = 0; i < n_handles; i++) {
        TP_TaskInfo info = tp_get_info(tp, handles[i], false);
        printf("[%d]\t", handles[i]);
        switch (info.status) {
            case TASK_NOT_FOUND: printf("Not found"); break;
            case TASK_IN_QUEUE: printf("In queue"); break;
            case TASK_RUNNING: printf("Running"); break;
            case TASK_FINISHED: printf("Finished"); break;
        }
        if (info.result != NULL) {
            double *result = info.result;
            printf("\tresult: %f\n", *result);
        } else {
            printf("\n");
        }
    }
}

void *integrate(void *arg)
{
    u32 id = (u32)arg;
    f64 x = id * H;
    f64 *local = malloc(8); // Will use the OS as a GC :-)
    *local = 0.0;
    for (size_t i = 0; i < STEPS; i += N_THREADS) {
        x += N_THREADS * H;
        *local += H / (1.0 + x*x);
    }
    return local;
}

int main(void)
{
    TP_TaskHandle handles[N_TASKS];
    TP_ThreadPool tp;
    tp_init(&tp, 4, N_TASKS);

    printf("--- Start ---\n");
    tp_start(&tp);

    for (u32 i = 0; i < N_TASKS; i++) {
        TP_TaskHandle handle = tp_queue_task(&tp, integrate, (void *)(i + 1), false, true);
        if (handle == TASK_HANDLE_FULL) {
            printf("FULL\n");
        } else {
            handles[i] = handle;
        }
    }

    printf("Initial Task Info:\n");
    print_task_info(&tp, handles, N_TASKS);
    printf("\n");

    usleep(10000);
    printf("Task Info after ~10000 us:\n");
    print_task_info(&tp, handles, N_TASKS);
    printf("\n");

    usleep(40000);
    printf("Task Info after ~50000 us:\n");
    print_task_info(&tp, handles, N_TASKS);
    printf("\n");

    tp_join(&tp);

    printf("Task Info final:\n");
    print_task_info(&tp, handles, N_TASKS);
    printf("\n");

    tp_destroy(&tp);
    printf("--- End ---\n");
}
