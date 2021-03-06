// SPDX-License-Identifier: GPL-2.0
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <errno.h>

#include "./_test/so_scheduler.h"
#include "glist.h"

#define FAIL -1

#define DIE(assertion, call_description)                       \
	do {                                                       \
		if (assertion) {                                       \
			fprintf(stderr, "(%s, %d): ", __FILE__, __LINE__); \
			perror(call_description);                          \
			exit(errno);                                       \
		}                                                      \
	} while (0)

/* contine toate informatiile specifice unui thread din sistemul simulat */
typedef struct {
	/* semaforul blocheaza sau nu executia thread-ului */
	HANDLE exec_sem;
	/* id-ul thread-ului */
	tid_t id;
	/* functia handler care va fi executata de thread*/
	so_handler *func;
	/* prioritatea statica a thread-ului */
	int priority;
	/* timpul ramas din cuanta de timp a thread-ului */
	int time_left;
} thread;

/* contine toate informatiile specifice planificatorului din sistemul simulat */
typedef struct {
	/* mutex-ul este folosit pentru sincronizarea thread-urilor
	 * atunci cand ele efectueaza operatii cu membrii acestei structurii
	 */
	CRITICAL_SECTION mutex;
	/* cuanta de timp a sistemului */
	unsigned int time_quantum;
	/* numarul de dispozitive I/O */
	unsigned int io;
	/* structura thread-ului care e in RUNNING */
	thread *running_th;
	/* cozile thread-urilor aflate in WAITING
	 * cate una pentru fiecare dispozitiv
	 */
	list waiting_queues[SO_MAX_NUM_EVENTS];
	/* cozile thread-urilor aflate in READY
	 * cate una pentru fiecare prioritate
	 */
	list ready_queues[SO_MAX_PRIO + 1];
	/* o lista cu toate thread-urile sistemului simulat */
	list all_threads;
} scheduler;

/* variabila in care se tine minte planificatorul */
scheduler *sched;

/* Se planifica un alt thread (dupa ce cel curent si-a terminat treaba). */
void schedule_other_thread(void)
{
	int rc = 0;
	BOOL r;
	int i;

	for (i = SO_MAX_PRIO; i >= 0; --i) {
		if (!is_empty(sched->ready_queues[i])) {
			sched->running_th =
				remove_first(&sched->ready_queues[i]);
			/* dam liber rularii thread-ului ales */
			r = ReleaseSemaphore(sched->running_th->exec_sem, 1, 0);
			DIE(r == 0, "Failed to release thread's semaphore!");
			break;
		}
	}
}

/* Se consulta planificatorul in vederea rularii urmatoarei instructiuni. */
void check_schedule(void)
{
	thread *curr = sched->running_th;
	int rc = 0;
	BOOL r;
	int i;

	/* Calculam index-ul ultimei cozi pe care trebuie s-o verificam.
	 * Daca nu i-a expirat cuanta thread-ului curent,
	 * doar un thread cu o prioritate mai mare il poate preempta.
	 */
	int min_prio = curr->priority + 1;

	if (curr->time_left == 0) {
		/* Daca i-a expirat cuanta,
		 * si unul de aceeasi prioritate il poate preempta.
		 */
		min_prio = curr->priority;
		/* resetam cuanta de timp pentru acest thread */
		curr->time_left = sched->time_quantum;
	}

	/* Este preemptat thread-ul care ruleaza
	 * in caz ca exista un proces READY cu o prioritate mai mare sau
	 * exista unul de aceeasi prioritate care urmeaza conform ROUND ROBIN.
	 */
	for (i = SO_MAX_PRIO; i >= min_prio; --i) {
		if (!is_empty(sched->ready_queues[i])) {
			sched->running_th =
				remove_first(&sched->ready_queues[i]);
			insert_last(curr, &sched->ready_queues[curr->priority]);
			/* dam liber rularii thread-ului ales */
			r = ReleaseSemaphore(sched->running_th->exec_sem, 1, 0);
			DIE(r == 0, "Failed to release thread's semaphore!");
			break;
		}
	}

}

/* Functie rulata de fiecare thread,
 * determina contextul ??n care se va executa handler-ul thread-ului
 * si la terminare planifica urmatorul thread.
 */
DWORD start_thread(void *arg)
{
	thread *th = (thread *) arg;
	int rc = 0;

	/* thread-ul asteapta momentul in care poate rula handler-ul */
	WaitForSingleObject(th->exec_sem, INFINITE);

	/* apelam functia handler cu prioritatea thread-ului ca argument */
	th->func(th->priority);

	/* thread-ul asteapta sa obtina acces la planificator */
	EnterCriticalSection(&sched->mutex);

	/* in acest moment, thread-ul si-a terminat treaba
	 * astfel, daca inca este in RUNNING acest thread, se planifica altul
	 */
	if (sched->running_th == th)
		schedule_other_thread();

	/* lasam alte thread-uri sa aiba acces la planificator */
	LeaveCriticalSection(&sched->mutex);

	/* iesire thread */
	return 0;
}

/* Ini??ializeaz?? planificatorul.
 * ??ntoarce 0 dac?? planificatorul a fost ini??ializat cu succes,
 * sau negativ ??n caz de eroare.
 */
