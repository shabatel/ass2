#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "spinlock.h"
#include "proc.h"
#include "kthread.h"

// static struct kthread_mutex_t mutex_arr[MAX_MUTEXES];   // Global static array to hold the mutex objects

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

struct {
  struct spinlock lock;
	struct kthread_mutex_t mutex_arr[MAX_MUTEXES];
} mtable;

int nextpid = 1;
int nexttid = 1;
int nextmid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void lockPtable(void){
  acquire(&ptable.lock);
}

void releasePtable(void){
  release(&ptable.lock);
}

int countRunnableThreads(struct proc *p)
{
  struct thread *t;
  int counter = 0;
  //Loop over threads table looking for threads
  for (t = p->pthreads; t < &p->pthreads[NTHREAD]; t++) {
		if (t->state != T_UNUSED && t->state != T_ZOMBIE)
			counter++;
		else {
			t->killed = 1;
  	}
	}
  return counter;
}

void finishAllThreads(){
  struct proc *curproc = myproc();
  
      if(!holding(&ptable.lock)) {
      acquire(&ptable.lock);
    }

  while( countRunnableThreads(curproc) > 1){
	wakeup1(curproc);
	sleep(curproc, &ptable.lock);
  }
  release(&ptable.lock);
}

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
	panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
	if (cpus[i].apicid == apicid)
	  return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

struct thread*
mythread(void) {
  struct cpu *c;
  struct thread *t;
  pushcli();
  c = mycpu();
  t = c->thread;
  popcli();
  return t;
}

