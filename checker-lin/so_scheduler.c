// SPDX-License-Identifier: GPL-2.0
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
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
	sem_t exec_sem;
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
	pthread_mutex_t mutex;
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

	for (int i = SO_MAX_PRIO; i >= 0; --i) {
		if (!is_empty(sched->ready_queues[i])) {
			sched->running_th = remove_first(&sched->ready_queues[i]);
			/* dam liber rularii thread-ului ales */
			rc = sem_post(&sched->running_th->exec_sem);
			DIE(rc == FAIL, "Failed to release thread's semaphore!");
			break;
		}
	}
}

/* Se consulta planificatorul in vederea rularii urmatoarei instructiuni. */
void check_schedule(void)
{
	thread *curr = sched->running_th;
	int rc = 0;

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
	 * in caz ca exista un proces READY cu o prioritate mai mare
	 * sau exista unul de aceeasi prioritate care urmeaza conform ROUND ROBIN.
	 */
	for (int i = SO_MAX_PRIO; i >= min_prio; --i) {
		if (!is_empty(sched->ready_queues[i])) {
			sched->running_th = remove_first(&sched->ready_queues[i]);
			insert_last(curr, &sched->ready_queues[curr->priority]);
			/* dam liber rularii thread-ului ales */
			rc = sem_post(&sched->running_th->exec_sem);
			DIE(rc == FAIL, "Failed to release thread's semaphore!");
			break;
		}
	}

}

/* Functie rulata de fiecare thread,
 * determina contextul în care se va executa handler-ul thread-ului
 * si la terminare planifica urmatorul thread.
 */
void *start_thread(void *arg)
{
	thread *th = (thread *) arg;
	int rc = 0;

	/* thread-ul asteapta momentul in care poate rula handler-ul */
	rc = sem_wait(&th->exec_sem);
	DIE(rc == FAIL, "Failed to acquire thread's semaphore!");

	/* apelam functia handler cu prioritatea thread-ului ca argument */
	th->func(th->priority);

	/* thread-ul asteapta sa obtina acces la planificator */
	rc = pthread_mutex_lock(&sched->mutex);
	DIE(rc != 0, "Failed to lock scheduler's mutex!");

	/* in acest moment, thread-ul si-a terminat treaba
	 * astfel, daca inca este in RUNNING acest thread, se planifica altul
	 */
	if (sched->running_th == th)
		schedule_other_thread();

	/* lasam alte thread-uri sa aiba acces la planificator */
	rc = pthread_mutex_unlock(&sched->mutex);
	DIE(rc != 0, "Failed to unlock scheduler's mutex!");

	/* iesire thread */
	pthread_exit(NULL);
}

/* Inițializează planificatorul.
 * Întoarce 0 dacă planificatorul a fost inițializat cu succes,
 * sau negativ în caz de eroare.
 */
int so_init(unsigned int time_quantum, unsigned int io)
{
	int rc = 0;

	/* cuanta de timp trebuie sa fie strict pozitiva si
	 * numarul de dispozitive I/O suportat nu trebuie sa depaseasca maximul
	 */
	if (time_quantum == 0 || io > SO_MAX_NUM_EVENTS)
		return FAIL;

	/* odata initialiazat, planificatorul nu poate fi initializat a doua oara */
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
	 * astfel, un thread care detine mutex-ul deja nu se va bloca la un nou lock
	 */
	pthread_mutexattr_t mutex_attr;

	pthread_mutexattr_init(&mutex_attr);
	pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE);
	rc = pthread_mutex_init(&sched->mutex, &mutex_attr);
	DIE(rc != 0, "Failed to initialize semaphore!");

	return 0;
}

/* Pornește și introduce în planificator un nou thread.
 * Funcția se va întoarce abia după ce noul thread creat
 * fie a fost planificat, fie a intrat în starea READY.
 * Returnează id-ul noului thread sau INVALID_TID in caz de eroare.
 */
