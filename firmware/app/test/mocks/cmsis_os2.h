/*
 * Minimal host-side mock of CMSIS-RTOS v2 — only what Protocol.c / Ramps.h need
 * to compile for native unit tests. Threads/flags/mutexes are no-ops; tests
 * drive the protocol directly via ProtocolProcessLine()/ProtocolFeedByte().
 */
#ifndef MOCK_CMSIS_OS2_H
#define MOCK_CMSIS_OS2_H

#include <stdint.h>

typedef void *osThreadId_t;
typedef void *osMutexId_t;
typedef void *osSemaphoreId_t;
typedef void (*osThreadFunc_t)(void *argument);
typedef enum { osPriorityNone = 0, osPriorityLow = 8, osPriorityNormal = 24 } osPriority_t;
typedef enum { osOK = 0 } osStatus_t;

typedef struct {
  const char  *name;
  uint32_t     attr_bits;
  void        *cb_mem;
  uint32_t     cb_size;
  void        *stack_mem;
  uint32_t     stack_size;
  osPriority_t priority;
  uint32_t     tz_module;
  uint32_t     reserved;
} osThreadAttr_t;

typedef struct {
  const char *name;
  uint32_t    attr_bits;
  void       *cb_mem;
  uint32_t    cb_size;
} osMutexAttr_t;

#define osFlagsWaitAny 0x00000000U
#define osWaitForever  0xFFFFFFFFU
#define osMutexRecursive   0x00000001U
#define osMutexPrioInherit 0x00000002U

static inline osThreadId_t osThreadNew(osThreadFunc_t f, void *a, const osThreadAttr_t *at) {
  (void)f; (void)a; (void)at; return (osThreadId_t)1;
}
static inline uint32_t osThreadFlagsWait(uint32_t f, uint32_t o, uint32_t t) { (void)f; (void)o; (void)t; return 0; }
static inline uint32_t osThreadFlagsSet(osThreadId_t id, uint32_t f)         { (void)id; (void)f; return 0; }
static inline uint32_t osDelay(uint32_t ticks)                                { (void)ticks; return 0; }
static inline uint32_t osKernelGetTickCount(void)                             { return 0; }

static inline osMutexId_t osMutexNew(const osMutexAttr_t *attr) { (void)attr; return (osMutexId_t)1; }
static inline osStatus_t  osMutexAcquire(osMutexId_t m, uint32_t t) { (void)m; (void)t; return osOK; }
static inline osStatus_t  osMutexRelease(osMutexId_t m)             { (void)m; return osOK; }

#endif /* MOCK_CMSIS_OS2_H */