struct thread* searchThreadByStatus(struct proc *p, enum threadstate state) {
	struct thread *t;
	for(t = p->pthreads; t < &p->pthreads[NTHREAD]; t++)
		if (t->state == state)
			return t;
	return 0;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void) {
	struct proc *p;
	char *sp;
	struct thread *t;

	if(!holding(&ptable.lock)) {
      acquire(&ptable.lock);
    }

	for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
		if (p->state == UNUSED)
			goto found;

	release(&ptable.lock);
	return 0;

	found:
	p->state = EMBRYO;
	p->pid = nextpid++;

	release(&ptable.lock);

	//init thread state of all thread to UNUSED
	for (t = p->pthreads; t < &p->pthreads[NTHREAD]; t++) {
		t->state = T_UNUSED;
	}

	t = searchThreadByStatus(p, T_UNUSED);
	t->state = T_EMBRYO;
	t->tid = nexttid++;
	t->proc = p;
	t->killed = 0;

	// Allocate kernel stack.
	if ((t->kstack = kalloc()) == 0) {
		t->state = T_UNUSED;
    p->state=UNUSED; // todo CHANGED
		return 0;
	}
	sp = t->kstack + KSTACKSIZE;

	// Leave room for trap frame.
	sp -= sizeof *t->tf;
	t->tf = (struct trapframe *) sp;

	// Set up new context to start executing at forkret,
	// which returns to trapret.
	sp -= 4;
	*(uint *) sp = (uint) trapret;

	sp -= sizeof *t->context;
	t->context = (struct context *) sp;
	memset(t->context, 0, sizeof *t->context);
	t->context->eip = (uint) forkret;

	return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void) {
	struct proc *p;
	extern char _binary_initcode_start[], _binary_initcode_size[];

	p = allocproc();


	initproc = p;
	if ((p->pgdir = setupkvm()) == 0)
		panic("userinit: out of memory?");
	inituvm(p->pgdir, _binary_initcode_start, (int) _binary_initcode_size);
	p->sz = PGSIZE;
	safestrcpy(p->name, "initcode", sizeof(p->name));
	p->cwd = namei("/");

	struct thread *t = searchThreadByStatus(p, T_EMBRYO); // the process wasn't in use, all threads are free to work
	memset(t->tf, 0, sizeof(*t->tf));
	t->tf->cs = (SEG_UCODE << 3) | DPL_USER;
	t->tf->ds = (SEG_UDATA << 3) | DPL_USER;
	t->tf->es = t->tf->ds;
	t->tf->ss = t->tf->ds;
	t->tf->eflags = FL_IF;
	t->tf->esp = PGSIZE;
	t->tf->eip = 0;  // beginning of initcode.S

	// this assignment to p->state lets other cores
	// run this process. the acquire forces the above
	// writes to be visible, and the lock is also needed
	// because the assignment might not be atomic.
	    if(!holding(&ptable.lock)) {
      acquire(&ptable.lock);
    }

	p->state = INUSED;
	t->state = RUNNABLE;

	release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n) { // todo need to be synced
	uint sz;
	struct proc *curproc = myproc();


	sz = curproc->sz;
	if (n > 0) {
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
			return -1;
	} else if (n < 0) {
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
			return -1;
  }
	curproc->sz = sz;
	switchuvm(curproc, mythread());
	return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void) {
	int i, pid;
	struct proc *np;
	struct proc *curproc = myproc();
	struct thread *currthread = mythread();

	// Allocate process.
	if ((np = allocproc()) == 0) {
		return -1;
	}

	struct thread *nt = searchThreadByStatus(np, T_EMBRYO);

	// Copy process state from proc.
	if ((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0) {
		kfree(nt->kstack);
		nt->kstack = 0;
		np->state = UNUSED;
		nt->state = T_UNUSED;
		return -1;
	}
	np->sz = curproc->sz;
	np->parent = curproc;
	*nt->tf = *currthread->tf;

	// Clear %eax so that fork returns 0 in the child.
	nt->tf->eax = 0;

	for (i = 0; i < NOFILE; i++)
		if (curproc->ofile[i])
			np->ofile[i] = filedup(curproc->ofile[i]);
	np->cwd = idup(curproc->cwd);

	safestrcpy(np->name, curproc->name, sizeof(curproc->name));

	pid = np->pid;
	    if(!holding(&ptable.lock)) {
      acquire(&ptable.lock);
    }

	nt->state = RUNNABLE;
	np->state = INUSED;

	release(&ptable.lock);

	return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void) {  struct proc *curproc = myproc();
  struct thread *curthread = mythread();
  struct proc *p;
  int fd;
  struct thread *t;


  if(curproc == initproc)
    panic("init exiting");

  if(curthread->killed)
    kthread_exit();

  kill(curproc->pid);

  acquire(&ptable.lock);

  int allZombies = 1;
  for (int i =0; i<NTHREAD; i++){
    t = &curproc->pthreads[i];
    if(t->tid !=mythread()->tid &&( t->state != T_ZOMBIE && t->state != T_UNUSED))
      allZombies = 0;
  }

  if(allZombies)//this is the last thread to exit!
  {
    release(&ptable.lock);

    // Close all open files.
    for(fd = 0; fd < NOFILE; fd++){
      if(curproc->ofile[fd]){
        fileclose(curproc->ofile[fd]);
        curproc->ofile[fd] = 0;
      }
    }

    begin_op();
    iput(curproc->cwd);
    end_op();
    curproc->cwd = 0;
    // Parent might be sleeping in wait().
    acquire(&ptable.lock);

    wakeup1(curproc->parent);
    // Pass abandoned children to init.
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent == curproc){
        p->parent = initproc;
        if(p->state == ZOMBIE)
          wakeup1(initproc);
      }
    }
  }


  curthread->state = T_ZOMBIE;
  curproc->state = ZOMBIE;
  sched();
  panic("zombie (exit)");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  struct thread *t;
  int havekids, pid;
  struct proc *curproc = myproc();
  
      if(!holding(&ptable.lock)) {
      acquire(&ptable.lock);
    }
  for(;;){
	// Scan through table looking for exited children.
	havekids = 0;
	for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
	  if(p->parent != curproc)
		continue;
	  havekids = 1;
	  if(p->state == ZOMBIE){
		pid = p->pid;
		freevm(p->pgdir);
		p->pid = 0;
		p->parent = 0;
		p->name[0] = 0;
		p->killed = 0;
		p->state = UNUSED;
          // Found one.
          for(t = p->pthreads; t < &p->pthreads[NTHREAD] ; t++){
              if(t->state == T_ZOMBIE){
                  t->tid = 0;
                  kfree(t->kstack);
                  t->kstack=0;
              }
          }
		release(&ptable.lock);
		return pid;
	  }
	}

	// No point waiting if we don't have any children.
	if(!havekids || curproc->killed){
	  release(&ptable.lock);
	  return -1;
	}

	// Wait for children to exit.  (See wakeup1 call in proc_exit.)
	sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void) {
	struct proc *p;
	struct cpu *c = mycpu();
	struct thread *t;
	c->proc = 0;
	c->thread = 0;

	for (;;) {
		// Enable interrupts on this processor.
		sti();

		// Loop over process table looking for process to run.
		    if(!holding(&ptable.lock)) {
      acquire(&ptable.lock);
    }
		for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
			if (p->state != INUSED) {
				continue;
			}

			// Loop over process threads looking for thread to run
			if(!(t = searchThreadByStatus(p, RUNNABLE)))
				continue; // Thread not found, move to the next process

			// Switch to chosen process.  It is the process's job
			// to release ptable.lock and then reacquire it
			// before jumping back to us.
			c->proc = p;
			c->thread = t;
			switchuvm(p, t);
			t->state = RUNNING;

			swtch(&(c->scheduler), t->context);
			switchkvm();

			// Process is done running for now.
			// It should have changed its p->state before coming back.
			c->proc = 0;
			c->thread = 0;
		}
		release(&ptable.lock);

	}
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct thread *t = mythread();

  if(!holding(&ptable.lock))
	panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
	panic("sched locks");
  if(t->state == RUNNING)
	panic("sched running");
  if(readeflags()&FL_IF)
	panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&t->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
