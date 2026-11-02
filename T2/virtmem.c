//
// Subsistema de memória virtual (thread única — Trabalho 2).
//

#include "virtmem_types.h"
#include "virtmem.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "queue.h"

/* No ponto de partida só o FIFO está disponível.
 * TODO (alunos): acrescentar "RANDOM", "LRU", "NRU" e "CLOCK" a esta lista (pela
 * mesma ordem do enum eviction_algo_en em virtmem.h), ao switch de evict() e
 * implementar as funções random_eviction() / lru_eviction() / nru_eviction() /
 * clock_eviction(). */
const char* eviction_algo_str[] = { "FIFO", "RANDOM", "LRU","NRU","CLOCK", NULL };

static eviction_algo_en eviction_algo = FIFO;

static int npageacc = 0;
static int npagefaults = 0;
static int nswapin = 0;
static int nswapout = 0;

int set_eviction_algo(const char *ev_str) {
    for (int i = 0; eviction_algo_str[i] != NULL; i++) {
        if (strcasecmp(eviction_algo_str[i], ev_str) == 0) {
            eviction_algo = (eviction_algo_en) i;
            return eviction_algo;
        }
    }
    return -1;
}

const char *get_eviction_algo_str(void) {
    return eviction_algo_str[eviction_algo];
}

/* ------------------------------------------------------------------ */
/* Criação de estruturas                                              */
/* ------------------------------------------------------------------ */

frame_table_t *create_frame_table(int num_pframes) {
    if (num_pframes <= 0) {
        printf("create_frame_table: invalid num_pframes=%d\n", num_pframes);
        return NULL;
    }

    frame_table_t *ft = malloc(sizeof(frame_table_t));
    if (!ft) {
        printf("Cannot allocate memory for frame table\n");
        return NULL;
    }

    ft->frames = calloc((size_t)num_pframes, sizeof(frame_desc_t));
    if (!ft->frames) {
        printf("Cannot allocate memory for frame descriptors\n");
        free(ft);
        return NULL;
    }
    ft->num_pframes = num_pframes;

    if (init_free_stack(&ft->free_stack, num_pframes) < 0) {
        free(ft->frames);
        free(ft);
        return NULL;
    }
    if (init_eviction_arr(&ft->eviction_order, num_pframes) < 0) {
        printf("Cannot allocate memory for eviction order\n");
        free(ft->free_stack.ids);
        free(ft->frames);
        free(ft);
        return NULL;
    }
    return ft;
}

int create_page_table(page_table_t *pt, int num_vpages) {
    if (pt == NULL || num_vpages <= 0) {
        printf("Invalid page table\n");
        return -1;
    }
    pt->ptes = malloc((size_t)num_vpages * sizeof(pte_t));
    if (pt->ptes == NULL) {
        printf("Cannot allocate memory for page table\n");
        return -1;
    }
    pt->nvalid = num_vpages;
    for (int i = 0; i < num_vpages; ++i) {
        pt->ptes[i].pfn = INVALID_PFN;
        pt->ptes[i].present = 0;
        pt->ptes[i].referenced = 0;
        pt->ptes[i].dirty = 0;
        pt->ptes[i].last_accessed = 0;
    }
    return 0;
}

int is_active(pte_t *page) {
    return page != NULL && page->present;
}

int is_valid(pte_t *page) {
    return page != NULL && page->pfn != INVALID_PFN;
}

pte_t *find_page(page_table_t *pt, int32_t vpn) {
    if (pt == NULL || vpn < 0 || (uint32_t)vpn > pt->nvalid) {
        return NULL;
    }
    return &pt->ptes[vpn];
}

/* ------------------------------------------------------------------ */
/* Pilha de frames livres                                            */
/* ------------------------------------------------------------------ */

int init_free_stack(free_stack_t *stack, int num_pframes) {
    if (stack == NULL || num_pframes <= 0) {
        printf("Invalid free stack\n");
        return -1;
    }
    stack->ids = malloc((size_t)num_pframes * sizeof(uint32_t));
    if (stack->ids == NULL) {
        printf("Cannot allocate memory for free stack\n");
        return -1;
    }
    stack->top = num_pframes - 1;
    stack->max_size = num_pframes;
    for (int i = 0; i < num_pframes; ++i) stack->ids[i] = i;
    return 0;
}

