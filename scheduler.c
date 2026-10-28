#include "scheduler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "msg.h"

/* TODO (opcional, tarefa 5): acrescentar "RR" a esta lista e ao enum sched_algo_en. */
static const char *SCHED_NAMES[] = { "FIFO", NULL };
static sched_algo_en sched_algo = SCHED_FIFO;

int set_sched_algo(const char *name) {
    for (int i = 0; SCHED_NAMES[i] != NULL; i++) {
        if (strcasecmp(SCHED_NAMES[i], name) == 0) {
            sched_algo = (sched_algo_en) i;
            return sched_algo;
        }
    }
    return -1;
}

const char *get_sched_algo_str(void) {
    return SCHED_NAMES[sched_algo];
}

/** Send DONE to the application and move the finished burst to the command queue. */
void finish_burst(uint32_t current_time_ms, queue_t *cq, pcb_t *task) {
    msg_t msg = {
        .pid = task->pid,
        .request = PROCESS_REQUEST_DONE,
        .time_ms = current_time_ms
    };
    if (write(task->sockfd, &msg, sizeof(msg_t)) != sizeof(msg_t)) {
        perror("write");
    }
    enqueue_pcb(cq, task);
}

/* FIFO — não preemptivo: corre o burst até ao fim. */
int sched_fifo(uint32_t current_time_ms, queue_t *rq, queue_t *cq, pcb_t **cpu_task) {
    if (*cpu_task) {
        (*cpu_task)->ellapsed_time_ms += TICKS_MS;
        if ((*cpu_task)->ellapsed_time_ms >= (*cpu_task)->time_ms) {
            finish_burst(current_time_ms, cq, *cpu_task);
            *cpu_task = NULL;
        }
    }

    if (*cpu_task == NULL) {
        *cpu_task = dequeue_pcb(rq);
        if (*cpu_task) {
            (*cpu_task)->slice_start_ms = current_time_ms;
            return 1;
        }
    }
    return 0;
}

int scheduler(uint32_t current_time_ms, queue_t *rq, queue_t *cq, pcb_t **cpu_task) {
    switch (sched_algo) {
        case SCHED_FIFO: return sched_fifo(current_time_ms, rq, cq, cpu_task);
    }
    return 0;
}