//	if(mythread()->killed){
//		kthread_exit();
//	}
      if(!holding(&ptable.lock)) {
      acquire(&ptable.lock);
    }  //DOC: yieldlock
    myproc()->state = INUSED;
  mythread()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
	// Some initialization functions must be run in the context
	// of a regular process (e.g., they call sleep), and thus cannot
	// be run from main().
	first = 0;
	iinit(ROOTDEV);
	initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
//	struct proc *p = myproc();
  struct thread *t = mythread();
  
  if(t == 0)
	panic("sleep");

  if(lk == 0)
	panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
	    if(!holding(&ptable.lock)) {
      acquire(&ptable.lock);
    }  //DOC: sleeplock1
	release(lk);
  }
  // Go to sleep.
  t->chan = chan;
  t->state = SLEEPING;

  sched();

  // Tidy up.
  t->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
	release(&ptable.lock);
	acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;
  struct thread *t;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
//	if(p->state == INUSED){
	  for(t = p->pthreads; t < &p->pthreads[NTHREAD] ; t++){
		if(t->state == SLEEPING && t->chan == chan){
		  t->state = RUNNABLE;
		}
	  }
//	}
  }
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
      if(!holding(&ptable.lock)) {
      acquire(&ptable.lock);
    }
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid) {
  struct proc *p;
  int i;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      // Wake process from sleep if necessary.
      for(i = 0; i< NTHREAD; i++)
      {
        p->pthreads[i].killed = 1;
        if(p->pthreads[i].state == SLEEPING)
          p->pthreads[i].state = RUNNABLE;
      }
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void) {
	static char *states[] = {
			[UNUSED] "unused",
			[EMBRYO] "embryo",
			[INUSED] "inused",
			[ZOMBIE] "zombie"};
	static char *tstates[] = {
			[T_UNUSED] "t_unused",
			[T_EMBRYO] "t_embryo",
			[SLEEPING] "sleeping",
			[RUNNABLE] "runnable",
			[RUNNING] "running",
			[T_ZOMBIE] "t_zombie"};
	int i;
	struct proc *p;
	struct thread *t;
	char *state;
	uint pc[10];

	for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
		if (p->state == UNUSED)
			continue;
		if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
			state = states[p->state];
		else
			state = "???";
		cprintf("%d %s %s", p->pid, state, p->name);

		for (t = p->pthreads; t < &p->pthreads[NTHREAD]; t++) {
			if (t->state == T_UNUSED)
				continue;
			if (p->state >= 0 && p->state < NELEM(states) && tstates[p->state])
				state = tstates[p->state];
			else
				state = "???";
			cprintf("%d %s %s", p->pid, state, p->name);
		}

		if (t->state == SLEEPING) {
			getcallerpcs((uint *) t->context->ebp + 2, pc);
			for (i = 0; i < 10 && pc[i] != 0; i++)
				cprintf(" %p", pc[i]);
		}
		cprintf("\n");
	}
}