int so_init(unsigned int time_quantum, unsigned int io)
{
	int rc = 0;

	/* cuanta de timp trebuie sa fie strict pozitiva si
	 * numarul de dispozitive I/O suportat nu trebuie sa depaseasca maximul
	 */
	if (time_quantum == 0 || io > SO_MAX_NUM_EVENTS)
		return FAIL;

	/* odata initialiazat, planificatorul nu poate fi initializat din nou */
	if (sched) {
		free(sched);
		sched = NULL;
		return FAIL;
	}

	/* alocam memoria structurii planificatorului */
	sched = calloc(1, sizeof(scheduler));
	DIE(sched == NULL, "Failed to allocate memory!");

	/* completam structura */
	sched->time_quantum = time_quantum;
	sched->io = io;
	/* initializam mutex-ul planificatorului care va fi de tip recursiv
	 * astfel, un thread care detine mutex-ul deja
	 * nu se va bloca la un nou lock
	 */
	InitializeCriticalSection(&sched->mutex);

	return 0;
}

/* Porne??te ??i introduce ??n planificator un nou thread.
 * Func??ia se va ??ntoarce abia dup?? ce noul thread creat
 * fie a fost planificat, fie a intrat ??n starea READY.
 * Returneaz?? id-ul noului thread sau INVALID_TID in caz de eroare.
 */
tid_t so_fork(so_handler *func, unsigned int priority)
{
	int rc = 0;
	thread *th = NULL;
	thread *curr = NULL;
	DWORD id;
	BOOL r;

	/* adresa functiei nu trebuie sa fie NULL si
	 * prioritatea thread-ului nu trebuie sa o depaseasca pe cea maxima
	 */
	if (!func || priority > SO_MAX_PRIO)
		return INVALID_TID;

	/* thread-ul asteapta sa obtina acces la planificator */
	EnterCriticalSection(&sched->mutex);

	curr = sched->running_th;
	if (curr) {
		/* decrementam cuanta de timp odata cu executarea instructiunii
		 * din contextul unui thread din sistemul simulat
		 */
		curr->time_left--;
	}

	/* alocam memoria pentru structura thread-ului */
	th = calloc(1, sizeof(thread));
	DIE(th == NULL, "Failed to allocate memory!");

	/* adaugam thread-ul in lista tuturor thread-urilor */
	insert_last(th, &sched->all_threads);

	/* initializam structura thread-ului */
	th->func = func;
	th->priority = priority;
	th->time_left = sched->time_quantum;

	/* implicit thread-ul nu are voie sa execute instructiuni
	 * pana nu decide planificatorul
	 * prin urmare, semaforul are initial valoarea 0
	 */
	th->exec_sem = CreateSemaphore(0, 0, 1, 0);
	DIE(th->exec_sem == NULL, "Failed to initialize semaphore!");

	/* thread-ul nou va executa functia start_thread */
	th->id = (tid_t)CreateThread(NULL, 0,
			(LPTHREAD_START_ROUTINE) start_thread,
			th, 0, &id);

	/* threadul va intra ??n starea READY/RUN */
	if (!curr) {
		/* nu ruleaza nimic, deci threadul este pus direct in RUNNING */
		sched->running_th = th;

		/* dam liber rularii thread-ului */
		r = ReleaseSemaphore(th->exec_sem, 1, 0);
		DIE(r == 0, "Failed to release thread's semaphore!");
	} else {
		/* intai punem thread-ul in coada READY dupa prioritatea sa */
		insert_last(th, &sched->ready_queues[th->priority]);

		/* obtinem planificarea pentru urmatoarea instructiune
		 * de executat
		 */
		check_schedule();
	}

	/* lasam alte thread-uri sa aiba acces la planificator */
	LeaveCriticalSection(&sched->mutex);

	/* daca thread-ul curent a fost preemptat, ii blocam rularea */
	if (curr != NULL && curr != sched->running_th)
		WaitForSingleObject(curr->exec_sem, INFINITE);

	/* returneaz?? id-ul noului thread */
	return id;
}

/* Threadul curent se blocheaz?? ??n a??teptarea unui eveniment sau a unei opera??ii
 * de I/O. ??ntoarce 0 dac?? evenimentul exist?? (id-ul acestuia este valid)
 * sau negativ ??n caz de eroare.
 */
int so_wait(unsigned int io)
{
	int rc = 0;
	int ret = 0;
	thread *curr = NULL;

	/* thread-ul asteapta sa obtina acces la planificator */
	EnterCriticalSection(&sched->mutex);

	curr = sched->running_th;

	/* decrementam cuanta de timp a thread-ului */
	curr->time_left--;

	/* evenimentul/operatia I/O trebuie sa aiba id-ul mai mare sau egal cu 0
	 * si mai mic decat numarul de dispozitive I/O suportat
	 */
	if (io < 0 || io >= sched->io) {
		/* obtinem planificarea pentru urmatoarea instructiune
		 * de executat
		 */
		check_schedule();
		ret = FAIL;
	} else {
		curr->time_left = sched->time_quantum;
		/* Thread-ul este preemptat, inlocuit cu unul din coada READY.
		 * Este adaugat in coada WAITING corespunzatoare
		 * dispozitivului io.
		 */
		schedule_other_thread();
		insert_last(curr, &sched->waiting_queues[io]);
	}

	/* lasam alte thread-uri sa aiba acces la planificator */
	LeaveCriticalSection(&sched->mutex);

	if (ret == 0 || (ret == FAIL && curr != sched->running_th)) {
		/* blocam rularea thread-ului care asteapta la I/O */
		WaitForSingleObject(curr->exec_sem, INFINITE);
	}

	return ret;
}

