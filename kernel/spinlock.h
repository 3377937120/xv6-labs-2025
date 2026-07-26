// Mutual exclusion lock.
struct spinlock {
  uint locked;       // Is the lock held?

  // For debugging:
  char *name;        // Name of lock.
  struct cpu *cpu;   // The cpu holding the lock.
#ifdef LAB_LOCK
  int nts;
  int n;
#endif
};

#ifdef LAB_LOCK
// Reader-writer lock.
struct rwspinlock {
  struct spinlock guard; // protects the fields below
  int readers;           // active readers
  int writer;            // 0 or 1
  int waiting_writers;   // registered but not yet active writers
};
#endif