int kthread_create(void (*start_func)(), void* stack) {
  struct proc *curproc = myproc();
  struct thread *t;
  char * sp;
      if(!holding(&ptable.lock)) {
      acquire(&ptable.lock);
    }
  t = searchThreadByStatus(myproc(), T_UNUSED);
      if(t == 0){
          release(&ptable.lock);
          return -1;
      }
      t->killed = 0;
//  t->state = T_EMBRYO;
  t->tid = nexttid++;
  t->proc = curproc;
  release(&ptable.lock);

  // Allocate kernel stack.
  if ((t->kstack = kalloc()) == 0) {
    t->state = T_UNUSED;
    return -1;
  }
  sp = t->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *t->tf;
  t->tf = (struct trapframe *)sp;

  // Set up new context to start executing at start_func,
  // which returns to trapret.
  sp -= 4;
  *(uint *) sp = (uint) trapret;

  sp -= sizeof *t->context;
  t->context = (struct context *) sp;
  memset(t->context, 0, sizeof *t->context);
  t->context->eip = (uint)forkret;

  struct thread *currthread = mythread();
  *t->tf = *currthread->tf;

  t->tf->esp = (uint)stack;
  t->tf->eip = (uint)start_func; // beginning of initcode.S
  if(!holding(&ptable.lock)) {
      acquire(&ptable.lock);
  }
  t->state = RUNNABLE;
  release(&ptable.lock);

  return t->tid;
}

int kthread_id() {
  return mythread()->tid;
}

void kthread_exit() {
  struct proc *curproc = myproc();
  struct thread *curthread = mythread();
  struct proc *p;
  int fd;
  struct thread *t;

  acquire(&ptable.lock);

  int allZombies = 1;
  for (int i =0; i<NTHREAD; i++){
    t = &curproc->pthreads[i];
    if(t->tid !=mythread()->tid &&( t->state != T_ZOMBIE && t->state != T_UNUSED))
      allZombies = 0;
  }

  if(allZombies)//this is the last thread to exit!
  {
    release(&ptable.lock);
    // Close all open files.
    for(fd = 0; fd < NOFILE; fd++){ //must do this part without locking!
      if(curproc->ofile[fd]){
        fileclose(curproc->ofile[fd]);
        curproc->ofile[fd] = 0;
      }
    }

    begin_op();
    iput(curproc->cwd);
    end_op();
    curproc->cwd = 0;
    // Parent might be sleeping in wait().
    acquire(&ptable.lock);

    wakeup1(curproc->parent);
    // Pass abandoned children to init.
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent == curproc){
        p->parent = initproc;
        if(p->state == ZOMBIE)
          wakeup1(initproc);
      }
    }
  }

  //curthread->killed = 1; no need to change beacuse we change it only in kill
  curthread->state = T_ZOMBIE;
  if(allZombies == 1) // if the proccess need to die
    curproc->state = ZOMBIE;
  wakeup1(curthread);
  sched();
  panic("zombie (exit)");

}