tid_t so_fork(so_handler *func, unsigned int priority)
{
	int rc = 0;
	thread *th = NULL;
	thread *curr = NULL;

	/* adresa functiei nu trebuie sa fie NULL si
	 * prioritatea thread-ului nu trebuie sa o depaseasca pe cea maxima
	 */
	if (!func || priority > SO_MAX_PRIO)
		return INVALID_TID;

	/* thread-ul asteapta sa obtina acces la planificator */
	rc = pthread_mutex_lock(&sched->mutex);
	DIE(rc != 0, "Failed to lock scheduler's mutex!");

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
	rc = sem_init(&th->exec_sem, 0, 0);
	DIE(rc == FAIL, "Failed to initialize semaphore!");

	/* thread-ul nou va executa functia start_thread */
	rc = pthread_create(&th->id, NULL, start_thread, th);
	DIE(rc != 0, "thread create");

	/* threadul va intra în starea READY/RUN */
	if (!curr) {
		/* nu ruleaza nimic, deci thread-ul este pus direct in RUNNING */
		sched->running_th = th;

		/* dam liber rularii thread-ului */
		rc = sem_post(&th->exec_sem);
		DIE(rc == FAIL, "Failed to release thread's semaphore!");
	} else {
		/* intai punem thread-ul in coada READY dupa prioritatea sa */
		insert_last(th, &sched->ready_queues[th->priority]);

		/* obtinem planificarea pentru urmatoarea instructiune de executat */
		check_schedule();
	}

	/* lasam alte thread-uri sa aiba acces la planificator */
	rc = pthread_mutex_unlock(&sched->mutex);
	DIE(rc != 0, "Failed to unlock scheduler's mutex!");

	/* daca thread-ul curent a fost preemptat, ii blocam rularea */
	if (curr != NULL && curr != sched->running_th) {
		rc = sem_wait(&curr->exec_sem);
		DIE(rc == FAIL, "Failed to acquire own semaphore!");
	}

	/* returnează id-ul noului thread */
	return th->id;
}

/* Threadul curent se blochează în așteptarea unui eveniment sau a unei operații
 * de I/O. Întoarce 0 dacă evenimentul există (id-ul acestuia este valid)
 * sau negativ în caz de eroare.
 */
int so_wait(unsigned int io)
{
	int rc = 0;
	/* thread-ul asteapta sa obtina acces la planificator */
	rc = pthread_mutex_lock(&sched->mutex);
	DIE(rc != 0, "Failed to lock scheduler's mutex!");

	int ret = 0;
	thread *curr = sched->running_th;

	/* decrementam cuanta de timp a thread-ului */
	curr->time_left--;

	/* evenimentul/operatia I/O trebuie sa aiba id-ul mai mare sau egal cu 0
	 * si mai mic decat numarul de dispozitive I/O suportat
	 */
	if (io < 0 || io >= sched->io) {
		/* obtinem planificarea pentru urmatoarea instructiune de executat */
		check_schedule();
		ret = FAIL;
	} else {
		curr->time_left = sched->time_quantum;
		/* Thread-ul este preemptat, inlocuit cu unul din coada READY.
		 * Este adaugat in coada WAITING corespunzatoare dispozitivului io.
		 */
		schedule_other_thread();
		insert_last(curr, &sched->waiting_queues[io]);
	}

	/* lasam alte thread-uri sa aiba acces la planificator */
	rc = pthread_mutex_unlock(&sched->mutex);
	DIE(rc != 0, "Failed to unlock scheduler's mutex!");

	if (ret == 0 || (ret == FAIL && curr != sched->running_th)) {
		/* blocam rularea thread-ului care asteapta la I/O */
		rc = sem_wait(&curr->exec_sem);
		DIE(rc == FAIL, "Failed to acquire own semaphore!");
	}

	return ret;
}

