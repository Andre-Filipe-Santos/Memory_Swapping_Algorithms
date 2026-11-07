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

/* Nomes aceites por --vm, pela mesma ordem do enum eviction_algo_en
 * (virtmem.h). Cada um tem a sua função de evicção, escolhida em evict():
 * fifo_eviction(), random_eviction(), lru_eviction(), nru_eviction() e
 * clock_eviction(). */
const char *eviction_algo_str[] = {"FIFO", "RANDOM", "LRU", "NRU", "CLOCK", NULL};

static eviction_algo_en eviction_algo = FIFO;

// NRU: limpeza periódica dos use bits (definida mais abaixo, junto do NRU).
static void nru_tick(frame_table_t *ft, uint32_t current_time_ms);

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

    ft->frames = calloc((size_t) num_pframes, sizeof(frame_desc_t));
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
    pt->ptes = malloc((size_t) num_vpages * sizeof(pte_t));
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
    if (pt == NULL || vpn < 0 || (uint32_t) vpn > pt->nvalid) {
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
    stack->ids = malloc((size_t) num_pframes * sizeof(uint32_t));
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
    stack->ids[++(stack->top)] = (uint32_t) pfn;
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
    uint64_t page_key = (((uint64_t) fd->pid) << 32) | ((uint64_t) fd->vpn);

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
    uint64_t page_key = (((uint64_t) fd->pid) << 32) | ((uint64_t) fd->vpn);
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

    if (is_active(pte)) {
        // page hit
        //TODO ADICIONADO PRINTF PAGE HIT
        printf("[PID %d] Page Hit: VPN %d Found in Frame %d.\n\n", pcb->pid, vpn, pte->pfn);
        update_eviction_array(&frame_table->eviction_order, pte->pfn, current_time_ms);
        return pte;
    }

    npagefaults++;
    //TODO ADICIONADO PRINTF PAGE FAULT
    printf("[PID %d] Page Fault: VPN %d Requested but not in RAM.\n\n", pcb->pid, vpn);

    if (is_valid(pte)) {
        // está em swap
        int32_t next_pfn = pop_free_frame(&frame_table->free_stack);
        if (next_pfn == INVALID_PFN) {
            // B2
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
    if (next_pfn == INVALID_PFN) {
        // B2
        printf("ERROR: no free pframe to allocate vpn %d of process %d\n", vpn, pcb->pid);
        return NULL;
    }
    frame_desc_t *fd = &frame_table->frames[next_pfn];
    pte->pfn = next_pfn;
    fd->pte = pte;
    fd->pid = pcb->pid;
    fd->vpn = vpn;
    fd->pte->dirty = 1; // páginas novas nascem "dirty"
    update_eviction_array(&frame_table->eviction_order, next_pfn, current_time_ms);
    return pte;
}

int page_eviction(frame_table_t *frame_table, swap_hash_t *swap, int32_t min_pages_threshold) {
    // NRU: antes de cada acesso, aplicar a limpeza periódica dos use bits se
    // tiver terminado um período (não faz nada com os outros algoritmos).
    nru_tick(frame_table, now_ms());

    while (frame_table->free_stack.top + 1 < min_pages_threshold) {
        int evict_pfn = evict(frame_table);
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
        //TODO ADDED PRINTF EVICTION
        printf("[PID %d] Eviction: Frame %d -> VPN %d EVICTED.\n", fd->pid, evict_pfn, fd->vpn);
        pte->present = 0;
        if (pte->dirty) {
            //TODO ADDED PRINTF DIRTY PAGES
            printf("[PID %d] Dirty Page: VPN %d is DIRTY -> swap-out to disc.\n\n", fd->pid, fd->vpn);
            if (swap_out(swap, fd) < 0) {
                printf("Failed to swap out vpn %d of process %d\n", fd->vpn, fd->pid);
            }
        } else {
            // Página limpa: a cópia em swap já está atualizada, não é preciso escrever.
            printf("[PID %d] Clean Page: VPN %d is CLEAN -> discarded, no swap-out.\n\n", fd->pid, fd->vpn);
        }
        push_free_frame(&frame_table->free_stack, evict_pfn);
        // A frame voltou a estar livre: apagar o registo do dono antigo.
        fd->pte = NULL;
        fd->pid = -1;
        fd->vpn = 0;
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
            remove_from_eviction_array(&frame_table->eviction_order, i); // B3
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
    uint64_t page_key = (((uint64_t) pid) << 32) | ((uint64_t) vpn);
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
    int sw = free_frames_from_swap(swap, pcb->pid, pcb->page_table.nvalid);
    for (uint32_t i = 0; i < pcb->page_table.nvalid; i++) {
        pcb->page_table.ptes[i].pfn = INVALID_PFN;
        pcb->page_table.ptes[i].present = 0;
    }
    printf("Process %d terminated: freed %d pframes from RAM, %d vpages from swap\n", pcb->pid, mem, sw);
    printf("%s stats: accesses=%d faults=%d swap-in=%d swap-out=%d fault-rate=%.3f\n",
           get_eviction_algo_str(), npageacc, npagefaults, nswapin, nswapout,
           npageacc ? (float) npagefaults / (float) npageacc : 0.0f);
}

/* ------------------------------------------------------------------ */
/* Estrutura de ordem de evicção                                     */
/* ------------------------------------------------------------------ */

int init_eviction_arr(array_t *array, int num_pframes) {
    if (array == NULL || num_pframes <= 0) return -1;
    array->ids = malloc((size_t) num_pframes * sizeof(array_elem_t));
    if (array->ids == NULL) {
        printf("Cannot allocate memory for eviction array\n");
        return -1;
    }
    array->top = -1;
    array->max_size = num_pframes;
    array->last_index = 0;
    srand((unsigned) time(NULL)); // usado pelo RANDOM
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

/* ------------------------------------------------------------------ */
/* Funções auxiliares usadas pelos algoritmos RANDOM, LRU, NRU, CLOCK */
/* ------------------------------------------------------------------ */

/**
 * Retira da estrutura de ordem de evicção o elemento que está na posição
 * `idx` e devolve o pfn (número da frame física) que lá estava.
 *
 * É usada por todos os algoritmos novos: cada algoritmo só tem de DECIDIR
 * qual é a posição da vítima; a remoção é sempre feita aqui, da mesma forma.
 */
static int32_t remove_at_index(array_t *arr, int idx) {
    // Passo 1: validar os argumentos (array vazio ou índice fora dos limites).
    if (arr == NULL || idx < 0 || idx > arr->top) return INVALID_PFN;

    // Passo 2: guardar o pfn da vítima antes de o elemento ser sobreposto.
    int32_t pfn = (int32_t) arr->ids[idx].pfn;

    // Passo 3: deslocar uma posição para trás todos os elementos seguintes.
    //          Assim o array continua contíguo (posições 0..top) e mantém a
    //          ordem relativa de carregamento das restantes páginas.
    for (int j = idx + 1; j <= arr->top; j++) {
        arr->ids[j - 1] = arr->ids[j];
    }

    // Passo 4: há menos um elemento no array.
    arr->top--;

    // Passo 5: manter o ponteiro do relógio (last_index, usado pelo CLOCK)
    //          a apontar para a MESMA página para onde apontava antes:
    //          - se o elemento retirado estava antes do ponteiro, todos os
    //            elementos a partir dele recuaram uma posição, por isso o
    //            ponteiro também recua uma posição;
    //          - se o elemento retirado era o apontado, o elemento seguinte
    //            ocupa agora a mesma posição, por isso o ponteiro fica igual
    //            (passa a apontar para a página seguinte à vítima);
    //          - se o ponteiro ficar para lá do fim, dá a volta para o início
    //            (a lista é circular).
    if (idx < arr->last_index) {
        arr->last_index--;
    }
    if (arr->last_index > arr->top) {
        arr->last_index = 0;
    }

    return pfn;
}

/**
 * Imprime o estado atual da estrutura de ordem de evicção, para demonstrar
 * que o algoritmo escolhido toma a decisão correta (requisito 4 do enunciado).
 *
 * Formato de cada elemento:  [posição]F<frame>(P<pid> V<vpn> use=<use bit> t=<último acesso em ms>)
 * O ponteiro do relógio (CLOCK) é assinalado com o carácter '>'.
 */
static void print_eviction_state(const frame_table_t *ft, const char *algo) {
    const array_t *arr = &ft->eviction_order;

    //TODO ADDED PRINTF EVICTION ORDER
    printf("[%s] Eviction order (%d frames, oldest loaded first):\n", algo, arr->top + 1);
    for (int i = 0; i <= arr->top; i++) {
        const array_elem_t *e = &arr->ids[i];
        const frame_desc_t *fd = &ft->frames[e->pfn];
        printf(" %s[%d]F%u(P%d V%u use=%u t=%u)\n",
               (eviction_algo == CLOCK && i == arr->last_index) ? ">" : "",
               i, e->pfn, fd->pid, fd->vpn, e->use_bit, e->last_accessed);
    }
    printf("\n");
}

/**
 * Regista um acesso a `pfn` na estrutura de ordem de evicção.
 * Se a frame ainda não estiver registada, é acrescentada no fim (ordem FIFO).
 */
int update_eviction_array(array_t *eviction_arr, int pfn, uint32_t current_time_ms) {
    if (eviction_arr == NULL || pfn < 0 || eviction_arr->top >= (eviction_arr->max_size - 1)) {
        // já registada? só atualizar timestamps / use bit
        if (eviction_arr) {
            for (int i = 0; i <= eviction_arr->top; i++) {
                if (eviction_arr->ids[i].pfn == (uint32_t) pfn) {
                    eviction_arr->ids[i].last_accessed = current_time_ms;
                    eviction_arr->ids[i].use_bit = 1;
                    return 1;
                }
            }
        }
        return 0;
    }
    for (int i = 0; i <= eviction_arr->top; i++) {
        if (eviction_arr->ids[i].pfn == (uint32_t) pfn) {
            eviction_arr->ids[i].last_accessed = current_time_ms;
            eviction_arr->ids[i].use_bit = 1;
            return 1;
        }
    }
    eviction_arr->top++;
    eviction_arr->ids[eviction_arr->top].pfn = pfn;
    eviction_arr->ids[eviction_arr->top].last_accessed = current_time_ms;
    eviction_arr->ids[eviction_arr->top].use_bit = 1;
    return 1;
}

/**
 * B3: retira `pfn` da estrutura de ordem de evicção (quando a frame é
 * libertada porque o processo terminou).
 */
void remove_from_eviction_array(array_t *eviction_arr, int pfn) {
    if (eviction_arr == NULL) return;
    for (int i = 0; i <= eviction_arr->top; i++) {
        if (eviction_arr->ids[i].pfn == (uint32_t) pfn) {
            // Usa a mesma função de remoção dos algoritmos, para que o
            // ponteiro do relógio (CLOCK) continue a apontar para a mesma
            // página quando um processo termina e as suas frames saem do array.
            remove_at_index(eviction_arr, i);
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* RANDOM                                                             */
/* ------------------------------------------------------------------ */

/**
 * RANDOM: "Seleciona uma página aleatoriamente"
 *
 * Todas as páginas em memória são candidatas, de qualquer processo
 * (slide 98: "Essas páginas podem pertencer a qualquer processo").
 * O gerador rand() é inicializado com srand(time(NULL)) em init_eviction_arr(),
 * por isso a escolha muda de execução para execução ("a execução não é
 * determinística", slide 104).
 */
int32_t random_eviction(frame_table_t *ft) {
    array_t *arr = &ft->eviction_order;

    // Passo 1: se não houver páginas em memória, não há vítima possível.
    if (arr->top < 0) return INVALID_PFN;

    // Passo 2: mostrar as páginas candidatas (todas as que estão em memória).
    print_eviction_state(ft, "RANDOM");

    // Passo 3: número de candidatas = número de elementos do array (0..top).
    int num_candidates = arr->top + 1;

    // Passo 4: sortear uma posição entre 0 e num_candidates - 1.
    int victim_idx = rand() % num_candidates;

    printf("[RANDOM] rand() %% %d = %d -> victim is frame %u (VPN %u)\n",
           num_candidates, victim_idx, arr->ids[victim_idx].pfn,
           ft->frames[arr->ids[victim_idx].pfn].vpn);

    // Passo 5: retirar a vítima do array e devolver a sua frame.
    return remove_at_index(arr, victim_idx);
}

/* ------------------------------------------------------------------ */
/* LRU                                                                */
/* ------------------------------------------------------------------ */

/**
 * LRU (Least Recently Used): "Substitui a página que não é utilizada há mais
 * tempo" (slide 107).
 *
 * Implementação do slide 111: em cada acesso é registado um timestamp
 * (campo last_accessed, atualizado por update_eviction_array() em cada page
 * hit e em cada carregamento, com o tempo do simulador em ms - now_ms()).
 * Na substituição percorre-se a estrutura e escolhe-se a página com o
 * timestamp MAIS ANTIGO (o menor valor).
 *
 * Empates: os acessos de um mesmo burst são feitos todos no mesmo instante,
 * por isso várias páginas podem ter o mesmo timestamp. Nesse caso é
 * escolhida a que foi carregada há mais tempo (a que aparece primeiro no
 * array, que está por ordem de carregamento).
 */
int32_t lru_eviction(frame_table_t *ft) {
    array_t *arr = &ft->eviction_order;

    // Passo 1: se não houver páginas em memória, não há vítima possível.
    if (arr->top < 0) return INVALID_PFN;

    // Passo 2: mostrar os timestamps de todas as páginas em memória.
    print_eviction_state(ft, "LRU");

    // Passo 3: começar por assumir que a vítima é a primeira página do array.
    int victim_idx = 0;

    // Passo 4: percorrer as restantes páginas e ficar com a de timestamp
    //          mais antigo. A comparação é ESTRITA (<): em caso de empate
    //          mantém-se a que apareceu primeiro, ou seja, a carregada há
    //          mais tempo.
    for (int i = 1; i <= arr->top; i++) {
        if (arr->ids[i].last_accessed < arr->ids[victim_idx].last_accessed) {
            victim_idx = i;
        }
    }

    printf("[LRU] Oldest last access t=%u ms -> victim is frame %u (VPN %u)\n\n",
           arr->ids[victim_idx].last_accessed, arr->ids[victim_idx].pfn,
           ft->frames[arr->ids[victim_idx].pfn].vpn);

    // Passo 5: retirar a vítima do array e devolver a sua frame.
    return remove_at_index(arr, victim_idx);
}

/* ------------------------------------------------------------------ */
/* NRU                                                                */
/* ------------------------------------------------------------------ */

/**
 * NRU - "Periodicamente, o SO coloca os use bit = 0" (slide 112).
 *
 * Coloca a 0 o use bit de todas as páginas em memória. É chamada por
 * nru_tick() uma vez por cada período de NRU_PERIOD_MS ms. Depois da limpeza,
 * o "hardware" volta a pôr use bit = 1 em cada página que for acedida (feito
 * em update_eviction_array()).
 *
 * period_end_ms: instante em que terminou o período (múltiplo de NRU_PERIOD_MS)
 * current_time_ms: instante em que a limpeza foi efetivamente aplicada
 */
static void nru_clear_use_bits(frame_table_t *ft, uint32_t period_end_ms,
                               uint32_t current_time_ms) {
    array_t *arr = &ft->eviction_order;

    // Passo 1: percorrer todas as páginas que estão em memória.
    for (int i = 0; i <= arr->top; i++) {
        // Passo 2: colocar o use bit a 0 ("não foi usada neste período").
        arr->ids[i].use_bit = 0;

        // Passo 3: manter coerente o bit "referenced" da entrada da tabela de
        //          páginas (PTE) correspondente, que representa a mesma
        //          informação.
        frame_desc_t *fd = &ft->frames[arr->ids[i].pfn];
        if (fd->pte) fd->pte->referenced = 0;
    }

    // Passo 4: registar a limpeza no log (só quando há páginas em memória,
    //          para não encher o log enquanto o simulador está parado).
    if (arr->top >= 0) {
        printf("[NRU] Periodic reset: period ended at t=%u ms (applied at t=%u ms)"
               " -> use bit = 0 for %d frames\n",
               period_end_ms, current_time_ms, arr->top + 1);
    }
}

/**
 * NRU - relógio periódico do SO (períodos de NRU_PERIOD_MS ms, virtmem.h).
 *
 * Os períodos são fixos, contados desde o arranque do simulador:
 * [0, 500[, [500, 1000[, [1000, 1500[, ... (com NRU_PERIOD_MS = 500).
 *
 * É chamada no início de page_eviction(), ou seja, imediatamente ANTES de cada
 * acesso a uma página (o ossim.c chama page_eviction() e depois
 * page_request() para cada página de um burst). Se desde a última limpeza
 * terminou pelo menos um período, os use bits são colocados a 0 nesse momento.
 *
 * O resultado é igual ao de um relógio que limpasse os bits exatamente nos
 * instantes 500, 1000, 1500, ...: os use bits só são lidos (na evicção) e
 * escritos (nos acessos) dentro de page_eviction()/page_request(), e a
 * limpeza é sempre aplicada antes do primeiro acesso de cada novo período.
 */
static void nru_tick(frame_table_t *ft, uint32_t current_time_ms) {
    // Início do período atual. É "static" para manter o valor entre chamadas
    // (começa em 0, o instante em que o simulador arranca).
    static uint32_t period_start_ms = 0;

    // Passo 1: a limpeza periódica só faz parte do algoritmo NRU.
    if (eviction_algo != NRU) return;

    // Passo 2: ainda não terminou o período atual -> nada a fazer.
    uint32_t elapsed_ms = current_time_ms - period_start_ms;
    if (elapsed_ms < NRU_PERIOD_MS) return;

    // Passo 3: terminaram um ou mais períodos completos. O novo período começa
    //          no último múltiplo de NRU_PERIOD_MS (não no instante atual),
    //          para que os períodos fiquem sempre alinhados: 500, 1000, 1500...
    //          Se passaram vários períodos sem acessos, basta uma limpeza.
    period_start_ms += (elapsed_ms / NRU_PERIOD_MS) * NRU_PERIOD_MS;

    // Passo 4: colocar todos os use bits a 0.
    nru_clear_use_bits(ft, period_start_ms, current_time_ms);
}

/**
 * NRU (Not Recently Used): "Quando o SO necessita de uma página, procura uma
 * que tenha o use bit = 0 (não foi usada neste período)" (slide 112).
 *
 * Regras quando o slide não especifica:
 *  - se várias páginas têm use bit = 0, escolhe-se a primeira encontrada
 *    percorrendo o array por ordem de carregamento (a carregada há mais tempo);
 *  - se TODAS as páginas têm use bit = 1, escolhe-se a página carregada há
 *    mais tempo (a primeira do array).
 */
int32_t nru_eviction(frame_table_t *ft) {
    array_t *arr = &ft->eviction_order;

    // Passo 1: se não houver páginas em memória, não há vítima possível.
    if (arr->top < 0) return INVALID_PFN;

    // Passo 2: mostrar o use bit de todas as páginas em memória.
    print_eviction_state(ft, "NRU");

    // Passo 3: procurar, por ordem de carregamento, a primeira página com
    //          use bit = 0 (não usada desde a última limpeza periódica).
    int victim_idx = -1;
    for (int i = 0; i <= arr->top; i++) {
        if (arr->ids[i].use_bit == 0) {
            victim_idx = i;
            break;
        }
    }

    if (victim_idx >= 0) {
        printf("[NRU] First page with use bit = 0 -> victim is frame %u (VPN %u)\n",
               arr->ids[victim_idx].pfn, ft->frames[arr->ids[victim_idx].pfn].vpn);
    } else {
        // Passo 4: todas as páginas foram usadas neste período (use bit = 1);
        //          escolher a carregada há mais tempo (posição 0).
        victim_idx = 0;
        printf("[NRU] All pages have use bit = 1 -> oldest loaded page, frame %u (VPN %u)\n",
               arr->ids[victim_idx].pfn, ft->frames[arr->ids[victim_idx].pfn].vpn);
    }

    // Passo 5: retirar a vítima do array e devolver a sua frame.
    return remove_at_index(arr, victim_idx);
}

/* ------------------------------------------------------------------ */
/* CLOCK                                                              */
/* ------------------------------------------------------------------ */

/**
 * CLOCK (algoritmo do relógio), slide 112:
 *  - "As páginas são colocadas numa lista circular (como se fossem um relógio)"
 *    -> o array eviction_order, percorrido de forma circular (depois da
 *       última posição, top, volta-se à posição 0). As páginas novas são
 *       acrescentadas no fim do array por update_eviction_array();
 *  - "O SO inicia a procura pela página apontada pelo ponteiro"
 *    -> o ponteiro é o campo last_index, que se mantém entre chamadas, por
 *       isso cada procura começa onde a anterior terminou;
 *  - "Use bit = 0: seleciona a página para ser substituída"
 *  - "Use bit = 1: coloca use bit = 0, avança o ponteiro para a próxima página
 *    e repete o processo até encontrar uma página com use bit = 0".
 *
 * Termina sempre: no pior caso (todas com use bit = 1) a primeira volta põe
 * todos os use bits a 0 e a vítima é encontrada na segunda volta.
 */
int32_t clock_eviction(frame_table_t *ft) {
    array_t *arr = &ft->eviction_order;

    // Passo 1: se não houver páginas em memória, não há vítima possível.
    if (arr->top < 0) return INVALID_PFN;

    // Passo 2: garantir que o ponteiro está dentro do array (a lista pode ter
    //          encolhido desde a última procura).
    if (arr->last_index > arr->top) arr->last_index = 0;

    // Passo 3: mostrar o estado do relógio ('>' marca o ponteiro).
    print_eviction_state(ft, "CLOCK");

    // Passo 4: rodar o ponteiro até encontrar uma página com use bit = 0.
    while (1) {
        array_elem_t *e = &arr->ids[arr->last_index];

        if (e->use_bit == 0) {
            // Passo 4a: use bit = 0 -> esta página é a vítima.
            printf("[CLOCK] Hand at [%d] frame %u (VPN %u) use=0 -> victim\n",
                   arr->last_index, e->pfn, ft->frames[e->pfn].vpn);

            // Passo 5: retirar a vítima. remove_at_index() deixa o ponteiro a
            //          apontar para a página seguinte à vítima, onde começará
            //          a próxima procura.
            return remove_at_index(arr, arr->last_index);
        }

        // Passo 4b: use bit = 1 -> dar uma "segunda oportunidade": colocar o
        //           use bit a 0 e avançar o ponteiro para a próxima página
        //           (voltando ao início depois da última - lista circular).
        printf("[CLOCK] Hand at [%d] frame %u (VPN %u) use=1 -> set use=0, advance\n",
               arr->last_index, e->pfn, ft->frames[e->pfn].vpn);
        e->use_bit = 0;
        arr->last_index = (arr->last_index + 1) % (arr->top + 1);
    }
}

/* ------------------------------------------------------------------ */
/* Escolha do algoritmo                                               */
/* ------------------------------------------------------------------ */

/**
 * Escolhe a frame vítima com o algoritmo definido por --vm e devolve o seu pfn
 * (ou INVALID_PFN se não houver nenhuma). A vítima já sai da estrutura de
 * ordem de evicção; page_eviction() trata do swap-out e de a pôr na lista de
 * frames livres.
 */
int evict(frame_table_t *ft) {
    switch (eviction_algo) {
        case FIFO:
            return fifo_eviction(&ft->eviction_order);
        case RANDOM:
            return random_eviction(ft);
        case LRU:
            return lru_eviction(ft);
        case NRU:
            return nru_eviction(ft);
        case CLOCK:
            return clock_eviction(ft);
        default:
            printf("Eviction algorithm '%s' not implemented yet\n", get_eviction_algo_str());
            return INVALID_PFN;
    }
}