int kthread_join(int thread_id) {
  struct proc * currProc = myproc();
  struct thread *t;

  if(thread_id == mythread()->tid) {
    return -1;
  }

      if(!holding(&ptable.lock)) {
      acquire(&ptable.lock);
    }

  int found = 0;
  for (t = currProc->pthreads; t < &currProc->pthreads[NTHREAD]; t++) {
    if (t->tid == thread_id) {
      found = 1;
      break;
    }
  }

  if (found == 0) {
    release(&ptable.lock);
    return -1;
  }

  while ((t->state != T_ZOMBIE) && (t->state != T_UNUSED)) {
    sleep(t, &ptable.lock);
    if (currProc->killed != 0) {
      release(&ptable.lock);
      return -1;
    }
  }

  if (t->state == T_ZOMBIE) {
      t->tid = 0;
      t->state = UNUSED;
      kfree(t->kstack);
      t->kstack = 0;
      t->killed = 0;
  }
  release(&ptable.lock);

  return 0;
}

int kthread_mutex_alloc(){
	struct kthread_mutex_t *mut;
	acquire(&mtable.lock);
	for (mut = mtable.mutex_arr; mut < &mtable.mutex_arr[MAX_MUTEXES]; mut++)
		if (mut->state == M_UNUSED)
			goto found;

	release(&mtable.lock);
	return -1;

	found:
	mut->state = M_INUSE;
	mut->locked = 0;
	mut->mid = nextmid++;
//	mut->thread = mythread();

	release(&mtable.lock);

	return mut->mid;
}

int kthread_mutex_dealloc(int mutex_id){
	struct kthread_mutex_t *mut;

	acquire(&mtable.lock);

	for (mut = mtable.mutex_arr; mut < &mtable.mutex_arr[MAX_MUTEXES]; mut++){
		if (mut->mid == mutex_id){
			if(mut->locked || mut->state == M_UNUSED || mut->thread != 0){
                release(&mtable.lock);					// dealloc failed
                return -1;
			}



//		    if(!(mut->locked) && mut->state != M_UNUSED && mut->thread == 0){			//we can dealloc the mutex
				mut->state = M_UNUSED;
				mut->thread = 0;
				mut->mid = 0;
				mut->locked = 0;
				release(&mtable.lock);
				return 0;
//			}
		}
	}
	release(&mtable.lock);					// dealloc failed
	return -1;
}

int kthread_mutex_lock(int mutex_id){
	struct kthread_mutex_t *mut;
	struct thread *currThread = mythread();

	acquire(&mtable.lock);


	for (mut = mtable.mutex_arr ; mut < &mtable.mutex_arr[MAX_MUTEXES]; mut++) {
        if (mut->mid == mutex_id)
			goto found;
	}
	release(&mtable.lock);							// not found
	return -1;

	found:

	if (mut->state == M_UNUSED){				// the mutex is unused, failed.
		release(&mtable.lock);
		return -1;
	}

    while (mut->locked) {
        sleep(mut->thread, &mtable.lock);
    }
    mut->locked = 1;
    mut->thread = currThread;

	release(&mtable.lock);
	return 0;
}

int kthread_mutex_unlock(int mutex_id){
	struct kthread_mutex_t *mut;
	//struct thread *currThread = mythread();

	acquire(&mtable.lock);

	for (mut = mtable.mutex_arr ; mut < &mtable.mutex_arr[MAX_MUTEXES]; mut++) {
         if (mut->mid == mutex_id)
			goto found;
	}
	release(&mtable.lock);							// not found
	return -1;

	found:

	if (mut->state == M_UNUSED || !(mut->locked)){				// the mutex is unused or unlocked, failed.
		release(&mtable.lock);
		return -1;
	}

	if(mut->locked){
        if(mut->thread == mythread()){			// the calling thread is the owner thread
            mut->locked = 0;
            wakeup(mut->thread);
            mut->thread = 0;


            release(&mtable.lock);
            return 0;
        }
	}

	release(&mtable.lock);
	return -1;
}