/* Trezește unul sau mai multe thread-uri care așteaptă un anumit eveniment.
 * Întoarce numărul total de thread-uri deblocate, sau negativ
 * în caz de eroare (evenimentul nu este valid).
 */
int so_signal(unsigned int io)
{
	int rc = 0;
	/* thread-ul asteapta sa obtina acces la planificator */
	rc = pthread_mutex_lock(&sched->mutex);
	DIE(rc != 0, "Failed to lock scheduler's mutex!");

	int ret = 0;
	thread *curr = sched->running_th;

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
	rc = pthread_mutex_unlock(&sched->mutex);
	DIE(rc != 0, "Failed to unlock scheduler's mutex!");

	/* daca thread-ul curent a fost preemptat, ii blocam rularea */
	if (curr != sched->running_th) {
		rc = sem_wait(&curr->exec_sem);
		DIE(rc == FAIL, "Failed to acquire own semaphore!");
	}

	return ret;
}

/* Simulează execuția unei instrucțiuni generice.
 * Planifica urmatoarea instructiune si blocheaza thread-ul curent
 * in caz de preemptie.
 */
void so_exec(void)
{
	int rc = 0;
	/* thread-ul asteapta sa obtina acces la planificator */
	rc = pthread_mutex_lock(&sched->mutex);
	DIE(rc != 0, "Failed to lock scheduler's mutex!");

	/* thread-ul curent este cel care se executa */
	thread *curr = sched->running_th;

	/* decrementam cuanta de timp odata cu executarea instructiunii */
	curr->time_left--;

	/* obtinem planificarea pentru urmatoarea instructiune de executat */
	check_schedule();

	/* lasam alte thread-uri sa aiba acces la planificator */
	rc = pthread_mutex_unlock(&sched->mutex);
	DIE(rc != 0, "Failed to unlock scheduler's mutex!");

	/* daca thread-ul curent a fost preemptat, ii blocam rularea */
	if (curr != sched->running_th) {
		rc = sem_wait(&curr->exec_sem);
		DIE(rc == FAIL, "Failed to acquire own semaphore!");
	}
}

/* Eliberează resursele planificatorului și așteaptă
 * terminarea tuturor threadurilor înainte de părăsirea sistemului.
 */
void so_end(void)
{
	int rc = 0;
	/* nu avem ce elibera, daca planificatorul nu a fost initializat */
	if (sched == NULL)
		return;

	/* asteptam ca toate thread-urile sa se termine */
	if (!is_empty(sched->all_threads)) {
		/* tinem minte primul element din lista
		 * pentru a stii cand am parcurs toate elementele listei
		 */
		thread *first = remove_first(&sched->all_threads);
		/* asteptam thread-ul */
		rc = pthread_join(first->id, NULL);
		DIE(rc != 0, "Failed join!");
		/* introducem thread-ul inapoi in lista */
		insert_last(first, &sched->all_threads);

		/* parcurgem restul elementelor din lista*/
		thread *curr = NULL;

		while (1) {
			curr = remove_first(&sched->all_threads);
			insert_last(curr, &sched->all_threads);
			if (curr == first)
				break;
			/* asteptam thread-ul */
			rc = pthread_join(curr->id, NULL);
			DIE(rc != 0, "Failed join!");
		}
	}

	/* odata ce s-au terminat, putem elibera memoria alocata lor */
	while (!is_empty(sched->all_threads)) {
		thread *first = remove_first(&sched->all_threads);

		/* eliberam resursele folosite de acest thread */
		rc = sem_destroy(&first->exec_sem);
		DIE(rc == FAIL, "Failed to destroy thread's semaphore!");
		free(first);
	}

	/* eliberam resursele planificatorului si il setam la NULL */
	rc = pthread_mutex_destroy(&sched->mutex);
	DIE(rc != 0, "Failed to destroy scheduler's semaphore!");
	free(sched);
	sched = NULL;
}