int push_free_frame(free_stack_t *stack, int pfn) {
    if (stack == NULL || pfn < 0 || stack->top >= (stack->max_size - 1)) {
        return 0;
    }
    stack->ids[++(stack->top)] = (uint32_t)pfn;
    return 1;
}

// FIFO na prática: devolve o elemento mais antigo (fundo) e faz shift.
int pop_free_frame(free_stack_t *stack) {
    if (stack == NULL || stack->top < 0) return INVALID_PFN;
    int pfn = stack->ids[0];
    for (int i = 1; i <= stack->top; ++i) stack->ids[i - 1] = stack->ids[i];
    stack->top--;
    return pfn;
}

/* ------------------------------------------------------------------ */
/* Swap (backing store)                                              */
/* ------------------------------------------------------------------ */

int swap_out(swap_hash_t *swap, frame_desc_t *fd) {
    pte_t *pte = fd->pte;
    uint64_t page_key = (((uint64_t)fd->pid) << 32) | ((uint64_t)fd->vpn);

    // B4: se a página já lá está (foi swap-in sem apagar), atualiza no lugar.
    swapped_page_t *existing = NULL;
    HASH_FIND(hh, swap->pages, &page_key, sizeof(uint64_t), existing);
    if (existing) {
        existing->dirty = pte->dirty;
        existing->last_accessed = pte->last_accessed;
        nswapout++;
        return 0;
    }

    swapped_page_t *swapped_page = malloc(sizeof(swapped_page_t));
    if (!swapped_page) {
        printf("Cannot allocate memory for swapped page\n");
        return -1;
    }
    swapped_page->page_id = page_key;
    swapped_page->dirty = pte->dirty;
    swapped_page->last_accessed = pte->last_accessed;
    HASH_ADD(hh, swap->pages, page_id, sizeof(uint64_t), swapped_page);
    swap->num_swapped += 1;
    nswapout++;
    return 0;
}

void destroy_swap(swap_hash_t *swap) {
    if (swap == NULL) return;
    swapped_page_t *swapped_page, *tmp;
    HASH_ITER(hh, swap->pages, swapped_page, tmp) {
        HASH_DEL(swap->pages, swapped_page);
        free(swapped_page);
    }
    swap->num_swapped = 0;
}