/* Treze??te unul sau mai multe thread-uri care a??teapt?? un anumit eveniment.
 * ??ntoarce num??rul total de thread-uri deblocate, sau negativ
 * ??n caz de eroare (evenimentul nu este valid).
 */
int so_signal(unsigned int io)
{
	int rc = 0;
	int ret = 0;
	thread *curr = NULL;

	/* thread-ul asteapta sa obtina acces la planificator */
	EnterCriticalSection(&sched->mutex);

	curr = sched->running_th;

	/* decrementam cuanta de timp a thread-ului */
	curr->time_left--;

	/* evenimentul/operatia I/O are id-ul mai mare sau egal cu 0
	 * si mai mic decat numarul de dispozitive I/O suportat
	 */
	if (io < 0 || io >= sched->io) {
		ret = FAIL;
	} else {
		/* Sunt scoase thread-urile din coada WAITING corespunzatoare
		 * dispozitivului io si sunt adaugate in READY.
		 */
		while (!is_empty(sched->waiting_queues[io])) {
			thread *th = remove_first(&sched->waiting_queues[io]);

			insert_last(th, &sched->ready_queues[th->priority]);
			/* incrementam numarul total de thread-uri deblocate */
			ret++;
		}
	}

	/* obtinem planificarea pentru urmatoarea instructiune de executat */
	check_schedule();

	/* lasam alte thread-uri sa aiba acces la planificator */
	LeaveCriticalSection(&sched->mutex);

	/* daca thread-ul curent a fost preemptat, ii blocam rularea */
	if (curr != sched->running_th)
		WaitForSingleObject(curr->exec_sem, INFINITE);

	return ret;
}

/* Simuleaz?? execu??ia unei instruc??iuni generice.
 * Planifica urmatoarea instructiune si blocheaza thread-ul curent
 * in caz de preemptie.
 */
void so_exec(void)
{
	int rc = 0;
	thread *curr = NULL;

	/* thread-ul asteapta sa obtina acces la planificator */
	EnterCriticalSection(&sched->mutex);

	/* thread-ul curent este cel care se executa */
	curr = sched->running_th;

	/* decrementam cuanta de timp odata cu executarea instructiunii */
	curr->time_left--;

	/* obtinem planificarea pentru urmatoarea instructiune de executat */
	check_schedule();

	/* lasam alte thread-uri sa aiba acces la planificator */
	LeaveCriticalSection(&sched->mutex);

	/* daca thread-ul curent a fost preemptat, ii blocam rularea */
	if (curr && curr != sched->running_th)
		WaitForSingleObject(curr->exec_sem, INFINITE);
}

/* Elibereaz?? resursele planificatorului ??i a??teapt??
 * terminarea tuturor threadurilor ??nainte de p??r??sirea sistemului.
 */
void so_end(void)
{
	int rc = 0;
	BOOL r;
	/* nu avem ce elibera, daca planificatorul nu a fost initializat */
	if (sched == NULL)
		return;

	/* asteptam ca toate thread-urile sa se termine */
	if (!is_empty(sched->all_threads)) {
		thread *curr = NULL;
		/* tinem minte primul element din lista
		 * pentru a stii cand am parcurs toate elementele listei
		 */
		thread *first = remove_first(&sched->all_threads);
		/* asteptam thread-ul */
		DWORD dwReturn = WaitForSingleObject((HANDLE)first->id,
					INFINITE);
		DIE(dwReturn == WAIT_FAILED, "Failed join!");
		/* introducem thread-ul inapoi in lista */
		insert_last(first, &sched->all_threads);

		/* parcurgem restul elementelor din lista*/
		while (1) {
			DWORD dwReturn;
			curr = remove_first(&sched->all_threads);
			insert_last(curr, &sched->all_threads);
			if (curr == first)
				break;

			/* asteptam thread-ul */
			dwReturn = WaitForSingleObject((HANDLE)curr->id,
					INFINITE);
			DIE(dwReturn == WAIT_FAILED, "Failed join!");
		}
	}

	/* odata ce s-au terminat, putem elibera memoria alocata lor */
	while (!is_empty(sched->all_threads)) {
		thread *first = remove_first(&sched->all_threads);

		/* eliberam resursele folosite de acest thread */
		r = CloseHandle(first->exec_sem);
		DIE(r == 0, "Failed to destroy thread's semaphore!");
		free(first);
	}

	/* eliberam resursele planificatorului si il setam la NULL */
	DeleteCriticalSection(&sched->mutex);
	free(sched);
	sched = NULL;
}