int swap_in(swap_hash_t *swap, frame_desc_t *fd) {
    pte_t *pte = fd->pte;
    uint64_t page_key = (((uint64_t)fd->pid) << 32) | ((uint64_t)fd->vpn);
    swapped_page_t *swapped_page = NULL;
    HASH_FIND(hh, swap->pages, &page_key, sizeof(uint64_t), swapped_page);
    if (!swapped_page) {
        printf("vpn %d of process %d not found in swap\n", fd->vpn, fd->pid);
        return -1;
    }
    // A cópia em swap mantém-se: a página volta limpa e, se não for escrita,
    // não precisa de ser escrita outra vez na próxima evicção.
    pte->dirty = 0;
    pte->last_accessed = swapped_page->last_accessed;
    nswapin++;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Pedido e evicção de páginas                                       */
/* ------------------------------------------------------------------ */

pte_t *page_request(pcb_t *pcb, frame_table_t *frame_table, swap_hash_t *swap, int vpn) {
    uint32_t current_time_ms = now_ms();
    npageacc++;

    pte_t *pte = find_page(&pcb->page_table, vpn);
    if (pte == NULL) {
        printf("ERROR: invalid vpn %d for process %d\n", vpn, pcb->pid);
        return NULL;
    }

    if (is_active(pte)) {                       // page hit
        //TODO ADICIONADO PRINTF
        printf("[PID %d] Page Hit: VPN %d Found in Frame %d.\n", pcb->pid, vpn, pte->pfn);
        update_eviction_array(&frame_table->eviction_order, pte->pfn, current_time_ms);
        return pte;
    }

    npagefaults++;
    //TODO ADICIONADO PRINTF
    printf("[PID %d] Page Fault: VPN %d Requested but not in RAM.\n", pcb->pid, vpn);

    if (is_valid(pte)) {                        // está em swap
        int32_t next_pfn = pop_free_frame(&frame_table->free_stack);
        if (next_pfn == INVALID_PFN) {     // B2
            printf("ERROR: no free pframe to swap in vpn %d of process %d\n", vpn, pcb->pid);
            return NULL;
        }
        frame_desc_t *fd = &frame_table->frames[next_pfn];
        fd->pte = pte;
        fd->pid = pcb->pid;
        fd->vpn = vpn;
        if (swap_in(swap, fd) < 0) {
            printf("WARNING: swap-in failed for vpn %d of process %d\n", vpn, pcb->pid);
        }
        pte->pfn = next_pfn;
        update_eviction_array(&frame_table->eviction_order, next_pfn, current_time_ms);
        return pte;
    }

    // Página nunca alocada → alocar frame livre
    int32_t next_pfn = pop_free_frame(&frame_table->free_stack);
    if (next_pfn == INVALID_PFN) {          // B2
        printf("ERROR: no free pframe to allocate vpn %d of process %d\n", vpn, pcb->pid);
        return NULL;
    }
    frame_desc_t *fd = &frame_table->frames[next_pfn];
    pte->pfn = next_pfn;
    fd->pte = pte;
    fd->pid = pcb->pid;
    fd->vpn = vpn;
    fd->pte->dirty = 1;                          // páginas novas nascem "dirty"
    update_eviction_array(&frame_table->eviction_order, next_pfn, current_time_ms);
    return pte;
}

int page_eviction(frame_table_t *frame_table, swap_hash_t *swap, int32_t min_pages_threshold) {
    while (frame_table->free_stack.top + 1 < min_pages_threshold) {
        int evict_pfn = evict(&frame_table->eviction_order);
        if (evict_pfn == INVALID_PFN) {
            printf("No pframe to evict!\n");
            return -1;
        }
        frame_desc_t *fd = &frame_table->frames[evict_pfn];
        pte_t *pte = fd->pte;
        if (!pte) {
            printf("pfn %d has no valid vpn to evict!\n", evict_pfn);
            continue;
        }
        //TODO ADDED PRINTF
        printf("[PID %d] Eviction: Frame %d -> VPN %d EVICTED.\n", fd->pid, evict_pfn, fd->vpn);
        pte->present = 0;
        if (pte->dirty) {

            //TODO ADDED PRINTF
            printf("[PID %d] Dirty Page: VPN %d is DIRTY -> swap-out to disc.\n", fd->pid, fd->vpn);
            if (swap_out(swap, fd) < 0) {
                printf("Failed to swap out vpn %d of process %d\n", fd->vpn, fd->pid);
            }
        }
        push_free_frame(&frame_table->free_stack, evict_pfn);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Libertação de páginas quando o processo termina                   */
/* ------------------------------------------------------------------ */

int free_frames_from_memory(frame_table_t *frame_table, int32_t pid) {
    int count = 0;
    for (int i = 0; i < frame_table->num_pframes; i++) {
        frame_desc_t *fd = &frame_table->frames[i];
        if (fd->pid == pid) {
            push_free_frame(&frame_table->free_stack, i);
            remove_from_eviction_array(&frame_table->eviction_order, i);   // B3
            if (fd->pte) fd->pte->pfn = INVALID_PFN;
            fd->pte = NULL;
            fd->pid = -1;
            fd->vpn = 0;
            count++;
        }
    }
    return count;
}

int swap_del(swap_hash_t *swap, int32_t pid, int32_t vpn) {
    uint64_t page_key = (((uint64_t)pid) << 32) | ((uint64_t)vpn);
    swapped_page_t *swapped_page = NULL;
    HASH_FIND(hh, swap->pages, &page_key, sizeof(uint64_t), swapped_page);
    if (!swapped_page) return 0;
    HASH_DEL(swap->pages, swapped_page);
    free(swapped_page);
    swap->num_swapped -= 1;
    return 1;
}

int free_frames_from_swap(swap_hash_t *swap, int32_t pid, int32_t pages) {
    int count = 0;
    for (int i = 0; i < pages; i++) {
        count += swap_del(swap, pid, i + 1);
    }
    return count;
}

void free_pages(frame_table_t *frame_table, swap_hash_t *swap, pcb_t *pcb) {
    int mem = free_frames_from_memory(frame_table, pcb->pid);
    int sw  = free_frames_from_swap(swap, pcb->pid, pcb->page_table.nvalid);
    for (uint32_t i = 0; i < pcb->page_table.nvalid; i++) {
        pcb->page_table.ptes[i].pfn = INVALID_PFN;
        pcb->page_table.ptes[i].present = 0;
    }
    printf("Process %d terminated: freed %d pframes from RAM, %d vpages from swap\n", pcb->pid, mem, sw);
    printf("%s stats: accesses=%d faults=%d swap-in=%d swap-out=%d fault-rate=%.3f\n",
           get_eviction_algo_str(), npageacc, npagefaults, nswapin, nswapout,
           npageacc ? (float)npagefaults / (float)npageacc : 0.0f);
}

/* ------------------------------------------------------------------ */
/* Estrutura de ordem de evicção                                     */
/* ------------------------------------------------------------------ */

int init_eviction_arr(array_t *array, int num_pframes) {
    if (array == NULL || num_pframes <= 0) return -1;
    array->ids = malloc((size_t)num_pframes * sizeof(array_elem_t));
    if (array->ids == NULL) {
        printf("Cannot allocate memory for eviction array\n");
        return -1;
    }
    array->top = -1;
    array->max_size = num_pframes;
    array->last_index = 0;
    srand((unsigned)time(NULL));   // usado pelo RANDOM
    return 0;
}

/**
 * FIFO: devolve a frame que está há mais tempo carregada (a da frente) e faz shift.
 */
int32_t fifo_eviction(array_t *fifo) {
    if (fifo == NULL || fifo->top < 0) return INVALID_PFN;
    int32_t pfn = fifo->ids[0].pfn;
    for (int i = 1; i <= fifo->top; ++i) {
        fifo->ids[i - 1] = fifo->ids[i];
    }
    fifo->top--;
    return pfn;
}

//RANDOM

/**
 * Regista um acesso a `pfn` na estrutura de ordem de evicção.
 * Se a frame ainda não estiver registada, é acrescentada no fim (ordem FIFO).
 */
int update_eviction_array(array_t *eviction_arr, int pfn, uint32_t current_time_ms) {
    if (eviction_arr == NULL || pfn < 0 || eviction_arr->top >= (eviction_arr->max_size - 1)) {
        // já registada? só atualizar timestamps / use bit
        if (eviction_arr) {
            for (int i = 0; i <= eviction_arr->top; i++) {
                if (eviction_arr->ids[i].pfn == (uint32_t)pfn) {
                    eviction_arr->ids[i].last_accessed = current_time_ms;
                    eviction_arr->ids[i].use_bit = 1;
                    return 1;
                }
            }
        }
        return 0;
    }
    for (int i = 0; i <= eviction_arr->top; i++) {
        if (eviction_arr->ids[i].pfn == (uint32_t)pfn) {
            eviction_arr->ids[i].last_accessed = current_time_ms;
            eviction_arr->ids[i].use_bit = 1;
            return 1;
        }
    }
    eviction_arr->top++;
    eviction_arr->ids[eviction_arr->top].pfn     = pfn;
    eviction_arr->ids[eviction_arr->top].last_accessed = current_time_ms;
    eviction_arr->ids[eviction_arr->top].use_bit       = 1;
    return 1;
}

/**
 * B3: retira `pfn` da estrutura de ordem de evicção (quando a frame é
 * libertada porque o processo terminou).
 */
void remove_from_eviction_array(array_t *eviction_arr, int pfn) {
    if (eviction_arr == NULL) return;
    for (int i = 0; i <= eviction_arr->top; i++) {
        if (eviction_arr->ids[i].pfn == (uint32_t)pfn) {
            for (int j = i + 1; j <= eviction_arr->top; j++) {
                eviction_arr->ids[j - 1] = eviction_arr->ids[j];
            }
            eviction_arr->top--;
            if (eviction_arr->last_index > eviction_arr->top) {
                eviction_arr->last_index = 0;
            }
            return;
        }
    }
}

int evict(array_t *eviction_arr) {
    switch (eviction_algo) {
        case FIFO:
            return fifo_eviction(eviction_arr);
        default:
            printf("Eviction algorithm '%s' not implemented yet\n", get_eviction_algo_str());
            return INVALID_PFN;
    }
}
